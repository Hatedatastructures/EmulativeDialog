#include "Source/Agent/AsyncAgentRuntime.hpp"
#include "Source/Agent/OutputBudgetController.hpp"
#include "Source/Core/CoreChatService.h"
#include "Source/Core/IdentityPolicy.h"
#include "Tests/Contract/Channels/Contracts/MockChannelAdapter.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

void RunLegacyToolCallRoundTripTests();

namespace {

namespace Asio = boost::asio;
namespace Channels = EmulativeDialog::Channels;
namespace Core = EmulativeDialog::Core;
namespace Providers = EmulativeDialog::Providers;
namespace Testing = EmulativeDialog::Channels::Testing;

enum class FakeScenario : std::uint8_t {
  Final,
  Refusal,
  ToolHandoff,
  ToolCall,
  OutputLimited,
  Failed,
  Cancelled,
  Unknown,
  InvalidSequence,
  ThrowAfterDispatch,
  ProfileRejected,
  CapabilitiesRejected,
  UnsupportedCapabilities,
  ToolRoundTrip,
  ToolRoundTripBadArguments,
  ToolRoundTripUnknownSecond,
  ToolHandoffLoop,
  ToolHandoffTwoCallsLoop,
  ToolCallOverflow,
  ToolCallDuplicate,
  ToolProposalFinal,
};

class StreamActivity final {
public:
  StreamActivity(std::atomic<std::uint32_t>& Active,
                 std::atomic<std::uint32_t>& Maximum) noexcept
      : Active(Active), Maximum(Maximum) {
    const std::uint32_t Current = Active.fetch_add(1) + 1;
    std::uint32_t Observed = Maximum.load();
    while (Observed < Current &&
           !Maximum.compare_exchange_weak(Observed, Current)) {
    }
  }

  ~StreamActivity() { Active.fetch_sub(1); }
  StreamActivity(const StreamActivity&) = delete;
  StreamActivity& operator=(const StreamActivity&) = delete;

private:
  std::atomic<std::uint32_t>& Active;
  std::atomic<std::uint32_t>& Maximum;
};

class TestChannelCredential final : public Core::CredentialLease {
public:
  [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept override {
    return "channel-test-lease";
  }
};

class TestProviderCredential final : public Providers::CredentialLease {
public:
  [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept override {
    return "provider-test-lease";
  }
};

class SyncRuntimeStub final : public Core::AgentRuntimePort {
public:
  [[nodiscard]] Core::AgentRunResult RunTurn(
      const Core::AuthorizedTurnContext&,
      Providers::ModelProviderPort&,
      const Providers::ModelProviderProfile&,
      const Providers::CredentialLease&,
      std::stop_token) override {
    ++Calls;
    if (OnRun) {
      OnRun();
    }
    return Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "sync compatibility response");
  }

  std::atomic<std::uint32_t> Calls{0};
  std::function<void()> OnRun;
};

class SyncProviderStub final : public Providers::ModelProviderPort {
public:
  [[nodiscard]] Providers::ProviderResult<Providers::ProviderCapabilities>
  GetCapabilities(const Providers::ProviderProfileId&) const override {
    return Providers::ProviderError{
        Providers::FailureCode::ProviderRejected, false, "sync stub unused"};
  }

  [[nodiscard]] Providers::ProviderResult<std::monostate> ValidateProfile(
      const Providers::ModelProviderProfile&) const override {
    return Providers::ProviderError{
        Providers::FailureCode::ProviderRejected, false, "sync stub unused"};
  }

  [[nodiscard]] Providers::CanonicalTerminalEvent StreamTurn(
      const Providers::ModelProviderProfile&,
      const Providers::CredentialLease&,
      const Providers::CanonicalModelRequest& Request,
      std::stop_token,
      const Providers::EventSink&) override {
    Providers::ResponseFailedEvent Failed;
    Failed.Metadata = {Request.Id, Request.TurnId, 1};
    Failed.Failure = {Providers::FailureCode::ProviderRejected, false,
                      "sync stub unused"};
    return Failed;
  }

  [[nodiscard]] Providers::ProviderResult<Providers::ProviderHealthStatus>
  HealthCheck(const Providers::ModelProviderProfile&) override {
    return Providers::ProviderError{
        Providers::FailureCode::ProviderRejected, false, "sync stub unused"};
  }
};

class AsyncFakeProvider final : public Providers::AsyncModelProviderPort {
public:
  explicit AsyncFakeProvider(FakeScenario Scenario = FakeScenario::Final)
      : Scenario(Scenario) {}

  [[nodiscard]] Asio::awaitable<
      Providers::ProviderResult<Providers::ProviderCapabilities>>
  GetCapabilitiesAsync(
      const Providers::ProviderProfileId& ProfileId) const override {
    if (Scenario == FakeScenario::CapabilitiesRejected) {
      co_return Providers::ProviderError{
          Providers::FailureCode::UnsupportedCapability, false,
          "fake capabilities rejected"};
    }
    if (ProfileId.Value.empty()) {
      co_return Providers::ProviderError{
          Providers::FailureCode::InvalidProfile, false,
          "fake profile id is empty"};
    }
    Providers::ProviderCapabilities Result = Capabilities;
    if (Scenario == FakeScenario::UnsupportedCapabilities) {
      Result.Streaming = false;
    }
    co_return Result;
  }

  [[nodiscard]] Asio::awaitable<
      Providers::ProviderResult<std::monostate>>
  ValidateProfileAsync(
      const Providers::ModelProviderProfile& Profile) const override {
    if (Scenario == FakeScenario::ProfileRejected) {
      co_return Providers::ProviderError{
          Providers::FailureCode::InvalidProfile, false,
          "fake profile rejected"};
    }
    if (Profile.Id.Value.empty() || Profile.ModelName.empty()) {
      co_return Providers::ProviderError{
          Providers::FailureCode::InvalidProfile, false,
          "fake profile is incomplete"};
    }
    co_return std::monostate{};
  }

  [[nodiscard]] Asio::awaitable<Providers::CanonicalTerminalEvent>
  StreamTurnAsync(const Providers::ModelProviderProfile& Profile,
                  const Providers::CredentialLease& Lease,
                  const Providers::CanonicalModelRequest& Request,
                  std::stop_token CancellationToken,
                  const Providers::EventSink& Sink) override {
    const auto Executor = co_await Asio::this_coro::executor;
    if (CancellationToken.stop_requested()) {
      Providers::ResponseCancelledEvent Cancelled;
      Cancelled.Metadata = {Request.Id, Request.TurnId, 1};
      Cancelled.Reason.Code.Value = "cancelled-before-dispatch";
      co_return Cancelled;
    }
    const bool ToolFlow =
        Scenario == FakeScenario::ToolRoundTrip ||
        Scenario == FakeScenario::ToolRoundTripBadArguments ||
        Scenario == FakeScenario::ToolRoundTripUnknownSecond ||
        Scenario == FakeScenario::ToolHandoffLoop ||
        Scenario == FakeScenario::ToolHandoffTwoCallsLoop ||
        Scenario == FakeScenario::ToolCallOverflow ||
        Scenario == FakeScenario::ToolCallDuplicate ||
        Scenario == FakeScenario::ToolProposalFinal;
    if (Profile.Id.Value.empty() || Profile.ModelName.empty() ||
        Lease.GetAuthenticationSecret().empty() || Request.Messages.empty() ||
        (!ToolFlow && Request.Messages.size() != 1U)) {
      Providers::ResponseFailedEvent Failed;
      Failed.Metadata = {Request.Id, Request.TurnId, 1};
      Failed.Failure = {Providers::FailureCode::InvalidRequest, false,
                        "invalid async fake fixture"};
      co_return Failed;
    }

    const StreamActivity Activity{ActiveStreamCount, MaxActiveStreamCount};
    const std::uint32_t RequestNumber = DispatchCount.fetch_add(1) + 1U;
    if (OnDispatched) {
      OnDispatched();
    }
    {
      std::lock_guard Lock(RequestMutex);
      LastRequestText.clear();
      Requests.push_back(Request);
      for (const Providers::ContentPart& Part : Request.Messages.front().ContentParts) {
        if (const auto* Text = std::get_if<Providers::TextContentPart>(&Part)) {
          LastRequestText += Text->Text;
        }
      }
    }

    Asio::steady_timer Suspension(Executor);
    Suspension.expires_after(StreamDelay);
    AwaitingProvider.store(true);
    co_await Suspension.async_wait(Asio::use_awaitable);
    AwaitingProvider.store(false);

    if (CustomEventScript && CustomTerminalScript) {
      CustomEventScript(Request, CancellationToken, Sink);
      if (CustomTerminalDelay > std::chrono::milliseconds::zero()) {
        AwaitingProvider.store(true);
        Asio::steady_timer TerminalDelay(Executor);
        TerminalDelay.expires_after(CustomTerminalDelay);
        co_await TerminalDelay.async_wait(Asio::use_awaitable);
        AwaitingProvider.store(false);
      }
      if (BeforeCustomTerminal) {
        BeforeCustomTerminal();
      }
      Providers::CanonicalTerminalEvent Terminal =
          CustomTerminalScript(Request);
      ++FinalTerminalCount;
      co_return Terminal;
    }

    const auto SendFinalUsage = [&Sink, &Request]() {
      Providers::UsageUpdatedEvent Usage;
      Usage.Metadata = {Request.Id, Request.TurnId, 2};
      Usage.Usage.InputTokens = 5;
      Usage.Usage.OutputTokens = 2;
      Usage.Usage.TotalTokens = 7;
      Usage.Usage.Completeness = Providers::UsageCompleteness::Final;
      Sink(Providers::CanonicalNonTerminalEvent{std::move(Usage)});
    };
    const auto MakeUsage = []() {
      Providers::UsageSnapshot Usage;
      Usage.InputTokens = 5;
      Usage.OutputTokens = 2;
      Usage.TotalTokens = 7;
      Usage.Completeness = Providers::UsageCompleteness::Final;
      return Usage;
    };
    const auto MakeHandoff = [&Request, &MakeUsage]() {
      Providers::ResponseCompletedEvent Handoff;
      Handoff.Metadata = {Request.Id, Request.TurnId, 3};
      Handoff.Kind = Providers::CompletionKind::ToolHandoff;
      Handoff.FinalUsage = MakeUsage();
      return Providers::CanonicalTerminalEvent{std::move(Handoff)};
    };

    if (ToolFlow) {
      if (Scenario == FakeScenario::ToolRoundTripUnknownSecond &&
          RequestNumber > 1U) {
        Providers::ResponseOutcomeUnknownEvent Unknown;
        Unknown.Metadata = {Request.Id, Request.TurnId, 1};
        Unknown.Reason.Code.Value = "unknown-after-read-only-tool";
        co_return Unknown;
      }

      const bool AlwaysHandoff =
          Scenario == FakeScenario::ToolHandoffLoop ||
          Scenario == FakeScenario::ToolHandoffTwoCallsLoop ||
          Scenario == FakeScenario::ToolCallOverflow ||
          Scenario == FakeScenario::ToolCallDuplicate;
      const bool FirstHandoff = RequestNumber == 1U;
      if (AlwaysHandoff || FirstHandoff) {
        std::uint32_t CallCount = 1U;
        if (Scenario == FakeScenario::ToolCallOverflow) {
          CallCount = 9U;
        } else if (Scenario == FakeScenario::ToolHandoffTwoCallsLoop) {
          CallCount = 2U;
        } else if (Scenario == FakeScenario::ToolCallDuplicate) {
          CallCount = 2U;
        }
        for (std::uint32_t Index = 0; Index < CallCount; ++Index) {
          Providers::ToolCallReadyEvent Tool;
          Tool.Metadata = {Request.Id, Request.TurnId,
                           static_cast<std::uint64_t>(Index + 1U)};
          Tool.Item = Providers::ItemId{"fake-tool-item"};
          std::string ToolCallId;
          if (Scenario == FakeScenario::ToolCallDuplicate) {
            ToolCallId = "duplicate-call";
          } else if (Scenario == FakeScenario::ToolHandoffLoop ||
                     Scenario == FakeScenario::ToolHandoffTwoCallsLoop) {
            ToolCallId = "loop-call-" + std::to_string(RequestNumber) +
                         "-" + std::to_string(Index + 1U);
          } else {
            ToolCallId = "fake-tool-call-" + std::to_string(Index + 1U);
          }
          Tool.Call = Providers::ToolCallId{std::move(ToolCallId)};
          Tool.ToolName = "LookupProfile";
          Tool.Arguments = Providers::JsonValue{
              Scenario == FakeScenario::ToolRoundTripBadArguments
                  ? R"({"query":7})"
                  : R"({"query":"profile"})"};
          Sink(Providers::CanonicalNonTerminalEvent{std::move(Tool)});
        }
        Providers::UsageUpdatedEvent Usage;
        Usage.Metadata = {Request.Id, Request.TurnId,
                          static_cast<std::uint64_t>(CallCount + 1U)};
        Usage.Usage = MakeUsage();
        Sink(Providers::CanonicalNonTerminalEvent{std::move(Usage)});
        if (Scenario == FakeScenario::ToolProposalFinal) {
          Providers::ResponseCompletedEvent Final;
          Final.Metadata = {Request.Id, Request.TurnId,
                            static_cast<std::uint64_t>(CallCount + 2U)};
          Final.Kind = Providers::CompletionKind::FinalAnswer;
          Final.FinalUsage = MakeUsage();
          co_return Final;
        }
        auto Terminal = MakeHandoff();
        std::visit(
            [&CallCount](auto& Value) {
              using ValueType = std::decay_t<decltype(Value)>;
              if constexpr (std::is_same_v<
                                ValueType,
                                Providers::ResponseCompletedEvent>) {
                Value.Metadata.SequenceNumber =
                    static_cast<std::uint64_t>(CallCount + 2U);
              }
            },
            Terminal);
        co_return Terminal;
      }

      Providers::TextDeltaEvent Delta;
      Delta.Metadata = {Request.Id, Request.TurnId, 1};
      Delta.Item = Providers::ItemId{"tool-roundtrip-final"};
      Delta.Utf8Delta = "profile result is ready";
      Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});
      SendFinalUsage();
      Providers::ResponseCompletedEvent Final;
      Final.Metadata = {Request.Id, Request.TurnId, 3};
      Final.Kind = Providers::CompletionKind::FinalAnswer;
      Final.FinalUsage = MakeUsage();
      ++FinalTerminalCount;
      co_return Final;
    }

    if (Scenario == FakeScenario::ThrowAfterDispatch) {
      throw std::runtime_error("synthetic unknown provider outcome");
    }

    if (Scenario == FakeScenario::OutputLimited) {
      Providers::ResponseOutputLimitedEvent Limited;
      Limited.Metadata = {Request.Id, Request.TurnId, 1};
      Limited.Kind = Providers::LimitKind::OutputTokens;
      Limited.Usage.Completeness = Providers::UsageCompleteness::Final;
      Limited.Usage.OutputTokens = 64;
      co_return Limited;
    }
    if (Scenario == FakeScenario::Failed) {
      Providers::ResponseFailedEvent Failed;
      Failed.Metadata = {Request.Id, Request.TurnId, 1};
      Failed.Failure = {Providers::FailureCode::ProviderRejected, false,
                        "synthetic provider rejection"};
      co_return Failed;
    }
    if (Scenario == FakeScenario::Cancelled) {
      Providers::ResponseCancelledEvent Cancelled;
      Cancelled.Metadata = {Request.Id, Request.TurnId, 1};
      Cancelled.Reason.Code.Value = "synthetic-cancel";
      co_return Cancelled;
    }
    if (Scenario == FakeScenario::Unknown) {
      Providers::ResponseOutcomeUnknownEvent Unknown;
      Unknown.Metadata = {Request.Id, Request.TurnId, 1};
      Unknown.Reason.Code.Value = "possibly-dispatched";
      co_return Unknown;
    }
    if (Scenario == FakeScenario::ToolHandoff) {
      Providers::ResponseCompletedEvent Handoff;
      Handoff.Metadata = {Request.Id, Request.TurnId, 1};
      Handoff.Kind = Providers::CompletionKind::ToolHandoff;
      Handoff.FinalUsage = Providers::UsageSnapshot{
          .InputTokens = 5,
          .OutputTokens = 2,
          .TotalTokens = 7,
          .CachedInputReadTokens = std::nullopt,
          .CachedInputWriteTokens = std::nullopt,
          .ReasoningOutputTokens = std::nullopt,
          .Completeness = Providers::UsageCompleteness::Final,
      };
      co_return Handoff;
    }
    if (Scenario == FakeScenario::ToolCall) {
      Providers::ToolCallReadyEvent Tool;
      Tool.Metadata = {Request.Id, Request.TurnId, 1};
      Tool.Item = Providers::ItemId{"fake-tool-item"};
      Tool.Call = Providers::ToolCallId{"fake-tool-call"};
      Tool.ToolName = "registered-action";
      Tool.Arguments = Providers::JsonValue{"{}"};
      Sink(Providers::CanonicalNonTerminalEvent{std::move(Tool)});
      Providers::ResponseCompletedEvent Handoff;
      Handoff.Metadata = {Request.Id, Request.TurnId, 2};
      Handoff.Kind = Providers::CompletionKind::ToolHandoff;
      co_return Handoff;
    }
    if (Scenario == FakeScenario::InvalidSequence) {
      Providers::TextDeltaEvent Delta;
      Delta.Metadata = {Request.Id, Request.TurnId, 2};
      Delta.Item = Providers::ItemId{"bad-sequence-item"};
      Delta.Utf8Delta = "must not send";
      Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});
      Providers::ResponseCompletedEvent Invalid;
      Invalid.Metadata = {Request.Id, Request.TurnId, 3};
      Invalid.Kind = Providers::CompletionKind::FinalAnswer;
      co_return Invalid;
    }

    const bool IsRefusal = Scenario == FakeScenario::Refusal;
    Providers::ResponseStartedEvent Started;
    Started.Metadata = {Request.Id, Request.TurnId, 1};
    Started.ProviderResponse = Providers::ProviderResponseId{"async-fake-response"};
    Sink(Providers::CanonicalNonTerminalEvent{std::move(Started)});
    co_await Asio::post(Executor, Asio::use_awaitable);

    if (IsRefusal) {
      Providers::RefusalDeltaEvent Delta;
      Delta.Metadata = {Request.Id, Request.TurnId, 2};
      Delta.Item = Providers::ItemId{"async-fake-refusal"};
      Delta.Utf8Delta = "safe refusal";
      Delta.Category = Providers::RefusalCategory::GeneralHarms;
      Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});
    } else {
      Providers::TextDeltaEvent Delta;
      Delta.Metadata = {Request.Id, Request.TurnId, 2};
      Delta.Item = Providers::ItemId{"async-fake-item"};
      Delta.Utf8Delta = "coroutine response";
      Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});
    }
    co_await Asio::post(Executor, Asio::use_awaitable);

    Providers::UsageUpdatedEvent Usage;
    Usage.Metadata = {Request.Id, Request.TurnId, 3};
    Usage.Usage.InputTokens = 5;
    Usage.Usage.OutputTokens = 2;
    Usage.Usage.TotalTokens = 7;
    Usage.Usage.Completeness = Providers::UsageCompleteness::Final;
    Sink(Providers::CanonicalNonTerminalEvent{std::move(Usage)});
    co_await Asio::post(Executor, Asio::use_awaitable);

    Providers::ResponseCompletedEvent Completed;
    Completed.Metadata = {Request.Id, Request.TurnId, 4};
    Completed.Kind = IsRefusal ? Providers::CompletionKind::Refusal
                               : Providers::CompletionKind::FinalAnswer;
    Providers::UsageSnapshot FinalUsage;
    FinalUsage.InputTokens = 5;
    FinalUsage.OutputTokens = 2;
    FinalUsage.TotalTokens = 7;
    FinalUsage.Completeness = Providers::UsageCompleteness::Final;
    Completed.FinalUsage = FinalUsage;
    if (IsRefusal) {
      Completed.StopDetails = Providers::RefusalStopDetails{
          Providers::RefusalStopDetailsType::Refusal,
          Providers::RefusalCategory::GeneralHarms};
    }
    ++FinalTerminalCount;
    co_return Completed;
  }

  [[nodiscard]] Asio::awaitable<
      Providers::ProviderResult<Providers::ProviderHealthStatus>>
  HealthCheckAsync(const Providers::ModelProviderProfile&) override {
    co_return Providers::ProviderHealthStatus{
        Providers::ProviderHealthState::Available};
  }

  [[nodiscard]] std::string GetLastRequestText() const {
    std::lock_guard Lock(RequestMutex);
    return LastRequestText;
  }

  [[nodiscard]] std::vector<Providers::CanonicalModelRequest>
  GetRequests() const {
    std::lock_guard Lock(RequestMutex);
    return Requests;
  }

  std::atomic<std::uint32_t> DispatchCount{0};
  std::atomic<std::uint32_t> ActiveStreamCount{0};
  std::atomic<std::uint32_t> MaxActiveStreamCount{0};
  std::atomic<bool> AwaitingProvider{false};
  std::atomic<std::uint32_t> FinalTerminalCount{0};
  std::function<void()> OnDispatched;
  std::function<void(const Providers::CanonicalModelRequest&,
                     std::stop_token,
                     const Providers::EventSink&)> CustomEventScript;
  std::function<Providers::CanonicalTerminalEvent(
      const Providers::CanonicalModelRequest&)> CustomTerminalScript;
  std::function<void()> BeforeCustomTerminal;
  std::chrono::milliseconds CustomTerminalDelay{};
  std::chrono::milliseconds StreamDelay{2};

