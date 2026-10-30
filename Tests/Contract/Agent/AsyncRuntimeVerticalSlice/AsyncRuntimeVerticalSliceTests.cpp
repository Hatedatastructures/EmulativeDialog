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
    if (Profile.Id.Value.empty() || Profile.ModelName.empty() ||
        Lease.GetAuthenticationSecret().empty() || Request.Messages.size() != 1) {
      Providers::ResponseFailedEvent Failed;
      Failed.Metadata = {Request.Id, Request.TurnId, 1};
      Failed.Failure = {Providers::FailureCode::InvalidRequest, false,
                        "invalid async fake fixture"};
      co_return Failed;
    }

    const StreamActivity Activity{ActiveStreamCount, MaxActiveStreamCount};
    ++DispatchCount;
    if (OnDispatched) {
      OnDispatched();
    }
    {
      std::lock_guard Lock(RequestMutex);
      LastRequestText.clear();
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

  std::atomic<std::uint32_t> DispatchCount{0};
  std::atomic<std::uint32_t> ActiveStreamCount{0};
  std::atomic<std::uint32_t> MaxActiveStreamCount{0};
  std::atomic<bool> AwaitingProvider{false};
  std::atomic<std::uint32_t> FinalTerminalCount{0};
  std::function<void()> OnDispatched;
  std::chrono::milliseconds StreamDelay{2};

private:
  FakeScenario Scenario;
  Providers::ProviderCapabilities Capabilities{
      true, true, true, true, true, true};
  mutable std::mutex RequestMutex;
  std::string LastRequestText;
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
  explicit Fixture(FakeScenario Scenario = FakeScenario::Final)
      : ChannelProfile(MakeChannelProfile()),
        Channel(MakeMockConfiguration(ChannelProfile)),
        ProviderProfile(MakeProviderProfile(Scenario)),
        Policy(3, {Core::IdentityBinding{
                       .Account = ChannelProfile,
                       .Sender = Channels::SenderId{"async-sender"},
                       .Principal = Core::PrincipalId{"async-principal"},
                   }}),
        Service(Policy, SyncRuntime, SyncProvider, ProviderProfile,
                ProviderLease, Channel, ChannelLease, IdSource,
                &OperationObserver),
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
        Service.ProcessAsync(std::move(Message), Runtime, Provider,
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
            Message, TestFixture.Runtime, TestFixture.Provider,
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
            Message, TestFixture.Runtime, TestFixture.Provider,
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
  bool ObservedAsyncWins = false;
  bool ObservedSyncWins = false;
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

    ObservedAsyncWins |= ActualWinner == RacePath::Async && WinnerCompleted &&
                         LoserWasDuplicate;
    ObservedSyncWins |= ActualWinner == RacePath::Sync && WinnerCompleted &&
                        LoserWasDuplicate;
    Passed &= Check(ActualWinner != RacePath::None &&
                        LoserReturnedBeforeRelease && LoserReturnedWhileHeld &&
                        !AsyncFailure && !SyncFailure && WinnerCompleted &&
                        LoserWasDuplicate && ExactlyOneRuntimePath &&
                        SingleRuntimeEntry &&
                        TestFixture.Channel.GetSendActionCallCount() == 1,
                    "both barrier-released API calls race reservation; loser returns Duplicate while winner is held");
  }

  Passed &= Check(ObservedAsyncWins && ObservedSyncWins,
                  "repeated mixed sync/async barrier races observe both winner orders");
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
        TestFixture.Service.ProcessAsync(FirstMessage, TestFixture.Runtime,
                                         TestFixture.Provider),
        [&FirstResult, &FirstFailure](std::exception_ptr Exception,
                                      Core::CoreChatResult Value) {
          FirstFailure = std::move(Exception);
          FirstResult = std::move(Value);
        });
    Asio::co_spawn(
        Context,
        TestFixture.Service.ProcessAsync(SecondMessage, TestFixture.Runtime,
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
        TestFixture.Service.ProcessAsync(AsyncMessage, TestFixture.Runtime,
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

bool RunAllTests() {
  bool Passed = true;
  Passed &= TestSuccessfulMockToMockCoroutineSlice();
  Passed &= TestTerminalAndProviderFailureMatrix();
  Passed &= TestCancellationAndInputOrdering();
  Passed &= TestSyncCompatibilityAndMixedDuplicateRace();
  Passed &= TestDistinctConcurrentMessagesAndIdentifierSafety();
  Passed &= TestChannelAndTerminalValidationFailures();
  Passed &= TestCorePolicyAndReceiptStateCoverage();
  return Passed;
}

} // namespace

int main() {
  return RunAllTests() ? 0 : 1;
}
