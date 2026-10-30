#include "Source/Agent/AgentRuntime.h"
#include "Source/Agent/AsyncAgentRuntime.hpp"
#include "Source/Core/AgentRuntimePort.h"
#include "Source/Core/AsyncAgentRuntimePort.hpp"
#include "Source/Providers/Contracts/AsyncModelProviderPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace Asio = boost::asio;
namespace Providers = EmulativeDialog::Providers;
namespace Core = EmulativeDialog::Core;

using AsyncProviderStreamSignature = Asio::awaitable<Providers::CanonicalTerminalEvent> (
    Providers::AsyncModelProviderPort::*)(
    const Providers::ModelProviderProfile&,
    const Providers::CredentialLease&,
    const Providers::CanonicalModelRequest&,
    std::stop_token,
    const Providers::EventSink&);
using SyncProviderStreamSignature = Providers::CanonicalTerminalEvent (
    Providers::ModelProviderPort::*)(
    const Providers::ModelProviderProfile&,
    const Providers::CredentialLease&,
    const Providers::CanonicalModelRequest&,
    std::stop_token,
    const Providers::EventSink&);
using AsyncRuntimeSignature = Asio::awaitable<Core::AgentRunResult> (
    Core::AsyncAgentRuntimePort::*)(
    const Core::AuthorizedTurnContext&,
    Providers::AsyncModelProviderPort&,
    const Providers::ModelProviderProfile&,
    const Providers::CredentialLease&,
    std::stop_token);
using SyncRuntimeSignature = Core::AgentRunResult (
    Core::AgentRuntimePort::*)(
    const Core::AuthorizedTurnContext&,
    Providers::ModelProviderPort&,
    const Providers::ModelProviderProfile&,
    const Providers::CredentialLease&,
    std::stop_token);

static_assert(std::is_same_v<
              decltype(&Providers::AsyncModelProviderPort::StreamTurnAsync),
              AsyncProviderStreamSignature>);
static_assert(std::is_same_v<
              decltype(&Providers::ModelProviderPort::StreamTurn),
              SyncProviderStreamSignature>);
static_assert(std::is_same_v<
              decltype(&Core::AsyncAgentRuntimePort::RunTurnAsync),
              AsyncRuntimeSignature>);
static_assert(std::is_same_v<
              decltype(&Core::AgentRuntimePort::RunTurn),
              SyncRuntimeSignature>);
static_assert(std::is_base_of_v<
              Core::AsyncAgentRuntimePort,
              EmulativeDialog::Agent::AsyncAgentRuntime>);

class TestCredentialLease final : public Providers::CredentialLease {
public:
  [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept override {
    return "test-only-secret";
  }
};

enum class FakeStreamMode : std::uint8_t {
  Complete,
  UnknownAfterDispatch
};

class AsyncFakeProvider final : public Providers::AsyncModelProviderPort {
public:
  explicit AsyncFakeProvider(FakeStreamMode StreamMode = FakeStreamMode::Complete)
      : StreamMode(StreamMode) {}

  [[nodiscard]] Asio::awaitable<
      Providers::ProviderResult<Providers::ProviderCapabilities>>
  GetCapabilitiesAsync(
      const Providers::ProviderProfileId& ProfileId) const override {
    if (ProfileId.Value.empty()) {
      co_return Providers::ProviderResult<Providers::ProviderCapabilities>{
          Providers::ProviderError{
              Providers::FailureCode::InvalidProfile,
              false,
              "profile id is empty"}};
    }
    co_return Providers::ProviderResult<Providers::ProviderCapabilities>{
        Capabilities};
  }

  [[nodiscard]] Asio::awaitable<
      Providers::ProviderResult<std::monostate>>
  ValidateProfileAsync(
      const Providers::ModelProviderProfile& Profile) const override {
    if (Profile.Id.Value.empty()) {
      co_return Providers::ProviderResult<std::monostate>{
          Providers::ProviderError{
              Providers::FailureCode::InvalidProfile,
              false,
              "profile id is empty"}};
    }
    co_return Providers::ProviderResult<std::monostate>{std::monostate{}};
  }