private:
  FakeScenario Scenario;
  Providers::ProviderCapabilities Capabilities{
      true, true, true, true, true, true};
  mutable std::mutex RequestMutex;
  std::string LastRequestText;
  std::vector<Providers::CanonicalModelRequest> Requests;
};

class ScriptedActionIdSource final : public Core::CoreActionIdSource {
public:
  [[nodiscard]] Core::CoreActionIdentifiers Next() override {
    if (EmptyIdentifiers) {
      return {};
    }
    const std::uint64_t Sequence = DuplicateIdentifiers
                                       ? 1
                                       : NextSequence.fetch_add(1);
    return Core::CoreActionIdentifiers{
        .ActionId = Channels::ActionId{"async-action-" + std::to_string(Sequence)},
        .OperationId = Channels::OperationId{"async-operation-" + std::to_string(Sequence)},
        .IdempotencyKey = Channels::IdempotencyKey{"async-idempotency-" + std::to_string(Sequence)},
    };
  }

  std::atomic<std::uint64_t> NextSequence{1};
  bool DuplicateIdentifiers{};
  bool EmptyIdentifiers{};
};

class CollectingSink final : public Channels::ChannelEventSink {
public:
  void OnChannelMessage(const Channels::InboundChannelMessage& Message) override {
    Events.push_back(Message);
  }

  std::vector<Channels::InboundChannelMessage> Events;
};

struct SendActionObservation final {
  std::string ActionId;
  std::string OperationId;
  std::string IdempotencyKey;
  std::string TargetPrincipalId;
  std::string TargetChannelId;
  std::string TargetAccountId;
  std::string TargetConversationId;
  std::string TextPayload;
  std::string AuthorizationRefValue;
  Core::CoreAuthorizedActionKind ActionKind{
      Core::CoreAuthorizedActionKind::SendText};
  std::string ReceiptActionId;
  std::string ReceiptOperationId;
  std::string PlatformReceiptId;
  Channels::ReceiptStatus ReceiptStatus{Channels::ReceiptStatus::Failed};
  std::uint64_t PolicyVersion{};
  std::uint64_t CapabilitySnapshotVersion{};
};

class SynchronizedMockChannel final : public Channels::ChannelAdapterPort {
public:
  explicit SynchronizedMockChannel(Testing::MockChannelConfiguration Configuration)
      : Mock(std::move(Configuration)) {}

  [[nodiscard]] Channels::CapabilitySnapshot GetCapabilities(
      const Channels::AccountProfile& Profile) override {
    std::lock_guard Lock(Mutex);
    ++CapabilityCallCount;
    if (ThrowCapabilitiesAtCall != 0 &&
        CapabilityCallCount == ThrowCapabilitiesAtCall) {
      throw std::runtime_error("synthetic capability query failure");
    }
    return Mock.GetCapabilities(Profile);
  }

  [[nodiscard]] Channels::StartResult Start(
      const Channels::AccountProfile& Profile,
      const Core::CredentialLease& Lease) override {
    std::lock_guard Lock(Mutex);
    return Mock.Start(Profile, Lease);
  }

  [[nodiscard]] Channels::StopResult Stop() override {
    std::lock_guard Lock(Mutex);
    return Mock.Stop();
  }

  [[nodiscard]] Channels::SubscriptionResult SubscribeEvents(
      Channels::ChannelEventSink& Sink) override {
    std::lock_guard Lock(Mutex);
    return Mock.SubscribeEvents(Sink);
  }

  [[nodiscard]] Channels::ChannelReceipt SendAction(
      const Core::CoreAuthorizedAction& Action,
      const Core::CredentialLease& Lease) override {
    std::lock_guard Lock(Mutex);
    ++SendAttemptCount;
    if (ThrowOnSend) {
      throw std::runtime_error("synthetic uncertain channel send");
    }
    Channels::ChannelReceipt Receipt = Mock.SendAction(Action, Lease);
    if (ReceiptOverride.has_value()) {
      Receipt.Status = ReceiptOverride->first;
      Receipt.ErrorCode = ReceiptOverride->second;
    }
    if (CorruptReceipt) {
      Receipt.Operation.Value = "foreign-operation-id";
    }
    const Core::ConversationScope& Scope = Action.GetTargetScope();
    SendObservations.push_back(SendActionObservation{
        .ActionId = Action.GetActionId().Value,
        .OperationId = Action.GetOperationId().Value,
        .IdempotencyKey = Action.GetIdempotencyKey().Value,
        .TargetPrincipalId = Scope.Principal.Value,
        .TargetChannelId = Scope.Channel.Value,
        .TargetAccountId = Scope.Account.Value,
        .TargetConversationId = Scope.Conversation.Value,
        .TextPayload = std::string(Action.GetTextPayload()),
        .AuthorizationRefValue = Action.GetAuthorizationRef().Value,
        .ActionKind = Action.GetActionKind(),
        .ReceiptActionId = Receipt.Action.Value,
        .ReceiptOperationId = Receipt.Operation.Value,
        .PlatformReceiptId = Receipt.PlatformReceipt.has_value()
                                 ? Receipt.PlatformReceipt->Value
                                 : std::string{},
        .ReceiptStatus = Receipt.Status,
        .PolicyVersion = Action.GetPolicyVersion(),
        .CapabilitySnapshotVersion =
            Action.GetCapabilitySnapshotVersion(),
    });
    return Receipt;
  }

  [[nodiscard]] Channels::HealthSnapshot GetHealthSnapshot() const override {
    std::lock_guard Lock(Mutex);
    return Mock.GetHealthSnapshot();
  }

  void SetFaults(std::size_t ThrowAtCapabilityCall = 0,
                 bool ShouldThrowOnSend = false,
                 bool ShouldCorruptReceipt = false) {
    std::lock_guard Lock(Mutex);
    ThrowCapabilitiesAtCall = ThrowAtCapabilityCall;
    ThrowOnSend = ShouldThrowOnSend;
    CorruptReceipt = ShouldCorruptReceipt;
  }

  void SetReceiptOutcome(Channels::ReceiptStatus Status,
                         Channels::ChannelErrorCode ErrorCode) {
    std::lock_guard Lock(Mutex);
    ReceiptOverride = std::pair{Status, ErrorCode};
  }

  [[nodiscard]] std::size_t GetSendActionCallCount() const {
    std::lock_guard Lock(Mutex);
    return Mock.GetSendActionCallCount();
  }

  [[nodiscard]] std::size_t GetSendAttemptCount() const {
    std::lock_guard Lock(Mutex);
    return SendAttemptCount;
  }

  [[nodiscard]] std::optional<Channels::ChannelReceipt>
  GetLastSendActionReceipt() const {
    std::lock_guard Lock(Mutex);
    return Mock.GetLastSendActionReceipt();
  }

  [[nodiscard]] std::vector<SendActionObservation> GetSendObservations() const {
    std::lock_guard Lock(Mutex);
    return SendObservations;
  }

private:
  mutable std::mutex Mutex;
  Testing::MockChannelAdapter Mock;
  std::size_t CapabilityCallCount{};
  std::size_t ThrowCapabilitiesAtCall{};
  std::size_t SendAttemptCount{};
  std::vector<SendActionObservation> SendObservations;
  bool ThrowOnSend{};
  bool CorruptReceipt{};
  std::optional<std::pair<Channels::ReceiptStatus, Channels::ChannelErrorCode>>
      ReceiptOverride;
};

class CollectingOperationObserver final : public Core::OperationRecordObserver {
public:
  void OnOperationRecordChanged(const Core::OperationRecord& Record) noexcept override {
    try {
      std::lock_guard Lock(Mutex);
      Records.push_back(Record);
    } catch (...) {
    }
  }

  [[nodiscard]] std::vector<Core::OperationRecord> Snapshot() const {
    std::lock_guard Lock(Mutex);
    return Records;
  }

private:
  mutable std::mutex Mutex;
  std::vector<Core::OperationRecord> Records;
};

[[nodiscard]] Channels::AccountProfile MakeChannelProfile() {
  return Channels::AccountProfile{
      .Channel = Channels::ChannelId{"Mock"},
      .Account = Channels::AccountId{"async-account"},
      .Mode = Channels::IntegrationMode{"Mock"},
  };
}

[[nodiscard]] Channels::CapabilitySnapshot MakeCapabilities(
    const Channels::AccountProfile& Profile) {
  return Channels::CapabilitySnapshot{
      .Channel = Profile.Channel,
      .Account = Profile.Account,
      .Mode = Profile.Mode,
      .SnapshotVersion = 9,
      .CapturedAt = Channels::Timestamp{},
      .ValidUntil = std::nullopt,
      .AuthenticationStatus = Channels::VerificationStatus::Verified,
      .AuditStatus = Channels::VerificationStatus::Verified,
      .Capabilities = {
          Channels::CapabilityObservation{
              .Capability = Channels::ChannelCapability::ReceiveText,
              .Status = Channels::CapabilityStatus::Supported,
              .EvidenceStatus = Channels::VerificationStatus::Verified,
              .ObservedAt = Channels::Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = Channels::EvidenceReference{"test-receive"},
          },
          Channels::CapabilityObservation{
              .Capability = Channels::ChannelCapability::SendText,
              .Status = Channels::CapabilityStatus::Supported,
              .EvidenceStatus = Channels::VerificationStatus::Verified,
              .ObservedAt = Channels::Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = Channels::EvidenceReference{"test-send"},
          },
      },
  };
}

[[nodiscard]] Channels::InboundChannelMessage MakeInboundMessage(
    std::string Deduplication = "async-dedup-1",
    std::string MessageId = "async-message-1") {
  return Channels::InboundChannelMessage{
      .Envelope = Channels::ChannelEnvelope{
          .Channel = Channels::ChannelId{"Mock"},
          .Mode = Channels::IntegrationMode{"Mock"},
          .Account = Channels::AccountId{"async-account"},
          .Conversation = Channels::ConversationId{"async-conversation"},
          .ConversationType = Channels::ConversationKind::Private,
          .Sender = Channels::SenderId{"async-sender"},
          .Message = Channels::MessageId{std::move(MessageId)},
          .PlatformEvent = Channels::PlatformEventId{"async-event"},
          .OccurredAt = Channels::Timestamp{},
          .StableDeduplication =
              Channels::StableDeduplicationId{std::move(Deduplication)},
      },
      .Message = Channels::CanonicalMessage{
          .MessageParts = {Channels::TextPart{"hello from mock channel"}},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      },
  };
}

[[nodiscard]] Providers::ModelProviderProfile MakeProviderProfile(
    FakeScenario Scenario = FakeScenario::Final) {
  Providers::ModelProviderProfile Profile;
  Profile.Id = Providers::ProviderProfileId{"async-fake-profile"};
  Profile.Protocol = Scenario == FakeScenario::Refusal
                         ? Providers::ProviderProtocol::AnthropicMessages
                         : Providers::ProviderProtocol::OpenAIResponses;
  Profile.Endpoint = "https://fake.invalid";
  Profile.ModelName = "async-fake-model";
  Profile.Credential = Providers::CredentialRef{"in-memory-test-credential"};
  if (Scenario == FakeScenario::Refusal) {
    Profile.Authentication = Providers::AuthenticationScheme::XApiKeyHeader;
    Profile.ProtocolVersion = "2026-10-30";
  }
  Profile.Limit.MaximumOutputTokens = 64;
  Profile.Capabilities = Providers::ProviderCapabilities{
      true, true, true, true, true, true};
  return Profile;
}

[[nodiscard]] Core::BusinessActionDefinition MakeAsyncLookup(
    std::function<Asio::awaitable<Core::BusinessActionExecutionResult>(
        const Core::BusinessActionInvocation&, std::stop_token)> Handler,
    Core::BusinessActionEffectClass Effect =
        Core::BusinessActionEffectClass::InProcessReadOnly,
    std::string RequiredCapability = "Chat.Lookup",
    std::string RequiredConversation = "async-conversation") {
  return Core::BusinessActionDefinition{
      .Name = "LookupProfile",
      .RequiredArgumentFields = {"query"},
      .RequiredCapabilities =
          RequiredCapability.empty()
              ? std::vector<std::string>{}
              : std::vector<std::string>{std::move(RequiredCapability)},
      .Scope = Core::BusinessActionScopeRequirement{
          .ChannelId = std::string{"Mock"},
          .ConversationId = std::move(RequiredConversation),
      },
      .Handler = {},
      .ModelTool = Providers::ToolDefinition{
          .Name = "LookupProfile",
          .Description = "Look up a profile using trusted in-process data",
          .Parameters = Providers::JsonSchema{
              R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})"},
          .Strict = true,
      },
      .EffectClass = Effect,
      .AsyncHandler = std::move(Handler),
  };
}

[[nodiscard]] Providers::ToolDefinition MakeRegistrySchemaTool(
    std::string SchemaText) {
  return Providers::ToolDefinition{
      .Name = "InspectRecord",
      .Description = "validate a canonical in-process tool schema",
      .Parameters = Providers::JsonSchema{std::move(SchemaText)},
      .Strict = true,
  };
}

[[nodiscard]] Testing::MockChannelConfiguration MakeMockConfiguration(
    const Channels::AccountProfile& Profile) {
  std::vector<Testing::MockActionReceipt> Receipts;
  for (std::uint64_t Sequence = 1; Sequence <= 16; ++Sequence) {
    const Channels::ActionId ActionId{"async-action-" + std::to_string(Sequence)};
    const Channels::OperationId OperationId{
        "async-operation-" + std::to_string(Sequence)};
    Receipts.emplace_back(Testing::MockActionReceipt{
        .Action = ActionId,
        .Operation = OperationId,
        .Receipt = Channels::ChannelReceipt{
            .Action = ActionId,
            .Operation = OperationId,
            .PlatformReceipt = Channels::PlatformReceiptId{
                "async-receipt-" + std::to_string(Sequence)},
            .OccurredAt = Channels::Timestamp{},
            .Status = Channels::ReceiptStatus::Succeeded,
            .ErrorCode = Channels::ChannelErrorCode::None,
            .RetryAfter = std::nullopt,
        },
    });
  }
  return Testing::MockChannelConfiguration{
      .Profile = Profile,
      .Capabilities = MakeCapabilities(Profile),
      .InboundEvents = {MakeInboundMessage()},
      .SendActionResults = std::move(Receipts),
  };
}

[[nodiscard]] bool Check(bool Condition, std::string_view Label) {
  if (!Condition) {
    std::cerr << "FAIL: " << Label << '\n';
  }
  return Condition;
}

[[nodiscard]] Providers::AgentTurnId MakeTurnId(
    const Channels::ChannelEnvelope& Envelope) {
  std::string Value{"CoreTurn:"};
  const auto Append = [&Value](std::string_view Part) {
    Value.append(std::to_string(Part.size()));
    Value.push_back(':');
    Value.append(Part);
  };
  Append(Envelope.Channel.Value);
  Append(Envelope.Account.Value);
  Append(Envelope.StableDeduplication.Value);
  return Providers::AgentTurnId{std::move(Value)};
}

class Fixture final {
public:
  explicit Fixture(
      FakeScenario Scenario = FakeScenario::Final,
      std::vector<std::string> GrantedCapabilities = {"Chat.Lookup"})
      : ChannelProfile(MakeChannelProfile()),
        Channel(MakeMockConfiguration(ChannelProfile)),
        ProviderProfile(MakeProviderProfile(Scenario)),
        Policy(3, {Core::IdentityBinding{
                       .Account = ChannelProfile,
                       .Sender = Channels::SenderId{"async-sender"},
                       .Principal = Core::PrincipalId{"async-principal"},
                       .GrantedCapabilities =
                           std::move(GrantedCapabilities),
                   }}),
        Service(Policy, SyncRuntime, SyncProvider, ProviderProfile,
                ProviderLease, Channel, ChannelLease, IdSource,
                BusinessActions, &OperationObserver),
        Provider(Scenario) {
    const bool Started =
        Channel.Start(ChannelProfile, ChannelLease).Status ==
        Channels::StartStatus::Started;
    const bool Subscribed =
        Channel.SubscribeEvents(InboundSink).Status ==
        Channels::SubscriptionStatus::Subscribed;
    Ready = Started && Subscribed && InboundSink.Events.size() == 1;
  }

  [[nodiscard]] Core::CoreChatResult RunAsync(
      Channels::InboundChannelMessage Message,
      std::stop_token CancellationToken = {}) {
    Asio::io_context Context;
    return RunServiceAsync(Context, std::move(Message), CancellationToken);
  }

  [[nodiscard]] Core::CoreChatResult RunDefaultAsync(
      std::stop_token CancellationToken = {}) {
    return RunAsync(InboundSink.Events.front(), CancellationToken);
  }

  [[nodiscard]] Core::CoreChatResult RunSync(
      const Channels::InboundChannelMessage& Message) {
    return Service.Process(Message);
  }

  [[nodiscard]] Core::CoreChatResult RunToolAsync(
      Channels::InboundChannelMessage Message,
      std::stop_token CancellationToken = {}) {
    Asio::io_context Context;
    std::optional<Core::CoreChatResult> Result;
    std::exception_ptr Failure;
    Core::AsyncAgentToolRuntimePort& ToolRuntime = Runtime;
    Asio::co_spawn(
        Context,
        Service.ProcessAsync(std::move(Message), ToolRuntime, Provider,
                             CancellationToken),
        [&Result, &Failure](std::exception_ptr Exception,
                            Core::CoreChatResult Value) {
          Failure = std::move(Exception);
          Result = std::move(Value);
        });
    Context.run();
    if (Failure) {
      std::rethrow_exception(Failure);
    }
    if (!Result.has_value()) {
      throw std::runtime_error("tool coroutine produced no CoreChatResult");
    }
    return *Result;
  }

  [[nodiscard]] Core::CoreChatResult RunToolAsyncObservingTerminal(
      bool& CoreReturnedBeforeProviderTerminal,
      std::stop_token CancellationToken = {}) {
    Asio::io_context Context;
    std::optional<Core::CoreChatResult> Result;
    std::exception_ptr Failure;
    Provider.BeforeCustomTerminal = [&]() {
      CoreReturnedBeforeProviderTerminal = Result.has_value();
    };
    Core::AsyncAgentToolRuntimePort& ToolRuntime = Runtime;
    Asio::co_spawn(
        Context,
        Service.ProcessAsync(Inbound(), ToolRuntime, Provider,
                             CancellationToken),
        [&Result, &Failure](std::exception_ptr Exception,
                            Core::CoreChatResult Value) {
          Failure = std::move(Exception);
          Result = std::move(Value);
        });
    Context.run();
    if (Failure) {
      std::rethrow_exception(Failure);
    }
    if (!Result.has_value()) {
      throw std::runtime_error("terminal observation produced no Core result");
    }
    return *Result;
  }

  [[nodiscard]] Channels::InboundChannelMessage Inbound() const {
    return InboundSink.Events.front();
  }

  Channels::AccountProfile ChannelProfile;
  SynchronizedMockChannel Channel;
  TestChannelCredential ChannelLease;
  TestProviderCredential ProviderLease;
  SyncRuntimeStub SyncRuntime;
  SyncProviderStub SyncProvider;
  Providers::ModelProviderProfile ProviderProfile;
  ScriptedActionIdSource IdSource;
  CollectingOperationObserver OperationObserver;
  Core::IdentityPolicy Policy;
  Core::BusinessActionRegistry BusinessActions;
  Core::CoreChatService Service;
  CollectingSink InboundSink;
  EmulativeDialog::Agent::AsyncAgentRuntime Runtime;
  AsyncFakeProvider Provider;
  bool Ready{};

private:
  [[nodiscard]] Core::CoreChatResult RunServiceAsync(
      Asio::io_context& Context,
      Channels::InboundChannelMessage Message,
      std::stop_token CancellationToken) {
    std::optional<Core::CoreChatResult> Result;
    std::exception_ptr Failure;
    Asio::co_spawn(
        Context,
        Service.ProcessAsync(
            std::move(Message),
            static_cast<Core::AsyncAgentRuntimePort&>(Runtime), Provider,
            CancellationToken),
        [&Result, &Failure](std::exception_ptr Exception,
                            Core::CoreChatResult Value) {
          Failure = std::move(Exception);
          Result = std::move(Value);
        });
    Context.run();
    if (Failure) {
      std::rethrow_exception(Failure);
    }
    if (!Result.has_value()) {
      throw std::runtime_error("co_spawn produced no CoreChatResult");
    }
    return *Result;
  }
};

class AsyncResultStub final : public Core::AsyncAgentRuntimePort {
public:
  explicit AsyncResultStub(Core::AgentRunResult Result)
      : Result(std::move(Result)) {}

  [[nodiscard]] Asio::awaitable<Core::AgentRunResult> RunTurnAsync(
      const Core::AuthorizedTurnContext&,
      Providers::AsyncModelProviderPort&,
      const Providers::ModelProviderProfile&,
      const Providers::CredentialLease&,
      std::stop_token) override {
    co_return Result;
  }

private:
  Core::AgentRunResult Result;
};

class ThrowAfterProviderDispatchRuntime final
    : public Core::AsyncAgentToolRuntimePort {
public:
  [[nodiscard]] Asio::awaitable<Core::AgentRunResult> RunTurnAsync(
      const Core::AuthorizedTurnContext&,
      Providers::AsyncModelProviderPort&,
      const Providers::ModelProviderProfile&,
      const Providers::CredentialLease&,
      std::stop_token) override {
    co_return Core::AgentRunResult::Failed(
        Core::AgentRunFailureCode::InternalError);
  }

  [[nodiscard]] Asio::awaitable<Core::AgentRunResult>
  RunTurnWithToolsAsync(
      const Core::AuthorizedTurnContext& Context,
      Providers::AsyncModelProviderPort& Provider,
      const Providers::ModelProviderProfile& Profile,
      const Providers::CredentialLease& Lease,
      Core::AsyncBusinessActionDispatcherPort&,
      std::stop_token CancellationToken) override {
    Providers::CanonicalModelRequest Request;
    Request.Id = Providers::RequestId{"throw-after-provider-start"};
    Request.TurnId = Context.AgentTurn;
    Request.CreatedAt = std::chrono::system_clock::now();
    Request.Messages.push_back(Providers::CanonicalMessage{
        .Role = Providers::MessageRole::User,
        .ContentParts = {Providers::TextContentPart{.Text = "hello"}},
        .FunctionCalls = {},
        .ToolCallResultId = std::nullopt,
    });
    static_cast<void>(co_await Provider.StreamTurnAsync(
        Profile, Lease, Request, CancellationToken,
        [](const Providers::CanonicalNonTerminalEvent&) {}));
    throw std::runtime_error("runtime escaped after provider request start");
  }
};

[[nodiscard]] Core::CoreChatResult RunWithRuntimeStub(
    Fixture& TestFixture,
    Channels::InboundChannelMessage Message,
    AsyncResultStub& Runtime) {
  Asio::io_context Context;
  std::optional<Core::CoreChatResult> Result;
  std::exception_ptr Failure;
  Asio::co_spawn(
      Context,
      TestFixture.Service.ProcessAsync(std::move(Message), Runtime,
                                       TestFixture.Provider),
      [&Result, &Failure](std::exception_ptr Exception,
                         Core::CoreChatResult Value) {
        Failure = std::move(Exception);
        Result = std::move(Value);
      });
  Context.run();
  if (Failure) {
    std::rethrow_exception(Failure);
  }
  if (!Result.has_value()) {
    throw std::runtime_error("stub coroutine produced no CoreChatResult");
  }
  return *Result;
}

[[nodiscard]] bool TestSuccessfulMockToMockCoroutineSlice() {
  Fixture TestFixture;
  if (!Check(TestFixture.Ready,
             "MockChannel starts and publishes the trusted inbound message")) {
    return false;
  }
  const Channels::InboundChannelMessage Message = TestFixture.Inbound();
  const Core::CoreChatResult Result = TestFixture.RunAsync(Message);
  const auto Receipt = TestFixture.Channel.GetLastSendActionReceipt();
  const auto Sends = TestFixture.Channel.GetSendObservations();
  const auto Records = TestFixture.OperationObserver.Snapshot();
  bool Passed = true;
  Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                      Result.Completion == Providers::CompletionKind::FinalAnswer,
                  "awaitable provider final response completes through Core");
  Passed &= Check(TestFixture.Provider.DispatchCount.load() == 1 &&
                      TestFixture.Provider.GetLastRequestText() ==
                          "hello from mock channel",
                  "Core-authorized inbound text reaches one fake provider request");
  Passed &= Check(TestFixture.IdSource.NextSequence.load() == 2 &&
                      TestFixture.Channel.GetSendActionCallCount() == 1,
                  "Core creates one authorized action and sends it once");
  Passed &= Check(Receipt.has_value() &&
                      Receipt->Status == Channels::ReceiptStatus::Succeeded &&
                      Receipt->Action.Value == "async-action-1" &&
                      Receipt->Operation.Value == "async-operation-1",
                  "MockChannel receipt matches the exact Core action identifiers");
  Passed &= Check(Sends.size() == 1 && Sends.front().PolicyVersion == 3 &&
                      Sends.front().CapabilitySnapshotVersion == 9,
                  "CoreAuthorizedAction carries the expected policy and capability versions");
  Passed &= Check(Sends.size() == 1 &&
                      Sends.front().TargetPrincipalId == "async-principal" &&
                      Sends.front().TargetChannelId == "Mock" &&
                      Sends.front().TargetAccountId == "async-account" &&
                      Sends.front().TargetConversationId == "async-conversation" &&
                      Sends.front().ActionKind == Core::CoreAuthorizedActionKind::SendText &&
                      Sends.front().TextPayload == "coroutine response" &&
                      Sends.front().AuthorizationRefValue.starts_with(
                          "CoreAuthorization:"),
                  "CoreAuthorizedAction getters expose the authorized scope, kind, text and reference");
  Passed &= Check(!Records.empty() &&
                      Records.back().GetState() == Core::OperationState::Succeeded &&
                      Records.back().GetActionId().Value == "async-action-1" &&
                      Records.back().GetOperationId().Value == "async-operation-1" &&
                      Records.back().GetPolicyVersion() == 3 &&
                      Records.back().GetCapabilitySnapshotVersion() == 9,
                  "OperationRecord preserves action, receipt, policy and capability correlation");
  Passed &= Check(!EmulativeDialog::Agent::OutputBudgetController::Shared()
                       .HasActiveTurn(MakeTurnId(Message.Envelope)),
                  "completed async Core turn releases its output-budget ledger");
  return Passed;
}

struct ScenarioExpectation final {
  FakeScenario Scenario;
  Core::CoreChatStatus Status;
  std::optional<Core::AgentRunFailureCode> Failure;
  bool SendsAction{};
};

[[nodiscard]] bool TestTerminalAndProviderFailureMatrix() {
  const std::vector<ScenarioExpectation> Cases{
      {FakeScenario::Refusal, Core::CoreChatStatus::Completed, std::nullopt, true},
      {FakeScenario::ToolHandoff, Core::CoreChatStatus::Unsupported,
       std::nullopt, false},
      {FakeScenario::ToolCall, Core::CoreChatStatus::Unsupported,
       std::nullopt, false},
      {FakeScenario::OutputLimited, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::OutputLimitReached, false},
      {FakeScenario::Failed, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::ProviderRejected, false},
      {FakeScenario::Cancelled, Core::CoreChatStatus::Cancelled,
       std::nullopt, false},
      {FakeScenario::Unknown, Core::CoreChatStatus::UnknownOutcome,
       std::nullopt, false},
      {FakeScenario::ThrowAfterDispatch, Core::CoreChatStatus::UnknownOutcome,
       std::nullopt, false},
      {FakeScenario::InvalidSequence, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::InvalidStream, false},
      {FakeScenario::ProfileRejected, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::InvalidProfile, false},
      {FakeScenario::CapabilitiesRejected, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::UnsupportedCapability, false},
      {FakeScenario::UnsupportedCapabilities, Core::CoreChatStatus::Unsupported,
       std::nullopt, false},
  };

  bool Passed = true;
  for (const ScenarioExpectation& Expected : Cases) {
    Fixture TestFixture(Expected.Scenario);
    const Channels::InboundChannelMessage Message = TestFixture.Inbound();
    const Core::CoreChatResult Result = TestFixture.RunAsync(Message);
    const std::string Label =
        "async terminal scenario " +
        std::to_string(static_cast<unsigned>(Expected.Scenario));
    Passed &= Check(Result.Status == Expected.Status, Label + " maps to expected Core status");
    if (Expected.Failure.has_value()) {
      Passed &= Check(Result.AgentFailure == Expected.Failure,
                      Label + " preserves the typed Agent failure");
    }
    Passed &= Check(TestFixture.Channel.GetSendActionCallCount() ==
                        (Expected.SendsAction ? 1U : 0U),
                    Label + " sends only a valid completed final/refusal");
    Passed &= Check(TestFixture.IdSource.NextSequence.load() ==
                        (Expected.SendsAction ? 2U : 1U),
                    Label + " does not allocate action IDs before sendable final text");
    Passed &= Check(!EmulativeDialog::Agent::OutputBudgetController::Shared()
                         .HasActiveTurn(MakeTurnId(Message.Envelope)),
                    Label + " releases the output-budget ledger on every terminal");
  }
  return Passed;
}

[[nodiscard]] bool TestCancellationAndInputOrdering() {
  bool Passed = true;
  {
    Fixture TestFixture;
    std::stop_source CancelBeforePreflight;
    CancelBeforePreflight.request_stop();
    const Channels::InboundChannelMessage Message = TestFixture.Inbound();
    const auto Cancelled =
        TestFixture.RunAsync(Message, CancelBeforePreflight.get_token());
    Passed &= Check(Cancelled.Status == Core::CoreChatStatus::Cancelled &&
                        TestFixture.Provider.DispatchCount.load() == 0,
                    "pre-cancel returns before provider dispatch");
    const auto RetriedWithSameMessage = TestFixture.RunAsync(Message);
    Passed &= Check(RetriedWithSameMessage.Status ==
                        Core::CoreChatStatus::Completed,
                    "pre-cancel does not consume the deduplication key");
  }
  {
    Fixture TestFixture;
    TestFixture.Provider.StreamDelay = std::chrono::milliseconds(80);
    const Channels::InboundChannelMessage Message = TestFixture.Inbound();
    std::stop_source Cancellation;
    Asio::io_context Context;
    Asio::steady_timer CancelWhileProviderSuspended(Context);
    CancelWhileProviderSuspended.expires_after(std::chrono::milliseconds(10));
    std::atomic<bool> StopWasRequestedDuringProviderWait{false};
    CancelWhileProviderSuspended.async_wait(
        [&TestFixture, &Cancellation, &StopWasRequestedDuringProviderWait](
            const auto& Error) {
          if (!Error) {
            StopWasRequestedDuringProviderWait.store(
                TestFixture.Provider.AwaitingProvider.load());
            Cancellation.request_stop();
          }
        });
    std::optional<Core::CoreChatResult> Result;
    std::exception_ptr Failure;
    Asio::co_spawn(
        Context,
        TestFixture.Service.ProcessAsync(
            Message,
            static_cast<Core::AsyncAgentRuntimePort&>(TestFixture.Runtime),
            TestFixture.Provider,
            Cancellation.get_token()),
        [&Result, &Failure](std::exception_ptr Exception,
                            Core::CoreChatResult Value) {
          Failure = std::move(Exception);
          Result = std::move(Value);
        });
    Context.run();
    const bool BudgetReleased =
        !EmulativeDialog::Agent::OutputBudgetController::Shared()
             .HasActiveTurn(MakeTurnId(Message.Envelope));
    Passed &= Check(!Failure && Result.has_value() &&
                        StopWasRequestedDuringProviderWait.load() &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1,
                    "the stop source fires while the fake provider is suspended and it still returns Final");
    Passed &= Check(Result->Status == Core::CoreChatStatus::Cancelled &&
                        TestFixture.IdSource.NextSequence.load() == 1 &&
                        TestFixture.Channel.GetSendAttemptCount() == 0 &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "Core cancels a late Final before allocating IDs or sending");
    Passed &= Check(BudgetReleased,
                    "cancellation after the async wait releases the output-budget ledger");
  }
  {
    Fixture TestFixture(FakeScenario::Unknown);
    TestFixture.Provider.StreamDelay = std::chrono::milliseconds(80);
    const Channels::InboundChannelMessage Message = TestFixture.Inbound();
    std::stop_source Cancellation;
    Asio::io_context Context;
    Asio::steady_timer CancelWhileProviderSuspended(Context);
    CancelWhileProviderSuspended.expires_after(std::chrono::milliseconds(10));
    std::atomic<bool> StopWasRequestedDuringProviderWait{false};
    CancelWhileProviderSuspended.async_wait(
        [&TestFixture, &Cancellation, &StopWasRequestedDuringProviderWait](
            const auto& Error) {
          if (!Error) {
            StopWasRequestedDuringProviderWait.store(
                TestFixture.Provider.AwaitingProvider.load());
            Cancellation.request_stop();
          }
        });
    std::optional<Core::CoreChatResult> Result;
    std::exception_ptr Failure;
    Asio::co_spawn(
        Context,
        TestFixture.Service.ProcessAsync(
            Message,
            static_cast<Core::AsyncAgentRuntimePort&>(TestFixture.Runtime),
            TestFixture.Provider,
            Cancellation.get_token()),
        [&Result, &Failure](std::exception_ptr Exception,
                            Core::CoreChatResult Value) {
          Failure = std::move(Exception);
          Result = std::move(Value);
        });
    Context.run();
    const bool BudgetReleased =
        !EmulativeDialog::Agent::OutputBudgetController::Shared()
             .HasActiveTurn(MakeTurnId(Message.Envelope));
    Passed &= Check(!Failure && Result.has_value() &&
                        StopWasRequestedDuringProviderWait.load() &&
                        TestFixture.Provider.DispatchCount.load() == 1,
                    "cancellation is requested after dispatch while the fake provider is suspended");
    Passed &= Check(Result->Status == Core::CoreChatStatus::UnknownOutcome &&
                        TestFixture.IdSource.NextSequence.load() == 1 &&
                        TestFixture.Channel.GetSendAttemptCount() == 0 &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "a possibly dispatched UnknownOutcome outranks concurrent cancellation and is never replayed");
    Passed &= Check(BudgetReleased,
                    "unknown outcome after concurrent cancellation releases the output-budget ledger");
  }
  {
    Fixture TestFixture;
    auto Empty = TestFixture.Inbound();
    Empty.Message.MessageParts.clear();
    const auto EmptyResult = TestFixture.RunAsync(Empty);
    const auto DuplicateAfterEmpty = TestFixture.RunSync(TestFixture.Inbound());
    Passed &= Check(EmptyResult.Status == Core::CoreChatStatus::Rejected &&
                        EmptyResult.Failure == Core::CoreChatFailureCode::InvalidMessage &&
                        DuplicateAfterEmpty.Status == Core::CoreChatStatus::Duplicate,
                    "empty input consumes the key after identity and dedup reservation");
    Passed &= Check(TestFixture.Provider.DispatchCount.load() == 0 &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "invalid input never reaches Runtime or Channel.SendAction");
  }
  {
    Fixture TestFixture;
    auto Unsupported = TestFixture.Inbound();
    Unsupported.Message.MessageParts = {
        Channels::MediaReferencePart{
            Channels::MediaReferenceId{"opaque-audio-reference"},
            Channels::MediaReferenceKind::Audio}};
    const auto UnsupportedResult = TestFixture.RunAsync(Unsupported);
    const auto DuplicateAfterUnsupported =
        TestFixture.RunSync(TestFixture.Inbound());
    Passed &= Check(UnsupportedResult.Status == Core::CoreChatStatus::Unsupported &&
                        UnsupportedResult.Failure ==
                            Core::CoreChatFailureCode::UnsupportedInput &&
                        DuplicateAfterUnsupported.Status ==
                            Core::CoreChatStatus::Duplicate,
                    "unsupported input also consumes the reserved key");
  }
  {
    const std::vector<Core::CoreChatFailureCode> IdentityFailures{
        Core::CoreChatFailureCode::InvalidEnvelope,
        Core::CoreChatFailureCode::UnknownAccount,
        Core::CoreChatFailureCode::AccountRouteMismatch,
        Core::CoreChatFailureCode::UnknownSender,
    };
    for (const Core::CoreChatFailureCode ExpectedFailure : IdentityFailures) {
      Fixture TestFixture;
      auto InvalidIdentity = TestFixture.Inbound();
      if (ExpectedFailure == Core::CoreChatFailureCode::InvalidEnvelope) {
        InvalidIdentity.Envelope.Message.Value.clear();
      } else if (ExpectedFailure == Core::CoreChatFailureCode::UnknownAccount) {
        InvalidIdentity.Envelope.Account.Value = "unregistered-account";
      } else if (ExpectedFailure ==
                 Core::CoreChatFailureCode::AccountRouteMismatch) {
        InvalidIdentity.Envelope.Mode.Value = "unregistered-mode";
      } else {
        InvalidIdentity.Envelope.Sender.Value = "unregistered-sender";
      }

      const auto Rejected = TestFixture.RunAsync(InvalidIdentity);
      Passed &= Check(Rejected.Status == Core::CoreChatStatus::Rejected &&
                          Rejected.Failure == ExpectedFailure &&
                          TestFixture.Provider.DispatchCount.load() == 0 &&
                          TestFixture.IdSource.NextSequence.load() == 1 &&
                          TestFixture.Channel.GetSendActionCallCount() == 0,
                      "invalid, unknown-account, route-mismatch and unknown-sender identities fail before runtime");
      const auto AuthorizedRetry = TestFixture.RunAsync(TestFixture.Inbound());
      Passed &= Check(AuthorizedRetry.Status == Core::CoreChatStatus::Completed &&
                          TestFixture.Channel.GetSendActionCallCount() == 1,
                      "identity rejection happens before and does not reserve dedup");
    }
  }
  return Passed;
}