  [[nodiscard]] Asio::awaitable<Providers::CanonicalTerminalEvent>
  StreamTurnAsync(
      const Providers::ModelProviderProfile& Profile,
      const Providers::CredentialLease& Lease,
      const Providers::CanonicalModelRequest& Request,
      std::stop_token CancellationToken,
      const Providers::EventSink& Sink) override {
    const auto Executor = co_await Asio::this_coro::executor;

    if (CancellationToken.stop_requested()) {
      Providers::ResponseCancelledEvent Cancelled;
      Cancelled.Metadata = {Request.Id, Request.TurnId, 1};
      Cancelled.Reason.Code.Value = "cancelled-before-dispatch";
      co_return Providers::CanonicalTerminalEvent{std::move(Cancelled)};
    }

    if (Profile.Id.Value.empty() || Lease.GetAuthenticationSecret().empty()) {
      Providers::ResponseFailedEvent Failed;
      Failed.Metadata = {Request.Id, Request.TurnId, 1};
      Failed.Failure = {
          Providers::FailureCode::InvalidRequest,
          false,
          "invalid in-memory fixture"};
      co_return Providers::CanonicalTerminalEvent{std::move(Failed)};
    }

    ++DispatchCount;
    if (OnDispatched) {
      OnDispatched();
    }

    Asio::steady_timer Suspension(Executor);
    Suspension.expires_after(std::chrono::milliseconds(1));
    co_await Suspension.async_wait(Asio::use_awaitable);

    if (CancellationToken.stop_requested() ||
        StreamMode == FakeStreamMode::UnknownAfterDispatch) {
      Providers::ResponseOutcomeUnknownEvent Unknown;
      Unknown.Metadata = {Request.Id, Request.TurnId, 1};
      Unknown.Reason.Code.Value = "request-may-have-been-sent";
      co_return Providers::CanonicalTerminalEvent{std::move(Unknown)};
    }

    Providers::ResponseStartedEvent Started;
    Started.Metadata = {Request.Id, Request.TurnId, 1};
    Started.ProviderResponse = Providers::ProviderResponseId{"fake-response"};
    co_await Asio::post(Executor, Asio::use_awaitable);
    Sink(Providers::CanonicalNonTerminalEvent{std::move(Started)});

    Providers::TextDeltaEvent Delta;
    Delta.Metadata = {Request.Id, Request.TurnId, 2};
    Delta.Item = Providers::ItemId{"fake-item"};
    Delta.Utf8Delta = "awaitable reply";
    co_await Asio::post(Executor, Asio::use_awaitable);
    Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});

    Providers::ResponseCompletedEvent Completed;
    Completed.Metadata = {Request.Id, Request.TurnId, 3};
    Completed.Kind = Providers::CompletionKind::FinalAnswer;
    co_return Providers::CanonicalTerminalEvent{std::move(Completed)};
  }

  [[nodiscard]] Asio::awaitable<
      Providers::ProviderResult<Providers::ProviderHealthStatus>>
  HealthCheckAsync(
      const Providers::ModelProviderProfile&) override {
    co_return Providers::ProviderResult<Providers::ProviderHealthStatus>{
        Providers::ProviderHealthStatus{
            Providers::ProviderHealthState::Available}};
  }

  FakeStreamMode StreamMode;
  std::function<void()> OnDispatched;
  std::uint32_t DispatchCount{};
  Providers::ProviderCapabilities Capabilities{
      true, true, false, true, true, true};
};

[[nodiscard]] bool Check(bool Condition, std::string_view Label) {
  if (!Condition) {
    std::cerr << "FAIL: " << Label << '\n';
  }
  return Condition;
}

[[nodiscard]] Providers::CanonicalModelRequest MakeRequest() {
  Providers::CanonicalModelRequest Request;
  Request.Id.Value = "test-request";
  Request.TurnId.Value = "test-turn";
  Request.ModelName = "fixture-model";
  return Request;
}