[[nodiscard]] bool TestSyncCompatibilityAndMixedDuplicateRace() {
  bool Passed = true;
  {
    Fixture TestFixture;
    const auto Message = TestFixture.Inbound();
    const auto Result = TestFixture.RunSync(Message);
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        TestFixture.SyncRuntime.Calls.load() == 1 &&
                        TestFixture.Channel.GetSendActionCallCount() == 1,
                    "synchronous V1 Process remains callable through shared Core helpers");
  }

  enum class RacePath : std::uint8_t { None, Async, Sync };
  constexpr std::size_t RaceCount = 24;

  for (std::size_t RaceIndex = 0; RaceIndex < RaceCount; ++RaceIndex) {
    Fixture TestFixture;
    const Channels::InboundChannelMessage Message = TestFixture.Inbound();
    std::barrier<> StartGate{3};
    const RacePath DelayedArrivalPath =
        RaceIndex % 2 == 0 ? RacePath::Async : RacePath::Sync;
    std::atomic<RacePath> WinnerPath{RacePath::None};
    std::atomic<std::uint32_t> RuntimeEntryCount{0};
    std::promise<RacePath> WinnerEntered;
    std::future<RacePath> WinnerEnteredFuture = WinnerEntered.get_future();
    std::promise<void> ReleaseWinner;
    std::shared_future<void> ReleaseWinnerFuture = ReleaseWinner.get_future();
    std::promise<void> AsyncDone;
    std::future<void> AsyncDoneFuture = AsyncDone.get_future();
    std::promise<void> SyncDone;
    std::future<void> SyncDoneFuture = SyncDone.get_future();
    std::atomic<bool> WinnerReleased{false};
    std::atomic<bool> AsyncReturnedWhileWinnerHeld{false};
    std::atomic<bool> SyncReturnedWhileWinnerHeld{false};
    std::optional<Core::CoreChatResult> AsyncResult;
    std::optional<Core::CoreChatResult> SyncResult;
    std::exception_ptr AsyncFailure;
    std::exception_ptr SyncFailure;

    const auto HoldWinner = [&](RacePath Path) {
      RuntimeEntryCount.fetch_add(1);
      RacePath Expected = RacePath::None;
      if (WinnerPath.compare_exchange_strong(Expected, Path)) {
        WinnerEntered.set_value(Path);
      }
      ReleaseWinnerFuture.wait();
    };
    TestFixture.Provider.OnDispatched = [&]() {
      HoldWinner(RacePath::Async);
    };
    TestFixture.SyncRuntime.OnRun = [&]() {
      HoldWinner(RacePath::Sync);
    };

    const auto AsyncTask = [&]() {
      if (DelayedArrivalPath == RacePath::Async) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      StartGate.arrive_and_wait();
      try {
        AsyncResult = TestFixture.RunAsync(Message);
      } catch (...) {
        AsyncFailure = std::current_exception();
      }
      AsyncReturnedWhileWinnerHeld.store(!WinnerReleased.load());
      AsyncDone.set_value();
    };
    const auto SyncTask = [&]() {
      if (DelayedArrivalPath == RacePath::Sync) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      StartGate.arrive_and_wait();
      try {
        SyncResult = TestFixture.Service.Process(Message);
      } catch (...) {
        SyncFailure = std::current_exception();
      }
      SyncReturnedWhileWinnerHeld.store(!WinnerReleased.load());
      SyncDone.set_value();
    };

    std::thread AsyncThread;
    std::thread SyncThread;
    if (RaceIndex % 2 == 0) {
      AsyncThread = std::thread(AsyncTask);
      SyncThread = std::thread(SyncTask);
    } else {
      SyncThread = std::thread(SyncTask);
      AsyncThread = std::thread(AsyncTask);
    }

    StartGate.arrive_and_wait();
    RacePath ActualWinner = RacePath::None;
    if (WinnerEnteredFuture.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready) {
      ActualWinner = WinnerEnteredFuture.get();
    }

    bool LoserReturnedBeforeRelease = false;
    if (ActualWinner == RacePath::Async) {
      LoserReturnedBeforeRelease =
          SyncDoneFuture.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready;
    } else if (ActualWinner == RacePath::Sync) {
      LoserReturnedBeforeRelease =
          AsyncDoneFuture.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready;
    }

    WinnerReleased.store(true);
    ReleaseWinner.set_value();
    AsyncThread.join();
    SyncThread.join();

    const bool WinnerCompleted =
        ActualWinner == RacePath::Async
            ? AsyncResult.has_value() &&
                  AsyncResult->Status == Core::CoreChatStatus::Completed
            : ActualWinner == RacePath::Sync && SyncResult.has_value() &&
                  SyncResult->Status == Core::CoreChatStatus::Completed;
    const bool LoserWasDuplicate =
        ActualWinner == RacePath::Async
            ? SyncResult.has_value() &&
                  SyncResult->Status == Core::CoreChatStatus::Duplicate
            : ActualWinner == RacePath::Sync && AsyncResult.has_value() &&
                  AsyncResult->Status == Core::CoreChatStatus::Duplicate;
    const bool LoserReturnedWhileHeld =
        ActualWinner == RacePath::Async
            ? SyncReturnedWhileWinnerHeld.load()
            : ActualWinner == RacePath::Sync &&
                  AsyncReturnedWhileWinnerHeld.load();
    const bool ExactlyOneRuntimePath =
        ActualWinner == RacePath::Async
            ? TestFixture.Provider.DispatchCount.load() == 1 &&
                  TestFixture.SyncRuntime.Calls.load() == 0
            : ActualWinner == RacePath::Sync &&
                  TestFixture.Provider.DispatchCount.load() == 0 &&
                  TestFixture.SyncRuntime.Calls.load() == 1;
    const bool SingleRuntimeEntry = RuntimeEntryCount.load() == 1;

    Passed &= Check(ActualWinner != RacePath::None &&
                        LoserReturnedBeforeRelease && LoserReturnedWhileHeld &&
                        !AsyncFailure && !SyncFailure && WinnerCompleted &&
                        LoserWasDuplicate && ExactlyOneRuntimePath &&
                        SingleRuntimeEntry &&
                        TestFixture.Channel.GetSendActionCallCount() == 1,
                    "each simultaneous barrier race yields one Completed winner and one pre-release Duplicate");
  }

  const auto RunControlledWinnerCase = [&](RacePath DesiredWinner) {
    Fixture TestFixture;
    const Channels::InboundChannelMessage Message = TestFixture.Inbound();
    std::atomic<std::uint32_t> RuntimeEntryCount{0};
    std::atomic<bool> WinnerNotified{false};
    std::atomic<bool> WinnerReleased{false};
    std::promise<void> WinnerEntered;
    std::future<void> WinnerEnteredFuture = WinnerEntered.get_future();
    std::promise<void> ReleaseWinner;
    std::shared_future<void> ReleaseWinnerFuture = ReleaseWinner.get_future();
    std::promise<void> LoserDone;
    std::future<void> LoserDoneFuture = LoserDone.get_future();
    std::atomic<bool> LoserReturnedWhileWinnerHeld{false};
    std::optional<Core::CoreChatResult> WinnerResult;
    std::optional<Core::CoreChatResult> LoserResult;
    std::exception_ptr WinnerFailure;
    std::exception_ptr LoserFailure;

    const auto ObserveRuntimeEntry = [&](RacePath Path) {
      RuntimeEntryCount.fetch_add(1);
      if (Path == DesiredWinner) {
        bool Expected = false;
        if (WinnerNotified.compare_exchange_strong(Expected, true)) {
          WinnerEntered.set_value();
        }
        ReleaseWinnerFuture.wait();
      }
    };
    TestFixture.Provider.OnDispatched = [&]() {
      ObserveRuntimeEntry(RacePath::Async);
    };
    TestFixture.SyncRuntime.OnRun = [&]() {
      ObserveRuntimeEntry(RacePath::Sync);
    };

    std::thread WinnerThread([&]() {
      try {
        if (DesiredWinner == RacePath::Async) {
          WinnerResult = TestFixture.RunAsync(Message);
        } else {
          WinnerResult = TestFixture.Service.Process(Message);
        }
      } catch (...) {
        WinnerFailure = std::current_exception();
      }
    });

    const bool WinnerEnteredBeforeLoser =
        WinnerEnteredFuture.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready;
    std::thread LoserThread([&]() {
      try {
        if (DesiredWinner == RacePath::Async) {
          LoserResult = TestFixture.Service.Process(Message);
        } else {
          LoserResult = TestFixture.RunAsync(Message);
        }
      } catch (...) {
        LoserFailure = std::current_exception();
      }
      LoserReturnedWhileWinnerHeld.store(!WinnerReleased.load());
      LoserDone.set_value();
    });

    const bool LoserReturnedBeforeRelease =
        LoserDoneFuture.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready;
    WinnerReleased.store(true);
    ReleaseWinner.set_value();
    WinnerThread.join();
    LoserThread.join();

    const bool WinnerCompleted =
        WinnerResult.has_value() &&
        WinnerResult->Status == Core::CoreChatStatus::Completed;
    const bool LoserWasDuplicate =
        LoserResult.has_value() &&
        LoserResult->Status == Core::CoreChatStatus::Duplicate;
    const bool ExactlyOneExpectedPath =
        DesiredWinner == RacePath::Async
            ? TestFixture.Provider.DispatchCount.load() == 1 &&
                  TestFixture.SyncRuntime.Calls.load() == 0
            : TestFixture.Provider.DispatchCount.load() == 0 &&
                  TestFixture.SyncRuntime.Calls.load() == 1;

    return Check(WinnerEnteredBeforeLoser && LoserReturnedBeforeRelease &&
                     LoserReturnedWhileWinnerHeld.load() && !WinnerFailure &&
                     !LoserFailure && WinnerCompleted && LoserWasDuplicate &&
                     RuntimeEntryCount.load() == 1 && ExactlyOneExpectedPath &&
                     TestFixture.Channel.GetSendActionCallCount() == 1,
                 DesiredWinner == RacePath::Async
                     ? "controlled async winner remains held while sync duplicate returns"
                     : "controlled sync winner remains held while async duplicate returns");
  };

  Passed &= RunControlledWinnerCase(RacePath::Async);
  Passed &= RunControlledWinnerCase(RacePath::Sync);
  return Passed;
}

[[nodiscard]] bool TestDistinctConcurrentMessagesAndIdentifierSafety() {
  bool Passed = true;
  {
    Fixture TestFixture;
    TestFixture.Provider.StreamDelay = std::chrono::milliseconds(40);
    std::mutex GateMutex;
    std::condition_variable GateCondition;
    std::size_t EnteredStreams = 0;
    TestFixture.Provider.OnDispatched = [&]() {
      std::unique_lock Lock(GateMutex);
      ++EnteredStreams;
      GateCondition.notify_all();
      static_cast<void>(GateCondition.wait_for(
          Lock, std::chrono::seconds(2), [&]() { return EnteredStreams >= 2; }));
    };

    auto FirstMessage = TestFixture.Inbound();
    FirstMessage.Envelope.StableDeduplication.Value = "concurrent-dedup-a";
    FirstMessage.Envelope.Message.Value = "concurrent-message-a";
    auto SecondMessage = TestFixture.Inbound();
    SecondMessage.Envelope.StableDeduplication.Value = "concurrent-dedup-b";
    SecondMessage.Envelope.Message.Value = "concurrent-message-b";

    Asio::io_context Context;
    std::optional<Core::CoreChatResult> FirstResult;
    std::optional<Core::CoreChatResult> SecondResult;
    std::exception_ptr FirstFailure;
    std::exception_ptr SecondFailure;
    Asio::co_spawn(
        Context,
        TestFixture.Service.ProcessAsync(
            FirstMessage,
            static_cast<Core::AsyncAgentRuntimePort&>(TestFixture.Runtime),
            TestFixture.Provider),
        [&FirstResult, &FirstFailure](std::exception_ptr Exception,
                                      Core::CoreChatResult Value) {
          FirstFailure = std::move(Exception);
          FirstResult = std::move(Value);
        });
    Asio::co_spawn(
        Context,
        TestFixture.Service.ProcessAsync(
            SecondMessage,
            static_cast<Core::AsyncAgentRuntimePort&>(TestFixture.Runtime),
            TestFixture.Provider),
        [&SecondResult, &SecondFailure](std::exception_ptr Exception,
                                        Core::CoreChatResult Value) {
          SecondFailure = std::move(Exception);
          SecondResult = std::move(Value);
        });
    std::thread FirstWorker([&Context]() { Context.run(); });
    std::thread SecondWorker([&Context]() { Context.run(); });
    FirstWorker.join();
    SecondWorker.join();

    Passed &= Check(!FirstFailure && !SecondFailure && FirstResult.has_value() &&
                        SecondResult.has_value() &&
                        FirstResult->Status == Core::CoreChatStatus::Completed &&
                        SecondResult->Status == Core::CoreChatStatus::Completed,
                    "two distinct concurrent async messages both finish through Core");
    Passed &= Check(TestFixture.Provider.DispatchCount.load() == 2 &&
                        TestFixture.Provider.MaxActiveStreamCount.load() >= 2,
                    "Core state lock does not span await and both provider turns overlap");
    Passed &= Check(TestFixture.IdSource.NextSequence.load() == 3 &&
                        TestFixture.Channel.GetSendActionCallCount() == 2,
                    "different messages receive unique action IDs and correlated receipts");
    const auto Records = TestFixture.OperationObserver.Snapshot();
    std::vector<std::string> SuccessfulOperationIds;
    for (const Core::OperationRecord& Record : Records) {
      if (Record.GetState() == Core::OperationState::Succeeded) {
        SuccessfulOperationIds.push_back(Record.GetOperationId().Value);
        Passed &= Check(Record.GetActionId().Value ==
                            "async-action-" +
                                Record.GetOperationId().Value.substr(
                                    std::string("async-operation-").size()),
                        "OperationRecord action/operation IDs stay paired under concurrency");
      }
    }
    std::sort(SuccessfulOperationIds.begin(), SuccessfulOperationIds.end());
    SuccessfulOperationIds.erase(
        std::unique(SuccessfulOperationIds.begin(), SuccessfulOperationIds.end()),
        SuccessfulOperationIds.end());
    Passed &= Check(SuccessfulOperationIds.size() == 2,
                    "two concurrent turns create distinct successful operations");
  }
  {
    Fixture TestFixture;
    std::promise<void> SyncEntered;
    std::future<void> SyncEnteredFuture = SyncEntered.get_future();
    std::promise<void> ReleaseSync;
    std::future<void> ReleaseSyncFuture = ReleaseSync.get_future();
    TestFixture.SyncRuntime.OnRun = [&SyncEntered, &ReleaseSyncFuture]() {
      SyncEntered.set_value();
      ReleaseSyncFuture.wait();
    };

    std::promise<void> AsyncProviderEntered;
    std::future<void> AsyncProviderEnteredFuture =
        AsyncProviderEntered.get_future();
    std::promise<void> ReleaseAsyncProvider;
    std::future<void> ReleaseAsyncFuture = ReleaseAsyncProvider.get_future();
    TestFixture.Provider.OnDispatched =
        [&AsyncProviderEntered, &ReleaseAsyncFuture]() {
          AsyncProviderEntered.set_value();
          ReleaseAsyncFuture.wait();
        };

    auto SyncMessage = TestFixture.Inbound();
    SyncMessage.Envelope.StableDeduplication.Value = "mixed-sync-dedup";
    SyncMessage.Envelope.Message.Value = "mixed-sync-message";
    auto AsyncMessage = TestFixture.Inbound();
    AsyncMessage.Envelope.StableDeduplication.Value = "mixed-async-dedup";
    AsyncMessage.Envelope.Message.Value = "mixed-async-message";

    auto SyncResult = std::async(std::launch::async,
                                 [&TestFixture, &SyncMessage]() {
                                   return TestFixture.Service.Process(SyncMessage);
                                 });
    const bool SyncWasInRuntime =
        SyncEnteredFuture.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready;

    Asio::io_context Context;
    std::optional<Core::CoreChatResult> AsyncResult;
    std::exception_ptr AsyncFailure;
    Asio::co_spawn(
        Context,
        TestFixture.Service.ProcessAsync(
            AsyncMessage,
            static_cast<Core::AsyncAgentRuntimePort&>(TestFixture.Runtime),
            TestFixture.Provider),
        [&AsyncResult, &AsyncFailure](std::exception_ptr Exception,
                                      Core::CoreChatResult Value) {
          AsyncFailure = std::move(Exception);
          AsyncResult = std::move(Value);
        });
    std::thread AsyncWorker([&Context]() { Context.run(); });
    const bool AsyncWasInProviderWhileSyncPending =
        AsyncProviderEnteredFuture.wait_for(std::chrono::milliseconds(250)) ==
        std::future_status::ready;

    ReleaseSync.set_value();
    ReleaseAsyncProvider.set_value();
    const Core::CoreChatResult SyncCompleted = SyncResult.get();
    AsyncWorker.join();
    Passed &= Check(SyncWasInRuntime && AsyncWasInProviderWhileSyncPending &&
                        SyncCompleted.Status == Core::CoreChatStatus::Completed &&
                        !AsyncFailure && AsyncResult.has_value() &&
                        AsyncResult->Status == Core::CoreChatStatus::Completed,
                    "distinct sync and async messages execute concurrently on one CoreChatService");
    Passed &= Check(TestFixture.IdSource.NextSequence.load() == 3 &&
                        TestFixture.Channel.GetSendActionCallCount() == 2,
                    "mixed concurrent turns receive unique IDs and both correlated Mock receipts");
    const auto Sends = TestFixture.Channel.GetSendObservations();
    const auto Records = TestFixture.OperationObserver.Snapshot();
    std::vector<std::string> IssuedIdentifiers;
    std::size_t CorrelatedSucceededOperations = 0;
    for (const Core::OperationRecord& Record : Records) {
      if (Record.GetState() != Core::OperationState::Succeeded) {
        continue;
      }
      const std::string& ActionId = Record.GetActionId().Value;
      const std::string& OperationId = Record.GetOperationId().Value;
      const std::string& IdempotencyKey = Record.GetIdempotencyKey().Value;
      IssuedIdentifiers.push_back(ActionId);
      IssuedIdentifiers.push_back(OperationId);
      IssuedIdentifiers.push_back(IdempotencyKey);
      const auto Send = std::find_if(
          Sends.begin(), Sends.end(), [&](const SendActionObservation& Candidate) {
            return Candidate.ActionId == ActionId &&
                   Candidate.OperationId == OperationId &&
                   Candidate.IdempotencyKey == IdempotencyKey;
          });
      const bool ExactReceipt =
          Send != Sends.end() && Send->ReceiptActionId == ActionId &&
          Send->ReceiptOperationId == OperationId &&
          Send->ReceiptStatus == Channels::ReceiptStatus::Succeeded &&
          !Send->PlatformReceiptId.empty();
      Passed &= Check(ExactReceipt,
                      "each successful OperationRecord matches its returned receipt");
      if (ExactReceipt) {
        ++CorrelatedSucceededOperations;
      }
    }
    std::sort(IssuedIdentifiers.begin(), IssuedIdentifiers.end());
    IssuedIdentifiers.erase(
        std::unique(IssuedIdentifiers.begin(), IssuedIdentifiers.end()),
        IssuedIdentifiers.end());
    Passed &= Check(CorrelatedSucceededOperations == 2 &&
                        IssuedIdentifiers.size() == 6,
                    "two turns have distinct ActionId/OperationId/IdempotencyKey triples");
  }
  {
    Fixture TestFixture;
    TestFixture.IdSource.EmptyIdentifiers = true;
    const auto Result = TestFixture.RunDefaultAsync();
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.Failure == Core::CoreChatFailureCode::InvalidActionIdentifiers &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "Core rejects an empty ActionId tuple before channel dispatch");
  }
  {
    Fixture TestFixture;
    TestFixture.IdSource.DuplicateIdentifiers = true;
    const auto First = TestFixture.RunAsync(TestFixture.Inbound());
    auto SecondMessage = TestFixture.Inbound();
    SecondMessage.Envelope.StableDeduplication.Value = "distinct-second-turn";
    const auto Second = TestFixture.RunAsync(SecondMessage);
    Passed &= Check(First.Status == Core::CoreChatStatus::Completed &&
                        Second.Status == Core::CoreChatStatus::Failed &&
                        Second.Failure == Core::CoreChatFailureCode::InvalidActionIdentifiers &&
                        TestFixture.Channel.GetSendActionCallCount() == 1,
                    "duplicate Core-issued identifiers fail closed across distinct turns");
  }
  return Passed;
}

[[nodiscard]] bool TestChannelAndTerminalValidationFailures() {
  bool Passed = true;
  for (const std::size_t FailureAtCall : {1U, 2U, 3U}) {
    Fixture TestFixture;
    TestFixture.Channel.SetFaults(FailureAtCall);
    const auto Result = TestFixture.RunDefaultAsync();
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.Failure == Core::CoreChatFailureCode::CapabilityUnavailable &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "capability failure at each Core authorization checkpoint fails closed");
  }
  {
    Fixture TestFixture;
    TestFixture.Channel.SetFaults(0, true);
    const auto Result = TestFixture.RunDefaultAsync();
    Passed &= Check(Result.Status == Core::CoreChatStatus::UnknownOutcome &&
                        Result.Failure == Core::CoreChatFailureCode::ChannelException &&
                        TestFixture.Channel.GetSendAttemptCount() == 1 &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "channel exception after dispatch is unknown and never replayed");
  }
  {
    Fixture TestFixture;
    TestFixture.Channel.SetFaults(0, false, true);
    const auto Result = TestFixture.RunDefaultAsync();
    Passed &= Check(Result.Status == Core::CoreChatStatus::UnknownOutcome &&
                        TestFixture.Channel.GetSendActionCallCount() == 1,
                    "mismatched receipt becomes unknown outcome without retry");
  }
  {
    Fixture TestFixture;
    AsyncResultStub InvalidFinal{
        Core::AgentRunResult::ResponseReady(
            Providers::CompletionKind::FinalAnswer, "")};
    const auto Result = RunWithRuntimeStub(
        TestFixture, TestFixture.Inbound(), InvalidFinal);
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.Failure == Core::CoreChatFailureCode::InvalidAgentResult &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "empty Runtime final text is rejected before Core action creation");
  }
  return Passed;
}

[[nodiscard]] bool TestCorePolicyAndReceiptStateCoverage() {
  bool Passed = true;
  {
    Fixture TestFixture;
    TestFixture.Channel.SetFaults(0, false, true);
    const auto Result = TestFixture.RunDefaultAsync();
    Passed &= Check(Result.Status == Core::CoreChatStatus::UnknownOutcome &&
                        Result.Operation == Core::OperationState::UnknownOutcome,
                    "unmatched channel receipt cannot complete an operation");
  }
  {
    Fixture TestFixture;
    TestFixture.Channel.SetFaults(2);
    const auto Result = TestFixture.RunDefaultAsync();
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.Failure == Core::CoreChatFailureCode::CapabilityUnavailable &&
                        TestFixture.IdSource.NextSequence.load() == 1,
                    "missing send capability prevents ID allocation and action dispatch");
  }
  {
    Fixture TestFixture;
    TestFixture.Channel.SetFaults(3);
    const auto Result = TestFixture.RunDefaultAsync();
    const auto Records = TestFixture.OperationObserver.Snapshot();
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.Failure == Core::CoreChatFailureCode::CapabilityUnavailable &&
                        !Records.empty() &&
                        Records.back().GetState() == Core::OperationState::Failed &&
                        TestFixture.Channel.GetSendActionCallCount() == 0,
                    "capability change after authorization fails the operation before send");
  }
  struct ReceiptCase final {
    Channels::ReceiptStatus ReceiptStatus;
    Channels::ChannelErrorCode ErrorCode;
    Core::CoreChatStatus CoreStatus;
    Core::OperationState OperationState;
  };
  const std::vector<ReceiptCase> ReceiptCases{
      {Channels::ReceiptStatus::Rejected,
       Channels::ChannelErrorCode::PlatformRejected,
       Core::CoreChatStatus::Rejected, Core::OperationState::Rejected},
      {Channels::ReceiptStatus::UnsupportedCapability,
       Channels::ChannelErrorCode::UnsupportedCapability,
       Core::CoreChatStatus::Unsupported, Core::OperationState::Unsupported},
      {Channels::ReceiptStatus::Failed,
       Channels::ChannelErrorCode::TransportFailure,
       Core::CoreChatStatus::Failed, Core::OperationState::Failed},
  };
  for (const ReceiptCase& Expected : ReceiptCases) {
    Fixture TestFixture;
    TestFixture.Channel.SetReceiptOutcome(Expected.ReceiptStatus,
                                          Expected.ErrorCode);
    const auto Result = TestFixture.RunDefaultAsync();
    const auto Records = TestFixture.OperationObserver.Snapshot();
    const auto Sends = TestFixture.Channel.GetSendObservations();
    const bool ExactOperation =
        !Records.empty() &&
        Records.back().GetState() == Expected.OperationState &&
        Records.back().GetActionId().Value == "async-action-1" &&
        Records.back().GetOperationId().Value == "async-operation-1" &&
        Records.back().GetIdempotencyKey().Value == "async-idempotency-1" &&
        Records.back().GetPolicyVersion() == 3 &&
        Records.back().GetCapabilitySnapshotVersion() == 9;
    const bool ExactSendReceipt =
        Sends.size() == 1 && Sends.front().ActionId == "async-action-1" &&
        Sends.front().OperationId == "async-operation-1" &&
        Sends.front().IdempotencyKey == "async-idempotency-1" &&
        Sends.front().ReceiptActionId == "async-action-1" &&
        Sends.front().ReceiptOperationId == "async-operation-1" &&
        Sends.front().ReceiptStatus == Expected.ReceiptStatus;
    Passed &= Check(Result.Status == Expected.CoreStatus &&
                        Result.Operation == Expected.OperationState &&
                        ExactOperation && ExactSendReceipt &&
                        TestFixture.Channel.GetSendActionCallCount() == 1 &&
                        TestFixture.IdSource.NextSequence.load() == 2,
                    "non-success receipt maps to exact Core and OperationRecord state without retry");
  }
  return Passed;
}

[[nodiscard]] bool TestBusinessActionSchemaParserEdges() {
  bool Passed = true;
  const Providers::ToolDefinition FullSchema = MakeRegistrySchemaTool(
      R"({"type":"object","properties":{"query":{"type":"string"},"amount":{"type":"number"},"count":{"type":"integer"},"active":{"type":"boolean"},"empty":{"type":"null"}},"required":["query"],"additionalProperties":true})");

  const Providers::JsonValue AllJsonKindsAndEscapes{
      R"({"query":"quote \" slash \\ / backspace \b formfeed \f newline \n return \r tab \t latin \u00e9 euro \u20AC face \uD83D\uDE00","amount":-1.25e+2,"count":-5,"active":true,"empty":null,"extra":{"items":[false,null,0,2.5,1E-2,{"key":"value"}]}})"};
  Passed &= Check(Core::BusinessActionRegistry::IsValidToolDefinition(FullSchema),
                  "the five supported primitive schema types form a valid tool definition");
  Passed &= Check(Core::BusinessActionRegistry::ValidateArguments(
                      AllJsonKindsAndEscapes, FullSchema),
                  "JSON parser accepts escapes, Unicode surrogate pairs, nested values and numeric forms");

  const std::vector<std::string> LegacyRequiredFields{"query"};
  const std::vector<std::string> LegacyValidJson{
      R"({"query":"escaped \" \\ / \b \f \n \r \t \u00e9 \u20AC \uD83D\uDE00"})",
      R"({"query":"ok","nested":{"items":[true,false,null,-1.25e+2,0,2.5,1E-2]}})",
      R"({ "query" : "ok" , "array" : [] , "object" : {} })",
  };
  for (std::size_t Index = 0; Index < LegacyValidJson.size(); ++Index) {
    Passed &= Check(Core::BusinessActionRegistry::ValidateArguments(
                        LegacyValidJson[Index], LegacyRequiredFields),
                    "legacy JSON parser accepts valid nested/escaped input " +
                        std::to_string(Index));
  }
  const std::vector<std::string> LegacyInvalidJson{
      "",
      "[]",
      R"({"query":})",
      R"({"query":"bad\q"})",
      R"json({"query":"truncated escape\)json",
      R"({"query":"\u12"})",
      R"({"query":"unterminated})",
      R"({"query":"ok","number":01})",
      R"({"query":"ok","number":-})",
      R"({"query":"ok","number":1.})",
      R"({"query":"ok","number":1e})",
      R"({"query":"ok","flag":tru})",
      R"({"query":"ok",})",
      R"({"query":"ok","query":"duplicate"})",
      R"({"nested":[1]})",
      R"({"query":"ok"} trailing)",
  };
  std::vector<std::string> LegacyInvalidCases = LegacyInvalidJson;
  std::string LegacyDeepArray{R"({"query":"ok","nested":)"};
  LegacyDeepArray.append(40U, '[');
  LegacyDeepArray.push_back('0');
  LegacyDeepArray.append(40U, ']');
  LegacyDeepArray.push_back('}');
  LegacyInvalidCases.push_back(std::move(LegacyDeepArray));
  for (std::size_t Index = 0; Index < LegacyInvalidCases.size(); ++Index) {
    Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                        LegacyInvalidCases[Index], LegacyRequiredFields),
                    "legacy JSON parser rejects malformed input " +
                        std::to_string(Index));
  }
  const std::string LegacyRawControl = "{\"query\":\"bad\ncontrol\"}";
  Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                      LegacyRawControl, LegacyRequiredFields),
                  "legacy JSON parser rejects unescaped control characters");
  Passed &= Check(Core::BusinessActionRegistry::ValidateArguments(
                      R"({"optional":true})", std::vector<std::string>{}),
                  "legacy JSON parser allows a valid object with no required fields");
  Passed &= Check(Core::BusinessActionRegistry::ValidateArguments(
                      "{}", std::vector<std::string>{}),
                  "legacy JSON parser accepts an empty object when no fields are required");

  const std::vector<Providers::JsonValue> WrongTypes{
      {R"({"query":7})"},
      {R"({"query":"x","amount":"2"})"},
      {R"({"query":"x","count":1.1})"},
      {R"({"query":"x","count":1e-1})"},
      {R"({"query":"x","count":1.0000000000000000001})"},
      {R"({"query":"x","count":9007199254740992.1})"},
      {R"({"query":"x","active":"true"})"},
      {R"({"query":"x","empty":false})"},
      {R"({"amount":1,"count":1,"active":true,"empty":null})"},
  };
  for (std::size_t Index = 0; Index < WrongTypes.size(); ++Index) {
    Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                        WrongTypes[Index], FullSchema),
                    "schema argument type/required rejection case " +
                        std::to_string(Index));
  }
  const std::vector<std::string> MathematicalIntegers{
      R"({"query":"x","count":1.0})",
      R"({"query":"x","count":1e0})",
      R"({"query":"x","count":1e-0})",
      R"({"query":"x","count":10e-1})",
      R"({"query":"x","count":10e-0001})",
      R"({"query":"x","count":1.2300e2})",
      R"({"query":"x","count":100e-2})",
      R"({"query":"x","count":-0.00e-999999999999999999999})",
      R"({"query":"x","count":1e+999999999999999999999})",
      R"({"query":"x","count":9007199254740992.0})",
  };
  for (std::size_t Index = 0; Index < MathematicalIntegers.size(); ++Index) {
    Passed &= Check(Core::BusinessActionRegistry::ValidateArguments(
                        Providers::JsonValue{MathematicalIntegers[Index]},
                        FullSchema),
                    "integer schema accepts mathematical integer value " +
                        std::to_string(Index));
  }

  const Providers::ToolDefinition NoExtrasSchema = MakeRegistrySchemaTool(
      R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})");
  Passed &= Check(Core::BusinessActionRegistry::ValidateArguments(
                      Providers::JsonValue{R"({"query":"ok"})"},
                      NoExtrasSchema),
                  "declared required properties are accepted when extras are disabled");
  Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                      Providers::JsonValue{R"({"query":"ok","extra":1})"},
                      NoExtrasSchema),
                  "additionalProperties false rejects undeclared fields");
  Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                      Providers::JsonValue{R"({"query":"ok","query":"duplicate"})"},
                      FullSchema),
                  "duplicate object keys are rejected after JSON key decoding");
  Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                      Providers::JsonValue{R"({"query":"ok","extra":{"key":1,"key":2}})"},
                      FullSchema),
                  "duplicate keys in nested objects are rejected");

  std::string DeepArray{R"({"query":"ok","extra":)"};
  DeepArray.append(40U, '[');
  DeepArray.push_back('0');
  DeepArray.append(40U, ']');
  DeepArray.push_back('}');

  const std::vector<Providers::JsonValue> InvalidJson{
      {""},
      {R"([])"},
      {R"({"query":)"},
      {R"({"query":"bad\q"})"},
      {R"({"query":"\u0x00"})"},
      {R"({"query":"\uDC00"})"},
      {R"({"query":"\uD800"})"},
      {R"({"query":"\uD800\u0041"})"},
      {R"({"query":"\u12"})"},
      {R"({"query":"unterminated})"},
      {R"({"query":"ok","extra":01})"},
      {R"({"query":"ok","extra":-})"},
      {R"({"query":"ok","extra":1.})"},
      {R"({"query":"ok","extra":1e})"},
      {R"({"query":"ok","extra":tru})"},
      {R"({"query":"ok",})"},
      Providers::JsonValue{DeepArray},
  };
  for (std::size_t Index = 0; Index < InvalidJson.size(); ++Index) {
    Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                        InvalidJson[Index], FullSchema),
                    "malformed JSON parser rejection case " +
                        std::to_string(Index));
  }
  const std::string RawControlCharacter = "{\"query\":\"bad\ncontrol\"}";
  Passed &= Check(!Core::BusinessActionRegistry::ValidateArguments(
                      Providers::JsonValue{RawControlCharacter}, FullSchema),
                  "unescaped JSON control characters are rejected");

  std::vector<std::string> InvalidSchemas{
      "",
      "not-json",
      R"([])",
      R"({"type":"object","properties":{},"required":[]})",
      R"({"type":2,"properties":{},"required":[],"additionalProperties":false})",
      R"({"type":"array","properties":{},"required":[],"additionalProperties":false})",
      R"({"type":"object","properties":[],"required":[],"additionalProperties":false})",
      R"({"type":"object","properties":{},"required":{},"additionalProperties":false})",
      R"({"type":"object","properties":{},"required":[],"additionalProperties":"false"})",
      R"({"type":"object","properties":{"bad field":{"type":"string"}},"required":[],"additionalProperties":false})",
      R"({"type":"object","properties":{"query":{}},"required":[],"additionalProperties":false})",
      R"({"type":"object","properties":{"query":{"type":"string","format":"email"}},"required":[],"additionalProperties":false})",
      R"({"type":"object","properties":{"query":{"type":true}},"required":[],"additionalProperties":false})",
      R"({"type":"object","properties":{"query":{"type":"array"}},"required":[],"additionalProperties":false})",
      R"({"type":"object","properties":{},"required":[2],"additionalProperties":false})",
      R"({"type":"object","properties":{"query":{"type":"string"}},"required":["bad field"],"additionalProperties":false})",
      R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query","query"],"additionalProperties":false})",
      R"({"type":"object","properties":{},"required":["query"],"additionalProperties":false})",
      R"({"type":"object","properties":{},"required":[],"additionalProperties":false,"unsupported":true})",
  };
  InvalidSchemas.push_back(
      "{\"type\":\"object\",\"properties\":{\"\":{\"type\":\"string\"}},\"required\":[],\"additionalProperties\":false}");
  InvalidSchemas.push_back(
      "{\"type\":\"object\",\"properties\":{\"" +
      std::string(65U, 'a') +
      "\":{\"type\":\"string\"}},\"required\":[],\"additionalProperties\":false}");
  for (std::size_t Index = 0; Index < InvalidSchemas.size(); ++Index) {
    Passed &= Check(!Core::BusinessActionRegistry::IsValidToolDefinition(
                        MakeRegistrySchemaTool(InvalidSchemas[Index])),
                    "unsupported tool schema rejection case " +
                        std::to_string(Index));
  }
  return Passed;
}