[[nodiscard]] Asio::awaitable<int> RunContractChecks() {
  const auto Executor = co_await Asio::this_coro::executor;
  Providers::ModelProviderProfile Profile;
  Profile.Id.Value = "fixture-profile";
  Profile.ModelName = "fixture-model";
  TestCredentialLease Lease;
  auto Request = MakeRequest();
  bool Passed = true;

  AsyncFakeProvider Provider;
  const auto Capabilities =
      co_await Provider.GetCapabilitiesAsync(Profile.Id);
  const auto* CapabilityValue =
      std::get_if<Providers::ProviderCapabilities>(&Capabilities);
  Passed &= Check(CapabilityValue != nullptr &&
                      CapabilityValue->Streaming &&
                      CapabilityValue->ToolCalls,
                  "awaitable GetCapabilitiesAsync returns declared capabilities");

  const auto Validation = co_await Provider.ValidateProfileAsync(Profile);
  Passed &= Check(std::holds_alternative<std::monostate>(Validation),
                  "awaitable ValidateProfileAsync accepts the fixture profile");

  const auto Health = co_await Provider.HealthCheckAsync(Profile);
  const auto* HealthValue =
      std::get_if<Providers::ProviderHealthStatus>(&Health);
  Passed &= Check(HealthValue != nullptr &&
                      HealthValue->State ==
                          Providers::ProviderHealthState::Available,
                  "awaitable HealthCheckAsync returns its typed status");

  std::vector<std::uint64_t> Sequences;
  std::vector<std::string> Text;
  std::vector<std::thread::id> CallbackThreads;
  const auto CallerThread = std::this_thread::get_id();
  const Providers::EventSink Sink =
      [&Sequences, &Text, &CallbackThreads](
          const Providers::CanonicalNonTerminalEvent& Event) {
        Sequences.push_back(std::visit(
            [](const auto& Value) { return Value.Metadata.SequenceNumber; },
            Event));
        CallbackThreads.push_back(std::this_thread::get_id());
        if (const auto* Delta =
                std::get_if<Providers::TextDeltaEvent>(&Event)) {
          Text.push_back(Delta->Utf8Delta);
        }
      };

  const auto Terminal =
      co_await Provider.StreamTurnAsync(Profile, Lease, Request, {}, Sink);
  co_await Asio::post(Executor, Asio::use_awaitable);
  const auto* Completed =
      std::get_if<Providers::ResponseCompletedEvent>(&Terminal);
  Passed &= Check(Provider.DispatchCount == 1,
                  "provider fake dispatches the complete request once");
  Passed &= Check(Sequences == std::vector<std::uint64_t>{1, 2},
                  "nonterminal events are ordered and monotonically sequenced");
  Passed &= Check(Text == std::vector<std::string>{"awaitable reply"},
                  "text event is delivered to the consumer");
  Passed &= Check(std::all_of(
                      CallbackThreads.begin(), CallbackThreads.end(),
                      [CallerThread](std::thread::id Thread) {
                        return Thread == CallerThread;
                      }),
                  "event sink runs on the co_spawn associated executor");
  Passed &= Check(Completed != nullptr &&
                      Completed->Metadata.SequenceNumber == 3 &&
                      Completed->Kind == Providers::CompletionKind::FinalAnswer,
                  "exactly one final terminal is returned after nonterminals");
  const auto EventCountAfterCompletion = Sequences.size();
  co_await Asio::post(Executor, Asio::use_awaitable);
  Passed &= Check(Sequences.size() == EventCountAfterCompletion,
                  "no event callback occurs after awaitable completion");

  AsyncFakeProvider CancelledProvider;
  std::stop_source BeforeDispatch;
  BeforeDispatch.request_stop();
  std::vector<Providers::CanonicalNonTerminalEvent> CancelledEvents;
  const Providers::EventSink CancelledSink =
      [&CancelledEvents](const Providers::CanonicalNonTerminalEvent& Event) {
        CancelledEvents.push_back(Event);
      };
  const auto CancelledTerminal =
      co_await CancelledProvider.StreamTurnAsync(
          Profile, Lease, Request, BeforeDispatch.get_token(), CancelledSink);
  Passed &= Check(
      std::holds_alternative<Providers::ResponseCancelledEvent>(
          CancelledTerminal) &&
          CancelledProvider.DispatchCount == 0 &&
          CancelledEvents.empty(),
      "pre-dispatch cancellation returns ResponseCancelled without dispatch");

  AsyncFakeProvider UnknownProvider(FakeStreamMode::UnknownAfterDispatch);
  std::stop_source AfterDispatch;
  UnknownProvider.OnDispatched =
      [&AfterDispatch]() { AfterDispatch.request_stop(); };
  std::vector<Providers::CanonicalNonTerminalEvent> UnknownEvents;
  const Providers::EventSink UnknownSink =
      [&UnknownEvents](const Providers::CanonicalNonTerminalEvent& Event) {
        UnknownEvents.push_back(Event);
      };
  const auto UnknownTerminal =
      co_await UnknownProvider.StreamTurnAsync(
          Profile, Lease, Request, AfterDispatch.get_token(), UnknownSink);
  Passed &= Check(
      std::holds_alternative<Providers::ResponseOutcomeUnknownEvent>(
          UnknownTerminal) &&
          UnknownProvider.DispatchCount == 1 &&
          UnknownEvents.empty(),
      "post-dispatch cancellation returns UnknownOutcome without replay");
  const auto UnknownEventCountAfterCompletion = UnknownEvents.size();
  co_await Asio::post(Executor, Asio::use_awaitable);
  Passed &= Check(UnknownEvents.size() == UnknownEventCountAfterCompletion,
                  "unknown terminal also ends all callbacks");

  co_return Passed ? 0 : 1;
}

} // namespace

int main() {
  Asio::io_context Context;
  std::exception_ptr Failure;
  int Result = 1;
  Asio::co_spawn(
      Context,
      RunContractChecks(),
      [&Failure, &Result](std::exception_ptr Exception, int Value) {
        Failure = std::move(Exception);
        Result = Value;
      });
  Context.run();

  if (Failure) {
    try {
      std::rethrow_exception(Failure);
    } catch (const std::exception& Exception) {
      std::cerr << "uncaught contract-test exception: "
                << Exception.what() << '\n';
    } catch (...) {
      std::cerr << "uncaught contract-test exception\n";
    }
    return 2;
  }
  if (Result != 0) {
    return Result;
  }
  std::cout << "Async provider contract checks passed\n";
  return 0;
}