[[nodiscard]] bool TestCanonicalArgumentMeasurement() {
  const Providers::JsonValue Arguments{
      R"json({ "z" : "\u00E9", "a" : ["\n", 2] })json"};
  std::size_t CanonicalBytes = 0U;
  std::size_t NestedStringBytes = 0U;
  bool ExceedsLimit = false;
  const bool Measured = Core::BusinessActionRegistry::
      MeasureCanonicalArgumentJson(Arguments, 256U, 23U, 5U,
                                    CanonicalBytes, NestedStringBytes,
                                    ExceedsLimit);
  bool Passed = Check(Measured && !ExceedsLimit && CanonicalBytes == 23U &&
                          NestedStringBytes == 5U,
                      "canonical argument measure sorts keys, compacts whitespace, normalizes escapes and counts decoded nested UTF-8 strings");

  std::size_t SmallerCanonicalBytes = 0U;
  std::size_t SmallerNestedStringBytes = 0U;
  bool SmallerExceedsLimit = false;
  const bool SmallerMeasured = Core::BusinessActionRegistry::
      MeasureCanonicalArgumentJson(Arguments, 256U, 22U, 5U,
                                    SmallerCanonicalBytes,
                                    SmallerNestedStringBytes,
                                    SmallerExceedsLimit);
  Passed &= Check(SmallerMeasured && SmallerExceedsLimit,
                  "canonical argument counting stops when the remaining byte allowance is exceeded");

  const Providers::JsonValue DuplicateKeys{
      R"json({"query":"x","\u0071uery":"y"})json"};
  std::size_t DuplicateCanonicalBytes = 0U;
  std::size_t DuplicateNestedStringBytes = 0U;
  bool DuplicateExceedsLimit = false;
  Passed &= Check(!Core::BusinessActionRegistry::MeasureCanonicalArgumentJson(
                      DuplicateKeys, 128U, 128U, 128U,
                      DuplicateCanonicalBytes, DuplicateNestedStringBytes,
                      DuplicateExceedsLimit),
                  "canonical argument counting rejects duplicate keys after Unicode escape decoding");
  return Passed;
}

[[nodiscard]] bool TestBusinessActionAsyncRegistrationEdges() {
  bool Passed = true;
  const auto Handler = [](const Core::BusinessActionInvocation&,
                          std::stop_token)
      -> Asio::awaitable<Core::BusinessActionExecutionResult> {
    co_return Core::BusinessActionExecutionResult{
        Core::BusinessActionExecutionStatus::Succeeded, "ReadOnly"};
  };

  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.EffectClass = Core::BusinessActionEffectClass::Unspecified;
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           IneligibleEffectClass &&
                        Registry.GetAsyncDefinitions().empty(),
                    "unspecified async effects cannot be registered or advertised");
  }
  for (const Core::BusinessActionEffectClass Effect : {
           Core::BusinessActionEffectClass::ExternalWrite,
           Core::BusinessActionEffectClass::ExternalIo}) {
    Core::BusinessActionRegistry Registry;
    const auto Error = Registry.Register(MakeAsyncLookup(Handler, Effect));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           IneligibleEffectClass &&
                        Registry.GetAsyncDefinitions().empty(),
                    "side-effecting or external-I/O async actions are rejected before advertisement");
  }
  for (const std::string& Name : {
           std::string{}, std::string(65U, 'A'), std::string{"Lookup-Profile"}}) {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.Name = Name;
    Definition.ModelTool.Name = Name;
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidActionName,
                    "empty, overlong, and punctuation-bearing action names are rejected");
  }
  for (const std::string& Name : {std::string{"FileCatalog"},
                                 std::string{"OpenBrowser"},
                                 std::string{"Mcp"}}) {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.Name = Name;
    Definition.ModelTool.Name = Name;
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           ForbiddenActionName,
                    "file, browser, and MCP action names are denied by exact, prefix, or Pascal-word rules");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.ModelTool.Name = "OtherAction";
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidToolDefinition,
                    "model tool name must match the Core registry name");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.RequiredArgumentFields = {"other"};
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidToolDefinition,
                    "declared required fields must match the canonical schema");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.RequiredCapabilities = {"Chat.Lookup", "Chat.Lookup"};
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidSchemaField,
                    "duplicate capability requirements are rejected");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.RequiredCapabilities = {""};
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidSchemaField,
                    "empty capability requirements are rejected");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.ModelTool.Parameters.JsonText = R"({"type":"object","properties":{},"required":[],"additionalProperties":false,"extra":true})";
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidToolDefinition,
                    "unsupported schema keywords fail registration");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.Name = "ReadFile";
    Definition.ModelTool.Name = "ReadFile";
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           ForbiddenActionName,
                    "generic file actions are forbidden before schema registration");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.Name = "lookupProfile";
    Definition.ModelTool.Name = "lookupProfile";
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidActionName,
                    "non-PascalCase action names fail registration");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Passed &= Check(!Registry.Register(Definition).has_value() &&
                        Registry.GetAsyncDefinitions().size() == 1U,
                    "a valid async read-only definition is available for advertisement");
    const auto Duplicate = Registry.Register(std::move(Definition));
    Passed &= Check(Duplicate.has_value() &&
                        Duplicate->Code == Core::BusinessActionRegistrationErrorCode::
                                               DuplicateAction,
                    "duplicate action names cannot replace the registered definition");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.AsyncHandler = {};
    Definition.Handler = {};
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           EmptyHandler,
                    "definitions without a sync or async handler are rejected");
  }
  const auto SyncHandler = [](const Core::BusinessActionInvocation&) {
    return Core::BusinessActionExecutionResult{
        Core::BusinessActionExecutionStatus::Succeeded, "SyncReadOnly"};
  };
  for (const std::vector<std::string>& RequiredFields : {
           std::vector<std::string>{"bad field"},
           std::vector<std::string>{"query", "query"}}) {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.AsyncHandler = {};
    Definition.Handler = SyncHandler;
    Definition.RequiredArgumentFields = RequiredFields;
    const auto Error = Registry.Register(std::move(Definition));
    Passed &= Check(Error.has_value() &&
                        Error->Code == Core::BusinessActionRegistrationErrorCode::
                                           InvalidSchemaField,
                    "sync-only registrations reject invalid or duplicate required argument fields");
  }
  {
    Core::BusinessActionRegistry Registry;
    auto Definition = MakeAsyncLookup(Handler);
    Definition.AsyncHandler = {};
    Definition.Handler = SyncHandler;
    Passed &= Check(!Registry.Register(std::move(Definition)).has_value() &&
                        Registry.GetAsyncDefinitions().empty(),
                    "sync-only registrations are not included in async model-tool advertisement");
  }
  return Passed;
}

[[nodiscard]] bool TestCoreAuthorizedReadOnlyToolRoundTrip() {
  Fixture TestFixture(FakeScenario::ToolRoundTrip);
  std::uint32_t HandlerCalls = 0;
  Core::BusinessActionInvocation ObservedInvocation;
  const auto Registration = TestFixture.BusinessActions.Register(
      MakeAsyncLookup(
          [&HandlerCalls, &ObservedInvocation](
              const Core::BusinessActionInvocation& Invocation,
              std::stop_token) -> Asio::awaitable<
                  Core::BusinessActionExecutionResult> {
            ++HandlerCalls;
            ObservedInvocation = Invocation;
            co_return Core::BusinessActionExecutionResult{
                Core::BusinessActionExecutionStatus::Succeeded,
                "ProfileFound"};
          }));
  if (!Check(!Registration.has_value(),
             "Core must accept a valid in-process read-only action")) {
    return false;
  }

  const Channels::InboundChannelMessage Inbound = TestFixture.Inbound();
  const Core::CoreChatResult Result = TestFixture.RunToolAsync(Inbound);
  const auto Requests = TestFixture.Provider.GetRequests();
  const auto ChannelReceipt = TestFixture.Channel.GetLastSendActionReceipt();
  const auto Operations = TestFixture.OperationObserver.Snapshot();
  bool Passed = true;
  Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                      Result.Completion ==
                          Providers::CompletionKind::FinalAnswer,
                  "Core completes the user turn after an authorized tool round trip");
  Passed &= Check(HandlerCalls == 1U && Requests.size() == 2U &&
                      TestFixture.Provider.DispatchCount.load() == 2U,
                  "one terminal-authorized action runs before a second provider request");
  Passed &= Check(
      ObservedInvocation.Principal.Value == "async-principal" &&
          ObservedInvocation.Scope.Conversation.Value == "async-conversation" &&
          ObservedInvocation.Turn.Value == MakeTurnId(Inbound.Envelope).Value &&
          ObservedInvocation.ToolCallId == "fake-tool-call-1" &&
          ObservedInvocation.Arguments.JsonText == R"({"query":"profile"})",
      "action receives the trusted Core identity, scope, turn and canonical arguments");
  Passed &= Check(Requests[0].ToolDefinitions.size() == 1U &&
                      Requests[0].ToolDefinitions.front().Name ==
                          "LookupProfile" &&
                      Requests[1].Id.Value != Requests[0].Id.Value &&
                      Requests[0].TurnId.Value == Requests[1].TurnId.Value,
                  "Core advertises only the registered tool and continuation uses a fresh request id on the same turn");
  Passed &= Check(
      Requests[1].Messages.size() == 3U &&
          Requests[1].Messages[0].Role == Providers::MessageRole::User &&
          Requests[1].Messages[1].Role == Providers::MessageRole::Assistant &&
          Requests[1].Messages[1].FunctionCalls.size() == 1U &&
          Requests[1].Messages[1].FunctionCalls.front().Id.Value ==
              "fake-tool-call-1" &&
          Requests[1].Messages[2].Role == Providers::MessageRole::Tool &&
          Requests[1].Messages[2].ToolCallResultId.has_value() &&
          Requests[1].Messages[2].ToolCallResultId->Value ==
              "fake-tool-call-1" &&
          std::get<Providers::TextContentPart>(
              Requests[1].Messages[2].ContentParts.front()).Text ==
              "ProfileFound",
      "only the correlated sanitized CoreActionReceipt becomes a tool result");
  Passed &= Check(ChannelReceipt.has_value() &&
                      ChannelReceipt->Status ==
                          Channels::ReceiptStatus::Succeeded,
                  "the final Mock Channel receipt is successful");
  Passed &= Check(TestFixture.Channel.GetSendActionCallCount() == 1U,
                  "the final user reply is sent exactly once");
  const bool OneFinalOperation =
      !Operations.empty() &&
      std::all_of(Operations.begin(), Operations.end(),
                  [](const Core::OperationRecord& Record) {
                    return Record.GetOperationId().Value ==
                               "async-operation-1" &&
                           Record.GetActionId().Value == "async-action-1";
                  }) &&
      Operations.back().GetState() == Core::OperationState::Succeeded;
  Passed &= Check(OneFinalOperation,
                  "only the final channel send creates one successful OperationRecord lifecycle; CoreActionReceipt creates none");
  Passed &= Check(!EmulativeDialog::Agent::OutputBudgetController::Shared()
                       .HasActiveTurn(MakeTurnId(Inbound.Envelope)),
                  "Core releases output-budget state after the complete async tool turn");
  const Core::CoreChatResult Duplicate =
      TestFixture.RunToolAsync(Inbound);
  Passed &= Check(Duplicate.Status == Core::CoreChatStatus::Duplicate &&
                      HandlerCalls == 1U &&
                      TestFixture.Provider.DispatchCount.load() == 2U &&
                      TestFixture.Channel.GetSendActionCallCount() == 1U,
                  "the shared Core dedupe reservation prevents a repeated async tool turn");
  return Passed;
}

[[nodiscard]] bool TestReadOnlyToolAuthorizationAndSchemaRejections() {
  bool Passed = true;
  {
    bool RejectedDuplicateGrant = false;
    try {
      static_cast<void>(Core::IdentityPolicy(
          3, {Core::IdentityBinding{
                  .Account = MakeChannelProfile(),
                  .Sender = Channels::SenderId{"async-sender"},
                  .Principal = Core::PrincipalId{"async-principal"},
                  .GrantedCapabilities = {"Chat.Lookup", "Chat.Lookup"},
              }}));
    } catch (const std::invalid_argument&) {
      RejectedDuplicateGrant = true;
    }
    Passed &= Check(RejectedDuplicateGrant,
                    "Core identity configuration rejects duplicate capability grants");
  }
  {
    const Core::IdentityPolicy LegacyPolicy(
        3, {Core::IdentityBinding{
                .Account = MakeChannelProfile(),
                .Sender = Channels::SenderId{"async-sender"},
                .Principal = Core::PrincipalId{"async-principal"},
            }});
    const Core::IdentityResolution LegacyResolution =
        LegacyPolicy.Resolve(MakeInboundMessage().Envelope);
    const auto* Resolved =
        std::get_if<Core::ResolvedIdentity>(&LegacyResolution);
    Passed &= Check(Resolved != nullptr &&
                        Resolved->GrantedCapabilities.empty(),
                    "legacy IdentityBinding aggregates default to no capability grants");
    if (Resolved != nullptr) {
      const Channels::InboundChannelMessage LegacyInbound = MakeInboundMessage();
      const Core::AuthorizedTurnContext LegacyContext{
          Resolved->Principal, Resolved->Scope,
          MakeTurnId(LegacyInbound.Envelope), LegacyInbound.Message};
      Passed &= Check(
          LegacyContext.GrantedCapabilities.empty(),
          "legacy AuthorizedTurnContext aggregates default to no capability grants");
    }
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip, {});
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ShouldNotRun"};
                })).has_value(),
          "read-only test action registers for an identity without its grant");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    const auto Requests = TestFixture.Provider.GetRequests();
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        HandlerCalls == 0U && Requests.size() == 2U &&
                        Requests.front().ToolDefinitions.empty() &&
                        std::get<Providers::TextContentPart>(
                            Requests.back().Messages.back().ContentParts.front())
                                .Text == "MissingCapability",
                    "model-proposed calls cannot supply missing Core capability grants");
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTripBadArguments);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ShouldNotRun"};
                })).has_value(),
          "schema test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    const auto Requests = TestFixture.Provider.GetRequests();
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        HandlerCalls == 0U && Requests.size() == 2U &&
                        std::get<Providers::TextContentPart>(
                            Requests.back().Messages.back().ContentParts.front())
                                .Text == "InvalidArguments",
                    "Core validates tool JSON types before invoking the handler");
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0;
    auto External = MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "Unsafe"};
        },
        Core::BusinessActionEffectClass::ExternalWrite);
    const auto Rejected = TestFixture.BusinessActions.Register(std::move(External));
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    const auto Requests = TestFixture.Provider.GetRequests();
    Passed &= Check(Rejected.has_value() &&
                        Rejected->Code ==
                            Core::BusinessActionRegistrationErrorCode::
                                IneligibleEffectClass &&
                        TestFixture.BusinessActions.Count() == 0U &&
                        HandlerCalls == 0U &&
                        Result.Status == Core::CoreChatStatus::Completed &&
                        Requests.front().ToolDefinitions.empty(),
                    "ExternalWrite is rejected before registration, advertisement or handler invocation");
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0;
    auto ExternalIo = MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "Unsafe"};
        },
        Core::BusinessActionEffectClass::ExternalIo);
    const auto Rejected = TestFixture.BusinessActions.Register(std::move(ExternalIo));
    Passed &= Check(Rejected.has_value() &&
                        Rejected->Code ==
                            Core::BusinessActionRegistrationErrorCode::
                                IneligibleEffectClass &&
                        HandlerCalls == 0U,
                    "ExternalIo registration fails closed without executing its handler");
  }
  return Passed;
}

[[nodiscard]] bool TestToolHandoffTerminalAndUnknownOutcomeRules() {
  bool Passed = true;
  {
    Fixture TestFixture(FakeScenario::ToolProposalFinal);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ShouldNotRun"};
                })).has_value(),
          "terminal-only test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::InvalidStream &&
                        HandlerCalls == 0U &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "ToolCallReady followed by a final terminal is discarded without dispatch");
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTripUnknownSecond);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ProfileFound"};
                })).has_value(),
          "unknown provider outcome test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::UnknownOutcome &&
                        HandlerCalls == 1U &&
                        TestFixture.Provider.DispatchCount.load() == 2U &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "unknown provider outcome after a read-only action stops without replay or send");
  }
  {
    Fixture TestFixture(FakeScenario::ToolCallDuplicate);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ShouldNotRun"};
                })).has_value(),
          "duplicate-call test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::InvalidStream &&
                        HandlerCalls == 0U &&
                        TestFixture.Provider.DispatchCount.load() == 1U,
                    "duplicate ToolCallIds invalidate the correlated proposal batch");
  }
  return Passed;
}

[[nodiscard]] bool TestV2TerminalFailureMatrix() {
  const std::vector<ScenarioExpectation> Cases{
      {FakeScenario::Final, Core::CoreChatStatus::Completed, std::nullopt, true},
      {FakeScenario::Refusal, Core::CoreChatStatus::Completed, std::nullopt, true},
      {FakeScenario::OutputLimited, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::OutputLimitReached, false},
      {FakeScenario::Failed, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::ProviderRejected, false},
      {FakeScenario::Cancelled, Core::CoreChatStatus::Cancelled, std::nullopt,
       false},
      {FakeScenario::Unknown, Core::CoreChatStatus::UnknownOutcome, std::nullopt,
       false},
      {FakeScenario::InvalidSequence, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::InvalidStream, false},
      {FakeScenario::UnsupportedCapabilities, Core::CoreChatStatus::Unsupported,
       std::nullopt, false},
      {FakeScenario::ToolHandoff, Core::CoreChatStatus::Failed,
       Core::AgentRunFailureCode::InvalidStream, false},
  };

  bool Passed = true;
  for (const ScenarioExpectation& Expected : Cases) {
    Fixture TestFixture(Expected.Scenario);
    const Core::CoreChatResult Result =
        TestFixture.RunToolAsync(TestFixture.Inbound());
    const bool FailureMatches =
        Expected.Failure.has_value()
            ? Result.AgentFailure == Expected.Failure
            : !Result.AgentFailure.has_value();
    const std::string Label =
        "V2 terminal scenario #" +
        std::to_string(static_cast<unsigned>(Expected.Scenario));
    Passed &= Check(Result.Status == Expected.Status && FailureMatches &&
                        TestFixture.Channel.GetSendActionCallCount() ==
                            (Expected.SendsAction ? 1U : 0U),
                    Label + " maps the terminal and send behavior");
  }
  return Passed;
}

[[nodiscard]] bool TestReadOnlyActionFailureAndUnknownAreNotReplayed() {
  bool Passed = true;
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Failed,
                      "ProfileUnavailable"};
                })).has_value(),
          "definite failure action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    const auto Requests = TestFixture.Provider.GetRequests();
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        HandlerCalls == 1U && Requests.size() == 2U &&
                        std::get<Providers::TextContentPart>(
                            Requests.back().Messages.back().ContentParts.front())
                                .Text == "ProfileUnavailable" &&
                        TestFixture.Channel.GetSendActionCallCount() == 1U,
                    "a definite read-only failure yields a sanitized Core receipt for the next request");
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::UnknownOutcome,
                      "MustNotLeak"};
                })).has_value(),
          "read-only unknown test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::InternalError &&
                        HandlerCalls == 1U &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "an impossible unknown action status stops without receipt, continuation or replay");
  }
  return Passed;
}

[[nodiscard]] bool TestToolCycleAndCallLimits() {
  bool Passed = true;
  {
    Fixture TestFixture(FakeScenario::ToolHandoffLoop);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ReadOnly"};
                })).has_value(),
          "loop-limit test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        HandlerCalls == 4U &&
                        TestFixture.Provider.DispatchCount.load() == 5U &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "the fifth handoff stops after four dispatched cycles");
  }
  {
    Fixture TestFixture(FakeScenario::ToolHandoffTwoCallsLoop);
    std::uint32_t HandlerCalls = 0U;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ReadOnly"};
                })).has_value(),
          "eight-call turn-bound test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        HandlerCalls == 8U &&
                        TestFixture.Provider.DispatchCount.load() == 5U &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "four handoff cycles dispatch exactly eight calls and reject the fifth cycle before a ninth call or reply");
  }
  {
    Fixture TestFixture(FakeScenario::ToolCallOverflow);
    std::uint32_t HandlerCalls = 0;
    Passed &= Check(!TestFixture.BusinessActions.Register(MakeAsyncLookup(
                [&HandlerCalls](const Core::BusinessActionInvocation&,
                               std::stop_token)
                    -> Asio::awaitable<Core::BusinessActionExecutionResult> {
                  ++HandlerCalls;
                  co_return Core::BusinessActionExecutionResult{
                      Core::BusinessActionExecutionStatus::Succeeded,
                      "ShouldNotRun"};
                })).has_value(),
          "call-limit test action registers");
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        HandlerCalls == 0U &&
                        TestFixture.Provider.DispatchCount.load() == 1U,
                    "the ninth proposed call is rejected before any handler runs");
  }
  return Passed;
}

[[nodiscard]] bool TestV3PerRequestResourceLimits() {
  constexpr std::size_t MaximumEvents = 1024U;
  constexpr std::size_t MaximumDynamicBytes = 256U * 1024U;
  constexpr std::size_t MaximumTextBytes = 64U * 1024U;
  constexpr std::size_t MaximumArgumentBytes = 64U * 1024U;
  constexpr std::size_t ReceiptBoundaryCallIdBytes =
      (MaximumDynamicBytes - 13U - 13U - 6U) / 4U;
  static_assert(ReceiptBoundaryCallIdBytes == 65'528U);

  const auto EmitUsage = [](const Providers::CanonicalModelRequest& Request,
                            const Providers::EventSink& Sink,
                            std::uint64_t Sequence) {
    Providers::UsageUpdatedEvent Usage;
    Usage.Metadata = {Request.Id, Request.TurnId, Sequence};
    Usage.Usage.InputTokens = 5U;
    Usage.Usage.OutputTokens = 2U;
    Usage.Usage.TotalTokens = 7U;
    Usage.Usage.Completeness = Providers::UsageCompleteness::Final;
    Sink(Providers::CanonicalNonTerminalEvent{std::move(Usage)});
  };
  const auto MakeFinal = [](const Providers::CanonicalModelRequest& Request,
                            std::uint64_t Sequence)
      -> Providers::CanonicalTerminalEvent {
    Providers::ResponseCompletedEvent Final;
    Final.Metadata = {Request.Id, Request.TurnId, Sequence};
    Final.Kind = Providers::CompletionKind::FinalAnswer;
    Providers::UsageSnapshot Usage;
    Usage.InputTokens = 5U;
    Usage.OutputTokens = 2U;
    Usage.TotalTokens = 7U;
    Usage.Completeness = Providers::UsageCompleteness::Final;
    Final.FinalUsage = Usage;
    return Final;
  };
  const auto MakeHandoff = [](const Providers::CanonicalModelRequest& Request,
                              std::uint64_t Sequence)
      -> Providers::CanonicalTerminalEvent {
    Providers::ResponseCompletedEvent Handoff;
    Handoff.Metadata = {Request.Id, Request.TurnId, Sequence};
    Handoff.Kind = Providers::CompletionKind::ToolHandoff;
    Providers::UsageSnapshot Usage;
    Usage.InputTokens = 5U;
    Usage.OutputTokens = 2U;
    Usage.TotalTokens = 7U;
    Usage.Completeness = Providers::UsageCompleteness::Final;
    Handoff.FinalUsage = Usage;
    return Handoff;
  };
  const auto MakeProviderFailure = [](
      const Providers::CanonicalModelRequest& Request,
      std::uint64_t Sequence) -> Providers::CanonicalTerminalEvent {
    Providers::ResponseFailedEvent Failed;
    Failed.Metadata = {Request.Id, Request.TurnId, Sequence};
    Failed.Failure = {Providers::FailureCode::ProviderRejected, false,
                      "bounded resource test"};
    return Failed;
  };
  const auto EmitText = [](const Providers::CanonicalModelRequest& Request,
                           const Providers::EventSink& Sink,
                           std::uint64_t Sequence, std::string Text) {
    Providers::TextDeltaEvent Delta;
    Delta.Metadata = {Request.Id, Request.TurnId, Sequence};
    Delta.Item = Providers::ItemId{"resource-text"};
    Delta.Utf8Delta = std::move(Text);
    Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});
  };
  const auto MakeToolCall = [](const Providers::CanonicalModelRequest& Request,
                               std::uint64_t Sequence, std::string CallId,
                               std::string JsonText) {
    Providers::ToolCallReadyEvent Tool;
    Tool.Metadata = {Request.Id, Request.TurnId, Sequence};
    Tool.Item = Providers::ItemId{"resource-tool-item"};
    Tool.Call = Providers::ToolCallId{std::move(CallId)};
    Tool.ToolName = "LookupProfile";
    Tool.Arguments = Providers::JsonValue{std::move(JsonText)};
    return Providers::CanonicalNonTerminalEvent{std::move(Tool)};
  };
  const auto MakeSizedToolCall = [](
      const Providers::CanonicalModelRequest& Request,
      std::uint64_t Sequence, std::string CallId, std::string ToolName,
      std::string JsonText) {
    Providers::ToolCallReadyEvent Tool;
    Tool.Metadata = {Request.Id, Request.TurnId, Sequence};
    Tool.Item = Providers::ItemId{"sized-tool-item"};
    Tool.Call = Providers::ToolCallId{std::move(CallId)};
    Tool.ToolName = std::move(ToolName);
    Tool.Arguments = Providers::JsonValue{std::move(JsonText)};
    return Providers::CanonicalNonTerminalEvent{std::move(Tool)};
  };
  const auto EmitRefusal = [](const Providers::CanonicalModelRequest& Request,
                              const Providers::EventSink& Sink,
                              std::uint64_t Sequence, std::string Text) {
    Providers::RefusalDeltaEvent Delta;
    Delta.Metadata = {Request.Id, Request.TurnId, Sequence};
    Delta.Item = Providers::ItemId{"resource-refusal"};
    Delta.Utf8Delta = std::move(Text);
    Delta.Category = Providers::RefusalCategory::GeneralHarms;
    Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});
  };

  bool Passed = true;

  // The terminal is the 1,024th event and is admitted. The next nonterminal
  // event after 1,024 admitted events latches the stop before retention.
  {
    Fixture TestFixture;
    TestFixture.Provider.CustomEventScript =
        [MaximumEvents](const Providers::CanonicalModelRequest& Request,
           std::stop_token, const Providers::EventSink& Sink) {
          for (std::uint64_t Sequence = 1U; Sequence < MaximumEvents - 1U;
               ++Sequence) {
            Providers::ResponseStartedEvent Started;
            Started.Metadata = {Request.Id, Request.TurnId, Sequence};
            Sink(Providers::CanonicalNonTerminalEvent{std::move(Started)});
          }
          Providers::TextDeltaEvent Delta;
          Delta.Metadata = {Request.Id, Request.TurnId,
                            MaximumEvents - 1U};
          Delta.Item = Providers::ItemId{"event-limit-final"};
          Delta.Utf8Delta = "ok";
          Sink(Providers::CanonicalNonTerminalEvent{std::move(Delta)});
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeFinal, MaximumEvents](
            const Providers::CanonicalModelRequest& Request) {
          return MakeFinal(Request, MaximumEvents);
        };
    TestFixture.Provider.BeforeCustomTerminal = [] {};
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                        TestFixture.Channel.GetSendActionCallCount() == 1U,
                    "1,023 nonterminal events plus terminal exactly fit the 1,024-event request limit");
  }
  {
    Fixture TestFixture;
    bool StopSeenAtOverflow = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [&StopSeenAtOverflow, EmitText,
         MaximumEvents](const Providers::CanonicalModelRequest& Request,
                              std::stop_token Token,
                              const Providers::EventSink& Sink) {
          for (std::uint64_t Sequence = 1U; Sequence <= MaximumEvents;
               ++Sequence) {
            Providers::ResponseStartedEvent Started;
            Started.Metadata = {Request.Id, Request.TurnId, Sequence};
            Sink(Providers::CanonicalNonTerminalEvent{std::move(Started)});
          }
          EmitText(Request, Sink, MaximumEvents + 1U, "must be ignored");
          StopSeenAtOverflow = Token.stop_requested();
          EmitText(Request, Sink, MaximumEvents + 2U,
                   "ignored-after-limit");
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeFinal, MaximumEvents](
            const Providers::CanonicalModelRequest& Request) {
          return MakeFinal(Request, MaximumEvents + 3U);
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto Result = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal);
    Passed &= Check(StopSeenAtOverflow &&
                        Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "event 1,025 latches stop, ignores later callbacks, awaits the definitive terminal, and never replies");
  }

  // Text/refusal/safe-generated text share the exact 64 KiB accumulator cap.
  {
    Fixture TestFixture;
    TestFixture.Provider.CustomEventScript =
        [EmitText, MaximumTextBytes](
            const Providers::CanonicalModelRequest& Request,
                   std::stop_token, const Providers::EventSink& Sink) {
          EmitText(Request, Sink, 1U, std::string(MaximumTextBytes, 't'));
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeFinal](const Providers::CanonicalModelRequest& Request) {
          return MakeFinal(Request, 2U);
        };
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        TestFixture.Channel.GetSendActionCallCount() == 1U &&
                        TestFixture.Channel.GetSendObservations().front()
                                .TextPayload.size() == MaximumTextBytes,
                    "exactly 64 KiB of text is admitted and sent intact");
  }
  {
    Fixture TestFixture;
    bool StopSeenAfterOverflow = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [&StopSeenAfterOverflow, EmitText, MaximumTextBytes](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token Token, const Providers::EventSink& Sink) {
          EmitText(Request, Sink, 1U,
                   std::string(MaximumTextBytes + 1U, 'x'));
          StopSeenAfterOverflow = Token.stop_requested();
          EmitText(Request, Sink, 2U, "late-payload");
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeFinal](const Providers::CanonicalModelRequest& Request) {
          return MakeFinal(Request, 3U);
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto Result = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal);
    Passed &= Check(StopSeenAfterOverflow &&
                        Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "text over 64 KiB stops the provider, ignores later text, waits for terminal, and exposes no partial answer");
  }
  {
    Fixture TestFixture;
    bool StopSeen = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [EmitText, MaximumTextBytes, &StopSeen](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token Token, const Providers::EventSink& Sink) {
          EmitText(Request, Sink, 1U,
                   std::string(MaximumTextBytes + 1U, 'x'));
          StopSeen = Token.stop_requested();
        };
    TestFixture.Provider.CustomTerminalScript =
        [MaximumDynamicBytes](
            const Providers::CanonicalModelRequest& Request) {
          Providers::ResponseOutputLimitedEvent Limited;
          Limited.Metadata = {Request.Id, Request.TurnId, 2U};
          Limited.Kind = Providers::LimitKind::OutputTokens;
          Limited.SafeGeneratedText =
              std::string(MaximumDynamicBytes + 1U, 's');
          return Providers::CanonicalTerminalEvent{std::move(Limited)};
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto Result = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal);
    Passed &= Check(StopSeen && Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "after a resource latch, a definitive output-limited terminal is awaited and counted without retaining its oversized payload");
  }
  {
    Fixture TestFixture;
    TestFixture.Provider.CustomEventScript =
        [EmitText, EmitRefusal, MaximumTextBytes](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token, const Providers::EventSink& Sink) {
          const std::size_t Half = MaximumTextBytes / 2U;
          EmitText(Request, Sink, 1U, std::string(Half, 't'));
          EmitRefusal(Request, Sink, 2U,
                      std::string(MaximumTextBytes - Half, 'r'));
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeProviderFailure](
            const Providers::CanonicalModelRequest& Request) {
          return MakeProviderFailure(Request, 3U);
        };
    const auto ExactCombined = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(ExactCombined.Status == Core::CoreChatStatus::Failed &&
                        ExactCombined.AgentFailure ==
                            Core::AgentRunFailureCode::ProviderRejected &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "text and refusal deltas together use the inclusive 64 KiB combined-output limit");
  }
  {
    Fixture TestFixture;
    bool StopSeen = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [EmitText, EmitRefusal, MaximumTextBytes, &StopSeen](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token Token, const Providers::EventSink& Sink) {
          const std::size_t Half = MaximumTextBytes / 2U;
          EmitText(Request, Sink, 1U, std::string(Half, 't'));
          EmitRefusal(Request, Sink, 2U,
                      std::string(MaximumTextBytes - Half + 1U, 'r'));
          StopSeen = Token.stop_requested();
          EmitText(Request, Sink, 3U, "ignored-after-combined-limit");
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeProviderFailure](
            const Providers::CanonicalModelRequest& Request) {
          return MakeProviderFailure(Request, 4U);
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto OverCombined = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal);
    Passed &= Check(StopSeen && OverCombined.Status == Core::CoreChatStatus::Failed &&
                        OverCombined.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "text plus refusal byte 65,537 latches stop and waits for a definitive terminal without a partial reply");
  }
  {
    Fixture TestFixture;
    TestFixture.Provider.CustomEventScript =
        [MaximumTextBytes](const Providers::CanonicalModelRequest&,
           std::stop_token, const Providers::EventSink&) {};
    TestFixture.Provider.CustomTerminalScript =
        [MaximumTextBytes](const Providers::CanonicalModelRequest& Request) {
          Providers::ResponseOutputLimitedEvent Limited;
          Limited.Metadata = {Request.Id, Request.TurnId, 1U};
          Limited.Kind = Providers::LimitKind::OutputTokens;
          Limited.SafeGeneratedText = std::string(MaximumTextBytes, 's');
          return Providers::CanonicalTerminalEvent{std::move(Limited)};
        };
    const auto ExactSafeText = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(ExactSafeText.Status == Core::CoreChatStatus::Failed &&
                        ExactSafeText.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "safe generated text at exactly 64 KiB remains a non-sendable output-limited terminal");
  }
  {
    Fixture TestFixture;
    TestFixture.Provider.CustomEventScript =
        [MaximumTextBytes](const Providers::CanonicalModelRequest&,
           std::stop_token, const Providers::EventSink&) {};
    TestFixture.Provider.CustomTerminalScript =
        [MaximumTextBytes](const Providers::CanonicalModelRequest& Request) {
          Providers::ResponseOutputLimitedEvent Limited;
          Limited.Metadata = {Request.Id, Request.TurnId, 1U};
          Limited.Kind = Providers::LimitKind::OutputTokens;
          Limited.SafeGeneratedText = std::string(MaximumTextBytes + 1U, 's');
          return Providers::CanonicalTerminalEvent{std::move(Limited)};
        };
    const auto OverSafeText = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(OverSafeText.Status == Core::CoreChatStatus::Failed &&
                        OverSafeText.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "safe generated text over 64 KiB fails closed without a receipt");
  }

  // The ToolCallReady ID is retained by the accumulator pending set, the
  // coordinator's duplicate-ID set, and the pending FunctionCall. The tool
  // name, source JSON, and decoded JSON strings are retained separately.
  {
    Fixture TestFixture;
    bool StopSeen = false;
    TestFixture.Provider.CustomEventScript =
        [MaximumDynamicBytes, MakeSizedToolCall,
         &StopSeen](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token Token, const Providers::EventSink& Sink) {
          constexpr std::size_t ToolNameBytes = 15U;
          constexpr std::size_t SourceJsonBytes = 13U;
          constexpr std::size_t NestedJsonStringBytes = 6U;
          constexpr std::size_t FixedBytes =
              ToolNameBytes + SourceJsonBytes + NestedJsonStringBytes;
          const std::size_t CallIdBytes =
              (MaximumDynamicBytes - FixedBytes) / 3U;
          const std::string ToolCallId(CallIdBytes, 'c');
          Sink(MakeSizedToolCall(Request, 1U, ToolCallId,
                                 "LookupProfileXY",
                                 R"({"query":"p"})"));
          StopSeen = Token.stop_requested();
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeProviderFailure](
            const Providers::CanonicalModelRequest& Request) {
          return MakeProviderFailure(Request, 2U);
        };
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(!StopSeen && Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::ProviderRejected &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "exactly 262,144 retained bytes across the three ToolCallId copies are admitted");
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0U;
    static_cast<void>(TestFixture.BusinessActions.Register(MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "ReadOnly"};
        })));
    TestFixture.Provider.CustomEventScript =
        [MakeSizedToolCall, EmitUsage, EmitText,
         ReceiptBoundaryCallIdBytes](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token, const Providers::EventSink& Sink) {
          if (Request.Messages.size() == 1U) {
            const std::string ToolCallId(ReceiptBoundaryCallIdBytes, 'h');
            Sink(MakeSizedToolCall(Request, 1U, ToolCallId,
                                   "LookupProfile", R"({"query":"p"})"));
            EmitUsage(Request, Sink, 2U);
          } else {
            EmitText(Request, Sink, 1U, "done");
            EmitUsage(Request, Sink, 2U);
          }
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeHandoff, MakeFinal](
            const Providers::CanonicalModelRequest& Request)
            -> Providers::CanonicalTerminalEvent {
          return Request.Messages.size() == 1U ? MakeHandoff(Request, 3U)
                                              : MakeFinal(Request, 3U);
        };
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        HandlerCalls == 1U &&
                        TestFixture.Provider.DispatchCount.load() == 2U &&
                        TestFixture.Channel.GetSendActionCallCount() == 1U,
                    "a successful handoff admits exactly 262,144 retained bytes including the Core receipt ToolCallId copy");
  }
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0U;
    static_cast<void>(TestFixture.BusinessActions.Register(MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "MustNotRun"};
        })));
    TestFixture.Provider.CustomEventScript =
        [MakeSizedToolCall, EmitUsage, ReceiptBoundaryCallIdBytes](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token, const Providers::EventSink& Sink) {
          const std::string ToolCallId(ReceiptBoundaryCallIdBytes, 'h');
          Sink(MakeSizedToolCall(Request, 1U, ToolCallId,
                                 "LookupProfileX", R"({"query":"p"})"));
          EmitUsage(Request, Sink, 2U);
        };
    TestFixture.Provider.CustomTerminalScript = [MakeHandoff](
        const Providers::CanonicalModelRequest& Request) {
      return MakeHandoff(Request, 3U);
    };
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        HandlerCalls == 0U &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "the extra Core receipt ToolCallId copy makes 262,145 bytes, so the handoff is rejected before action dispatch");
  }
  {
    Fixture TestFixture;
    bool StopSeen = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [MaximumDynamicBytes, MakeSizedToolCall, &StopSeen](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token Token, const Providers::EventSink& Sink) {
          constexpr std::size_t ToolNameBytes = 16U;
          constexpr std::size_t SourceJsonBytes = 13U;
          constexpr std::size_t NestedJsonStringBytes = 6U;
          constexpr std::size_t FixedBytes =
              ToolNameBytes + SourceJsonBytes + NestedJsonStringBytes;
          const std::size_t CallIdBytes =
              (MaximumDynamicBytes - (FixedBytes - 1U)) / 3U;
          const std::string ToolCallId(CallIdBytes, 'c');
          Sink(MakeSizedToolCall(Request, 1U, ToolCallId,
                                 "LookupProfileXYZ",
                                 R"({"query":"p"})"));
          StopSeen = Token.stop_requested();
          Sink(MakeSizedToolCall(Request, 2U, "late-id", "LookupProfile",
                                 R"({"query":"late"})"));
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeProviderFailure](
            const Providers::CanonicalModelRequest& Request) {
          return MakeProviderFailure(Request, 3U);
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto Result = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal);
    Passed &= Check(StopSeen && Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "retained ToolCallId copies plus one extra tool-name byte reach 262,145, latch stop, ignore later input, and await terminal");
  }

  // Complete tool argument bytes use the exact compact canonical JSON length.
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0U;
    static_cast<void>(TestFixture.BusinessActions.Register(MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "ReadOnly"};
        })));
    const auto ExactJson = std::string(R"({"query":")") +
                           std::string(MaximumArgumentBytes - 12U, 'a') +
                           R"("})";
    TestFixture.Provider.CustomEventScript =
        [MakeToolCall, EmitText, EmitUsage, ExactJson](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token, const Providers::EventSink& Sink) {
          if (Request.Messages.size() == 1U) {
            Sink(MakeToolCall(Request, 1U, "exact-argument", ExactJson));
            EmitUsage(Request, Sink, 2U);
          } else {
            EmitText(Request, Sink, 1U, "done");
            EmitUsage(Request, Sink, 2U);
          }
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeHandoff, MakeFinal](
            const Providers::CanonicalModelRequest& Request)
            -> Providers::CanonicalTerminalEvent {
          return Request.Messages.size() == 1U ? MakeHandoff(Request, 3U)
                                              : MakeFinal(Request, 3U);
        };
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        HandlerCalls == 1U &&
                        TestFixture.Provider.DispatchCount.load() == 2U &&
                        TestFixture.Channel.GetSendActionCallCount() == 1U,
                    "64 KiB of compact canonical complete-tool JSON is admitted and dispatched");
  }
  {
    Fixture TestFixture;
    std::uint32_t HandlerCalls = 0U;
    static_cast<void>(TestFixture.BusinessActions.Register(MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "MustNotRun"};
        })));
    const auto OverJson = std::string(R"({"query":")") +
                          std::string(MaximumArgumentBytes - 11U, 'a') +
                          R"("})";
    bool StopSeen = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [MakeToolCall, EmitUsage, OverJson,
         &StopSeen](const Providers::CanonicalModelRequest& Request,
                    std::stop_token Token, const Providers::EventSink& Sink) {
          Sink(MakeToolCall(Request, 1U, "over-argument", OverJson));
          StopSeen = Token.stop_requested();
          EmitUsage(Request, Sink, 2U);
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeHandoff](const Providers::CanonicalModelRequest& Request) {
          return MakeHandoff(Request, 3U);
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto Result = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal);
    Passed &= Check(StopSeen && Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        HandlerCalls == 0U &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "the 65,537th canonical tool-argument byte stops the provider and prevents every dispatch or receipt");
  }

  // Eight complete calls are admitted; an incomplete ninth consumes the
  // ninth pending slot and prevents dispatch of all eight earlier proposals.
  {
    Fixture TestFixture(FakeScenario::ToolRoundTrip);
    std::uint32_t HandlerCalls = 0U;
    static_cast<void>(TestFixture.BusinessActions.Register(MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "ReadOnly"};
        })));
    TestFixture.Provider.CustomEventScript =
        [MakeToolCall, EmitText, EmitUsage](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token, const Providers::EventSink& Sink) {
          if (Request.Messages.size() == 1U) {
            for (std::uint64_t Index = 1U; Index <= 8U; ++Index) {
              Sink(MakeToolCall(Request, Index,
                                "pending-" + std::to_string(Index),
                                R"({"query":"p"})"));
            }
            EmitUsage(Request, Sink, 9U);
          } else {
            EmitText(Request, Sink, 1U, "done");
            EmitUsage(Request, Sink, 2U);
          }
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeHandoff, MakeFinal](
            const Providers::CanonicalModelRequest& Request)
            -> Providers::CanonicalTerminalEvent {
          return Request.Messages.size() == 1U ? MakeHandoff(Request, 10U)
                                              : MakeFinal(Request, 3U);
        };
    const auto Result = TestFixture.RunToolAsync(TestFixture.Inbound());
    Passed &= Check(Result.Status == Core::CoreChatStatus::Completed &&
                        HandlerCalls == 8U &&
                        TestFixture.Provider.DispatchCount.load() == 2U &&
                        TestFixture.Channel.GetSendActionCallCount() == 1U,
                    "all eight distinct pending tool calls are admitted and dispatched at the inclusive limit");
  }
  {
    Fixture TestFixture;
    std::uint32_t HandlerCalls = 0U;
    static_cast<void>(TestFixture.BusinessActions.Register(MakeAsyncLookup(
        [&HandlerCalls](const Core::BusinessActionInvocation&,
                       std::stop_token)
            -> Asio::awaitable<Core::BusinessActionExecutionResult> {
          ++HandlerCalls;
          co_return Core::BusinessActionExecutionResult{
              Core::BusinessActionExecutionStatus::Succeeded, "MustNotRun"};
        })));
    bool StopSeen = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [MakeToolCall, EmitUsage, &StopSeen](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token Token, const Providers::EventSink& Sink) {
          for (std::uint64_t Index = 1U; Index <= 8U; ++Index) {
            Sink(MakeToolCall(Request, Index,
                              "pending-" + std::to_string(Index),
                              R"({"query":"p"})"));
          }
          Providers::ToolCallStartedEvent Ninth;
          Ninth.Metadata = {Request.Id, Request.TurnId, 9U};
          Ninth.Item = Providers::ItemId{"ninth-pending"};
          Ninth.Call = Providers::ToolCallId{"pending-9"};
          Ninth.ToolName = "LookupProfile";
          Sink(Providers::CanonicalNonTerminalEvent{std::move(Ninth)});
          StopSeen = Token.stop_requested();
          Sink(MakeToolCall(Request, 10U, "pending-9",
                            R"({"query":"late"})"));
          EmitUsage(Request, Sink, 11U);
        };
    TestFixture.Provider.CustomTerminalScript =
        [MakeHandoff](const Providers::CanonicalModelRequest& Request) {
          return MakeHandoff(Request, 12U);
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto Result = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal);
    Passed &= Check(StopSeen && Result.Status == Core::CoreChatStatus::Failed &&
                        Result.AgentFailure ==
                            Core::AgentRunFailureCode::OutputLimitReached &&
                        HandlerCalls == 0U &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "an incomplete ninth call latches the pending limit before retention and prevents dispatch, receipt, and continuation");
  }

  // An uncertain terminal wins over a local text limit and caller cancellation.
  {
    Fixture TestFixture;
    std::stop_source CallerCancellation;
    bool StopSeen = false;
    bool CoreReturnedBeforeTerminal = false;
    TestFixture.Provider.CustomEventScript =
        [EmitText, &StopSeen, &CallerCancellation](
            const Providers::CanonicalModelRequest& Request,
            std::stop_token Token, const Providers::EventSink& Sink) {
          EmitText(Request, Sink, 1U, std::string(MaximumTextBytes + 1U, 'u'));
          StopSeen = Token.stop_requested();
          CallerCancellation.request_stop();
          EmitText(Request, Sink, 2U, "ignored-unknown-path");
        };
    TestFixture.Provider.CustomTerminalScript =
        [](const Providers::CanonicalModelRequest& Request) {
          Providers::ResponseOutcomeUnknownEvent Unknown;
          Unknown.Metadata = {Request.Id, Request.TurnId, 3U};
          Unknown.Reason.Code.Value = "uncertain-after-limit";
          return Providers::CanonicalTerminalEvent{std::move(Unknown)};
        };
    TestFixture.Provider.CustomTerminalDelay = std::chrono::milliseconds(5);
    const auto Result = TestFixture.RunToolAsyncObservingTerminal(
        CoreReturnedBeforeTerminal, CallerCancellation.get_token());
    Passed &= Check(StopSeen &&
                        Result.Status == Core::CoreChatStatus::UnknownOutcome &&
                        TestFixture.Provider.DispatchCount.load() == 1U &&
                        TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                        !CoreReturnedBeforeTerminal &&
                        TestFixture.Channel.GetSendActionCallCount() == 0U,
                    "an uncertain terminal is counted and returned as UnknownOutcome ahead of a latched limit and caller cancellation");
  }
  return Passed;
}

[[nodiscard]] bool TestRuntimeExceptionAfterProviderMayHaveDispatchedIsUnknown() {
  Fixture TestFixture;
  ThrowAfterProviderDispatchRuntime Runtime;
  Asio::io_context Context;
  std::optional<Core::CoreChatResult> Result;
  std::exception_ptr Failure;
  Core::AsyncAgentToolRuntimePort& ToolRuntime = Runtime;
  Asio::co_spawn(
      Context,
      TestFixture.Service.ProcessAsync(TestFixture.Inbound(), ToolRuntime,
                                       TestFixture.Provider),
      [&Result, &Failure](std::exception_ptr Exception,
                          Core::CoreChatResult Value) {
        Failure = std::move(Exception);
        Result = std::move(Value);
      });
  Context.run();
  if (Failure) {
    std::rethrow_exception(Failure);
  }
  return Check(Result.has_value() &&
                   Result->Status == Core::CoreChatStatus::UnknownOutcome &&
                   TestFixture.Provider.DispatchCount.load() == 1U &&
                   TestFixture.Provider.FinalTerminalCount.load() == 1U &&
                   TestFixture.Channel.GetSendActionCallCount() == 0U &&
                   TestFixture.IdSource.NextSequence.load() == 1U &&
                   TestFixture.OperationObserver.Snapshot().empty() &&
                   !EmulativeDialog::Agent::OutputBudgetController::Shared()
                        .HasActiveTurn(MakeTurnId(TestFixture.Inbound().Envelope)),
               "a V2 runtime exception after a provider request may have started is UnknownOutcome with no retry, operation receipt, or channel send");
}

[[nodiscard]] bool TestCancellationDuringReadOnlyAction() {
  Fixture TestFixture(FakeScenario::ToolRoundTrip);
  std::stop_source StopSource;
  std::uint32_t HandlerCalls = 0;
  const bool ActionRegistered =
      !TestFixture.BusinessActions.Register(MakeAsyncLookup(
              [&HandlerCalls, &StopSource](
                  const Core::BusinessActionInvocation&,
                  std::stop_token) -> Asio::awaitable<
                      Core::BusinessActionExecutionResult> {
                ++HandlerCalls;
                const auto Executor = co_await Asio::this_coro::executor;
                Asio::steady_timer Timer(Executor);
                Timer.expires_after(std::chrono::milliseconds(1));
                co_await Timer.async_wait(Asio::use_awaitable);
                StopSource.request_stop();
                co_return Core::BusinessActionExecutionResult{
                    Core::BusinessActionExecutionStatus::Succeeded,
                    "ProfileFound"};
              })).has_value();
  const auto Result =
      TestFixture.RunToolAsync(TestFixture.Inbound(), StopSource.get_token());
  return Check(ActionRegistered &&
                   Result.Status == Core::CoreChatStatus::Cancelled &&
                   HandlerCalls == 1U &&
                   TestFixture.Provider.DispatchCount.load() == 1U &&
                   TestFixture.Channel.GetSendActionCallCount() == 0U,
               "cancellation during a read-only handler creates no receipt, continuation or channel send");
}

[[nodiscard]] bool TestConcreteAsyncRuntimePreservesLegacyConstructor() {
  Fixture TestFixture(FakeScenario::Final);
  Core::CoreChatService LegacyService(
      TestFixture.Policy, TestFixture.SyncRuntime, TestFixture.SyncProvider,
      TestFixture.ProviderProfile, TestFixture.ProviderLease,
      TestFixture.Channel, TestFixture.ChannelLease, TestFixture.IdSource,
      &TestFixture.OperationObserver);

  Asio::io_context Context;
  std::optional<Core::CoreChatResult> Result;
  std::exception_ptr Failure;
  Asio::co_spawn(
      Context,
      LegacyService.ProcessAsync(TestFixture.Inbound(), TestFixture.Runtime,
                                 TestFixture.Provider),
      [&Result, &Failure](std::exception_ptr Exception,
                          Core::CoreChatResult Value) {
        Failure = std::move(Exception);
        Result = std::move(Value);
      });
  Context.run();
  if (Failure) {
    std::rethrow_exception(Failure);
  }
  return Check(Result.has_value() &&
                   Result->Status == Core::CoreChatStatus::Completed &&
                   TestFixture.Provider.DispatchCount.load() == 1U &&
                   TestFixture.Channel.GetSendActionCallCount() == 1U,
               "a concrete AsyncAgentRuntime keeps the V1 path with the original CoreChatService constructor");
}

bool RunAllTests() {
  bool Passed = true;
  RunLegacyToolCallRoundTripTests();
  Passed &= TestSuccessfulMockToMockCoroutineSlice();
  Passed &= TestTerminalAndProviderFailureMatrix();
  Passed &= TestCancellationAndInputOrdering();
  Passed &= TestSyncCompatibilityAndMixedDuplicateRace();
  Passed &= TestDistinctConcurrentMessagesAndIdentifierSafety();
  Passed &= TestChannelAndTerminalValidationFailures();
  Passed &= TestCorePolicyAndReceiptStateCoverage();
  Passed &= TestBusinessActionSchemaParserEdges();
  Passed &= TestCanonicalArgumentMeasurement();
  Passed &= TestBusinessActionAsyncRegistrationEdges();
  Passed &= TestCoreAuthorizedReadOnlyToolRoundTrip();
  Passed &= TestReadOnlyToolAuthorizationAndSchemaRejections();
  Passed &= TestToolHandoffTerminalAndUnknownOutcomeRules();
  Passed &= TestV2TerminalFailureMatrix();
  Passed &= TestReadOnlyActionFailureAndUnknownAreNotReplayed();
  Passed &= TestToolCycleAndCallLimits();
  Passed &= TestV3PerRequestResourceLimits();
  Passed &= TestCancellationDuringReadOnlyAction();
  Passed &= TestRuntimeExceptionAfterProviderMayHaveDispatchedIsUnknown();
  Passed &= TestConcreteAsyncRuntimePreservesLegacyConstructor();
  return Passed;
}

} // namespace

int main() {
  return RunAllTests() ? 0 : 1;
}
