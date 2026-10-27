#include "Source/Core/CoreAuthorizedAction.h"
#include "Source/Core/CoreChatService.h"
#include "Source/Core/IdentityPolicy.h"
#include "Tests/Contract/Channels/Contracts/MockChannelAdapter.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace EmulativeDialog::Channels;
using EmulativeDialog::Channels::Testing::MockActionReceipt;
using EmulativeDialog::Channels::Testing::MockChannelAdapter;
using EmulativeDialog::Channels::Testing::MockChannelConfiguration;
using EmulativeDialog::Core::CoreActionIdentifiers;
using EmulativeDialog::Core::CoreActionIdSource;
using EmulativeDialog::Core::CoreChatResult;
using EmulativeDialog::Core::CoreChatService;
using EmulativeDialog::Core::CoreChatStatus;
using EmulativeDialog::Core::IdentityBinding;
using EmulativeDialog::Core::IdentityPolicy;
using EmulativeDialog::Core::PrincipalId;

void Require(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

class TestChannelCredentialLease final
    : public EmulativeDialog::Core::CredentialLease {
public:
  std::string_view GetAuthenticationSecret() const noexcept override {
    return {};
  }
};

class TestProviderCredentialLease final
    : public EmulativeDialog::Providers::CredentialLease {
public:
  std::string_view GetAuthenticationSecret() const noexcept override {
    return {};
  }
};

class FakeProvider final : public EmulativeDialog::Providers::ModelProviderPort {
public:
  EmulativeDialog::Providers::ProviderResult<
      EmulativeDialog::Providers::ProviderCapabilities>
  GetCapabilities(
      const EmulativeDialog::Providers::ProviderProfileId&) const override {
    return EmulativeDialog::Providers::ProviderCapabilities{};
  }

  EmulativeDialog::Providers::ProviderResult<std::monostate> ValidateProfile(
      const EmulativeDialog::Providers::ModelProviderProfile&) const override {
    return std::monostate{};
  }

  EmulativeDialog::Providers::CanonicalTerminalEvent StreamTurn(
      const EmulativeDialog::Providers::ModelProviderProfile&,
      const EmulativeDialog::Providers::CredentialLease&,
      const EmulativeDialog::Providers::CanonicalModelRequest&,
      std::stop_token,
      const EmulativeDialog::Providers::EventSink&) override {
    return EmulativeDialog::Providers::ResponseCompletedEvent{};
  }

  EmulativeDialog::Providers::ProviderResult<
      EmulativeDialog::Providers::ProviderHealthStatus>
  HealthCheck(
      const EmulativeDialog::Providers::ModelProviderProfile&) override {
    return EmulativeDialog::Providers::ProviderHealthStatus{
        EmulativeDialog::Providers::ProviderHealthState::Available};
  }
};

class FakeAgentRuntime final : public EmulativeDialog::Core::AgentRuntimePort {
public:
  EmulativeDialog::Core::AgentRunResult RunTurn(
      const EmulativeDialog::Core::AuthorizedTurnContext&,
      EmulativeDialog::Providers::ModelProviderPort&,
      const EmulativeDialog::Providers::ModelProviderProfile&,
      const EmulativeDialog::Providers::CredentialLease&,
      std::stop_token) override {
    ++Calls;
    return EmulativeDialog::Core::AgentRunResult::ResponseReady(
        EmulativeDialog::Providers::CompletionKind::FinalAnswer,
        "Synthetic authorized reply.");
  }

  std::size_t Calls = 0;
};

class ScriptedActionIdSource final : public CoreActionIdSource {
public:
  explicit ScriptedActionIdSource(std::vector<CoreActionIdentifiers> Values)
      : Values(std::move(Values)) {}

  CoreActionIdentifiers Next() override {
    ++Calls;
    if (NextIndex >= Values.size()) {
      return {};
    }
    return Values[NextIndex++];
  }

  std::size_t Calls = 0;

private:
  std::vector<CoreActionIdentifiers> Values;
  std::size_t NextIndex = 0;
};

class CollectingSink final : public ChannelEventSink {
public:
  void OnChannelMessage(const InboundChannelMessage& Event) override {
    Events.push_back(Event);
  }

  std::vector<InboundChannelMessage> Events;
};

AccountProfile MakeProfile() {
  return AccountProfile{
      .Channel = ChannelId{"Mock"},
      .Account = AccountId{"account-synthetic-001"},
      .Mode = IntegrationMode{"Mock"},
  };
}

CapabilitySnapshot MakeCapabilities(const AccountProfile& Profile) {
  return CapabilitySnapshot{
      .Channel = Profile.Channel,
      .Account = Profile.Account,
      .Mode = Profile.Mode,
      .SnapshotVersion = 7,
      .CapturedAt = Timestamp{},
      .ValidUntil = std::nullopt,
      .AuthenticationStatus = VerificationStatus::Verified,
      .AuditStatus = VerificationStatus::Verified,
      .Capabilities = {
          CapabilityObservation{
              .Capability = ChannelCapability::ReceiveText,
              .Status = CapabilityStatus::Supported,
              .EvidenceStatus = VerificationStatus::Verified,
              .ObservedAt = Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = EvidenceReference{"mock-evidence-receive"},
          },
          CapabilityObservation{
              .Capability = ChannelCapability::SendText,
              .Status = CapabilityStatus::Supported,
              .EvidenceStatus = VerificationStatus::Verified,
              .ObservedAt = Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = EvidenceReference{"mock-evidence-send"},
          },
          CapabilityObservation{
              .Capability = ChannelCapability::SendSticker,
              .Status = CapabilityStatus::Unsupported,
              .EvidenceStatus = VerificationStatus::Verified,
              .ObservedAt = Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = EvidenceReference{"mock-evidence-sticker"},
          },
          CapabilityObservation{
              .Capability = ChannelCapability::SendProactiveMessage,
              .Status = CapabilityStatus::Unknown,
              .EvidenceStatus = VerificationStatus::Unknown,
              .ObservedAt = Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = std::nullopt,
          },
      },
  };
}

InboundChannelMessage MakeInboundMessage(const std::string& StableId,
                                         const std::string& Sender,
                                         const std::string& Body) {
  return InboundChannelMessage{
      .Envelope = ChannelEnvelope{
          .Channel = ChannelId{"Mock"},
          .Mode = IntegrationMode{"Mock"},
          .Account = AccountId{"account-synthetic-001"},
          .Conversation = ConversationId{"conversation-synthetic-001"},
          .ConversationType = ConversationKind::Private,
          .Sender = SenderId{Sender},
          .Message = MessageId{"message-" + StableId},
          .PlatformEvent = PlatformEventId{"event-" + StableId},
          .OccurredAt = Timestamp{},
          .StableDeduplication = StableDeduplicationId{StableId},
      },
      .Message = CanonicalMessage{
          .MessageParts = {TextPart{Body}},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      },
  };
}

std::vector<InboundChannelMessage> MakeInboundEvents() {
  return {
      MakeInboundMessage(
          "dedup-synthetic-001", "sender-synthetic-001",
          "Synthetic body says sender=forged; that text is untrusted."),
      MakeInboundMessage(
          "dedup-synthetic-001", "sender-forged-002",
          "Duplicate platform event with different body must not redeliver."),
  };
}

CoreActionIdentifiers MakeIdentifiers(const std::string& Suffix) {
  return CoreActionIdentifiers{
      .ActionId = ActionId{"action-synthetic-" + Suffix},
      .OperationId = OperationId{"operation-synthetic-" + Suffix},
      .IdempotencyKey = IdempotencyKey{"idempotency-synthetic-" + Suffix},
  };
}

ChannelReceipt MakeReceipt(const CoreActionIdentifiers& Identifiers,
                           ReceiptStatus Status,
                           ChannelErrorCode ErrorCode,
                           bool IncludePlatformReceipt = false,
                           bool IncludeRetryAfter = false,
                           std::optional<ActionId> ReceiptAction = std::nullopt,
                           std::optional<OperationId> ReceiptOperation =
                               std::nullopt) {
  return ChannelReceipt{
      .Action = ReceiptAction.value_or(Identifiers.ActionId),
      .Operation = ReceiptOperation.value_or(Identifiers.OperationId),
      .PlatformReceipt =
          IncludePlatformReceipt
              ? std::optional<PlatformReceiptId>{PlatformReceiptId{
                    "receipt-" + Identifiers.ActionId.Value}}
              : std::nullopt,
      .OccurredAt = Timestamp{},
      .Status = Status,
      .ErrorCode = ErrorCode,
      .RetryAfter = IncludeRetryAfter
                        ? std::optional<Timestamp>{Timestamp{}}
                        : std::nullopt,
  };
}

MockActionReceipt MakeScript(const CoreActionIdentifiers& Identifiers,
                             ReceiptStatus Status,
                             ChannelErrorCode ErrorCode,
                             bool IncludePlatformReceipt = false,
                             bool IncludeRetryAfter = false,
                             std::optional<ActionId> ReceiptAction =
                                 std::nullopt,
                             std::optional<OperationId> ReceiptOperation =
                                 std::nullopt) {
  return MockActionReceipt{
      .Action = Identifiers.ActionId,
      .Operation = Identifiers.OperationId,
      .Receipt = MakeReceipt(Identifiers, Status, ErrorCode,
                            IncludePlatformReceipt, IncludeRetryAfter,
                            std::move(ReceiptAction),
                            std::move(ReceiptOperation)),
  };
}

MockChannelConfiguration MakeConfiguration(
    const AccountProfile& Profile,
    std::vector<InboundChannelMessage> InboundEvents,
    std::vector<MockActionReceipt> SendActionResults = {}) {
  return MockChannelConfiguration{
      .Profile = Profile,
      .Capabilities = MakeCapabilities(Profile),
      .InboundEvents = std::move(InboundEvents),
      .SendActionResults = std::move(SendActionResults),
  };
}

EmulativeDialog::Providers::ModelProviderProfile MakeProviderProfile() {
  EmulativeDialog::Providers::ModelProviderProfile Profile;
  Profile.Id = EmulativeDialog::Providers::ProviderProfileId{
      "provider-synthetic-001"};
  Profile.ModelName = "synthetic-model";
  Profile.Credential = EmulativeDialog::Providers::CredentialRef{
      "credential-synthetic-001"};
  return Profile;
}

class Fixture final {
public:
  explicit Fixture(
      std::vector<MockActionReceipt> SendActionResults = {},
      std::vector<CoreActionIdentifiers> Identifiers = {},
      std::vector<InboundChannelMessage> InboundEvents = MakeInboundEvents())
      : Profile(MakeProfile()),
        ProviderProfile(MakeProviderProfile()),
        Channel(MakeConfiguration(Profile, std::move(InboundEvents),
                                  std::move(SendActionResults))),
        Ids(std::move(Identifiers)),
        Policy(7, {IdentityBinding{
                       .Account = Profile,
                       .Sender = SenderId{"sender-synthetic-001"},
                       .Principal = PrincipalId{"principal-synthetic-001"},
                   }}),
        Service(Policy, Runtime, Provider, ProviderProfile, ProviderLease,
                Channel, ChannelLease, Ids) {
    const auto Started = Channel.Start(Profile, ChannelLease);
    Require(Started.Status == StartStatus::Started,
            "synthetic Mock must start before publishing test messages");
    const auto Subscribed = Channel.SubscribeEvents(Sink);
    Require(Subscribed.Status == SubscriptionStatus::Subscribed,
            "synthetic Mock must publish its configured inbound messages");
  }

  CoreChatResult Process(std::size_t EventIndex = 0) {
    return Service.Process(Sink.Events.at(EventIndex));
  }

  AccountProfile Profile;
  FakeAgentRuntime Runtime;
  FakeProvider Provider;
  EmulativeDialog::Providers::ModelProviderProfile ProviderProfile;
  TestProviderCredentialLease ProviderLease;
  MockChannelAdapter Channel;
  TestChannelCredentialLease ChannelLease;
  ScriptedActionIdSource Ids;
  IdentityPolicy Policy;
  CoreChatService Service;
  CollectingSink Sink;
};

void RequireCorrelatedInvalidRequest(const Fixture& TestFixture,
                                    const CoreActionIdentifiers& Identifiers,
                                    const char* Message) {
  const auto& MaybeReceipt = TestFixture.Channel.GetLastSendActionReceipt();
  Require(MaybeReceipt.has_value(), Message);
  const auto& Receipt = *MaybeReceipt;
  Require(Receipt.Status == ReceiptStatus::Failed &&
              Receipt.ErrorCode == ChannelErrorCode::InvalidRequest,
          Message);
  Require(Receipt.Action.Value == Identifiers.ActionId.Value &&
              Receipt.Operation.Value == Identifiers.OperationId.Value &&
              !Receipt.PlatformReceipt.has_value(),
          Message);
}

void TestMockImplementsPortAndUsesStrongIdentityTypes() {
  static_assert(std::is_base_of_v<ChannelAdapterPort, MockChannelAdapter>);
  static_assert(!std::is_convertible_v<ChannelId, AccountId>);
  static_assert(!std::is_convertible_v<ConversationId, SenderId>);
  static_assert(!std::is_convertible_v<CanonicalMessage, ChannelEnvelope>);
  static_assert(!std::is_constructible_v<
                EmulativeDialog::Core::CoreAuthorizedAction, ActionId,
                OperationId, IdempotencyKey, EmulativeDialog::Core::ConversationScope,
                std::string,
                EmulativeDialog::Core::AuthorizationRef, std::uint64_t,
                std::uint64_t>);

  const auto Profile = MakeProfile();
  MockChannelAdapter Adapter{MakeConfiguration(Profile, MakeInboundEvents())};
  const auto Snapshot = Adapter.GetCapabilities(Profile);
  Require(Snapshot.Capabilities.size() == 4,
          "mock should return the configured capability snapshot");
  Require(Snapshot.Capabilities[0].Status == CapabilityStatus::Supported &&
              Snapshot.Capabilities[1].Status == CapabilityStatus::Supported,
          "verified ReceiveText and SendText capabilities must be preserved");
  Require(Snapshot.Capabilities[2].Status == CapabilityStatus::Unsupported,
          "unsupported capability state must be preserved");
  Require(Snapshot.Capabilities[3].Status == CapabilityStatus::Unknown,
          "unknown capability state must remain distinct");

  auto WrongProfile = Profile;
  WrongProfile.Account = AccountId{"account-other-synthetic"};
  const auto Unknown = Adapter.GetCapabilities(WrongProfile);
  Require(Unknown.Capabilities.empty(),
          "unmatched account must not inherit another account's capabilities");
  Require(Unknown.AuthenticationStatus == VerificationStatus::Unknown,
          "unmatched account authentication must be unknown");
}

void TestLifecycleAndOfflineInboundDeduplication() {
  const auto Profile = MakeProfile();
  MockChannelAdapter Adapter{MakeConfiguration(Profile, MakeInboundEvents())};
  const TestChannelCredentialLease Lease;
  CollectingSink FirstSink;

  Require(Adapter.SubscribeEvents(FirstSink).Status == SubscriptionStatus::Failed,
          "subscription before Start must fail without event delivery");
  Require(Adapter.Start(Profile, Lease).Status == StartStatus::Started,
          "first Start must be explicit");
  Require(Adapter.Start(Profile, Lease).Status == StartStatus::AlreadyStarted,
          "repeated Start must be idempotent");
  Require(Adapter.SubscribeEvents(FirstSink).Status ==
              SubscriptionStatus::Subscribed,
          "active Mock should subscribe its test sink");
  Require(FirstSink.Events.size() == 1,
          "duplicate StableDeduplicationId must not be delivered twice");
  Require(FirstSink.Events[0].Envelope.Sender.Value == "sender-synthetic-001",
          "sender identity must come from the configured Envelope");
  Require(std::get<TextPart>(FirstSink.Events[0].Message.MessageParts[0])
                  .Utf8Text.find("sender=forged") != std::string::npos,
          "spoof text remains ordinary untrusted message content");
  Require(Adapter.GetDeliveredEventCount() == 1,
          "delivered event counter must count only unique events");
  Require(Adapter.SubscribeEvents(FirstSink).Status ==
              SubscriptionStatus::AlreadySubscribed,
          "repeated subscription to the same sink must not replay events");

  CollectingSink OtherSink;
  Require(Adapter.SubscribeEvents(OtherSink).Status == SubscriptionStatus::Failed,
          "an active subscription cannot be silently replaced");
  Require(Adapter.Stop().Status == StopStatus::Stopped,
          "first Stop must stop the in-memory adapter");
  Require(!Adapter.IsStarted(), "Stop must clear the in-memory started state");
  Require(Adapter.Stop().Status == StopStatus::AlreadyStopped,
          "repeated Stop must be idempotent");
  Require(Adapter.Start(Profile, Lease).Status == StartStatus::Started,
          "Mock can be restarted for a deterministic test");
  Require(Adapter.IsStarted(), "Start must set the in-memory started state");

  CollectingSink ReconnectedSink;
  Require(Adapter.SubscribeEvents(ReconnectedSink).Status ==
              SubscriptionStatus::Subscribed,
          "restarted Mock should accept a new sink");
  Require(ReconnectedSink.Events.empty(),
          "stable deduplication state must survive Mock Stop/Start");
  Require(Adapter.GetHealthSnapshot().Connection == ConnectionStatus::Connected,
          "HealthSnapshot must report the in-memory mock state");
}

void TestCoreServiceProducesOneExactCorrelatedReceipt() {
  const auto Identifiers = MakeIdentifiers("success");
  Fixture TestFixture({MakeScript(Identifiers, ReceiptStatus::Succeeded,
                                  ChannelErrorCode::None, true)},
                      {Identifiers});
  const auto Result = TestFixture.Process();
  Require(Result.Status == CoreChatStatus::Completed,
          "normal CoreChatService authorization should complete on exact success");
  Require(TestFixture.Runtime.Calls == 1 && TestFixture.Ids.Calls == 1,
          "one trusted inbound message should run once and obtain one ID tuple");
  Require(TestFixture.Channel.GetSendActionCallCount() == 1,
          "CoreChatService should call SendAction exactly once");

  const auto& MaybeReceipt = TestFixture.Channel.GetLastSendActionReceipt();
  Require(MaybeReceipt.has_value(),
          "SendAction must return exactly one observable ChannelReceipt");
  const auto& Receipt = *MaybeReceipt;
  Require(Receipt.Status == ReceiptStatus::Succeeded &&
              Receipt.ErrorCode == ChannelErrorCode::None &&
              Receipt.PlatformReceipt.has_value(),
          "matching one-shot script should preserve its configured success");
  Require(Receipt.Action.Value == Identifiers.ActionId.Value &&
              Receipt.Operation.Value == Identifiers.OperationId.Value,
          "scripted success receipt must correlate to the Core-issued action");

  const auto Duplicate = TestFixture.Process();
  Require(Duplicate.Status == CoreChatStatus::Duplicate,
          "replayed inbound envelope must remain a Core duplicate");
  Require(TestFixture.Runtime.Calls == 1 && TestFixture.Ids.Calls == 1 &&
              TestFixture.Channel.GetSendActionCallCount() == 1,
          "replayed inbound envelope must not create a second action or send");
}

void TestStoppedMockReturnsOneCorrelatedFailureReceipt() {
  const auto Identifiers = MakeIdentifiers("stopped-adapter");
  Fixture TestFixture({}, {Identifiers});
  Require(TestFixture.Channel.Stop().Status == StopStatus::Stopped,
          "stopped-adapter test must stop the in-memory mock first");

  const auto Result = TestFixture.Process();
  Require(Result.Status != CoreChatStatus::Completed,
          "a stopped Mock must not complete an authorized send");
  Require(TestFixture.Channel.GetSendActionCallCount() == 1,
          "Core must receive one receipt from the stopped Mock SendAction call");
  const auto& MaybeReceipt = TestFixture.Channel.GetLastSendActionReceipt();
  Require(MaybeReceipt.has_value(),
          "stopped SendAction must still return exactly one receipt");
  const auto& Receipt = *MaybeReceipt;
  Require(Receipt.Status == ReceiptStatus::Failed &&
              Receipt.ErrorCode == ChannelErrorCode::InvalidProfile &&
              Receipt.Action.Value == Identifiers.ActionId.Value &&
              Receipt.Operation.Value == Identifiers.OperationId.Value &&
              !Receipt.PlatformReceipt.has_value(),
          "stopped SendAction must fail with its actual action correlation");
}

void TestMissingMismatchedDuplicateAndMalformedScriptsFailClosed() {
  const auto ExpectInvalidRequest = [](
      const CoreActionIdentifiers& Identifiers,
      std::vector<MockActionReceipt> Scripts,
      const char* Message) {
    Fixture TestFixture(std::move(Scripts), {Identifiers});
    const auto Result = TestFixture.Process();
    Require(Result.Status != CoreChatStatus::Completed,
            "invalid receipt scripts must never complete the Core turn");
    Require(TestFixture.Channel.GetSendActionCallCount() == 1,
            "each authorized test action must invoke SendAction exactly once");
    RequireCorrelatedInvalidRequest(TestFixture, Identifiers, Message);
  };

  const auto MissingIds = MakeIdentifiers("missing");
  ExpectInvalidRequest(MissingIds, {},
                       "missing script must return a correlated invalid receipt");

  const auto MismatchedIds = MakeIdentifiers("mismatched");
  auto MismatchedKey = MakeScript(MismatchedIds, ReceiptStatus::Succeeded,
                                  ChannelErrorCode::None, true);
  MismatchedKey.Operation = OperationId{"operation-other-synthetic"};
  ExpectInvalidRequest(
      MismatchedIds, {std::move(MismatchedKey)},
      "mismatched action/operation key must fail with correlated InvalidRequest");

  const auto MismatchedReceiptOperationIds =
      MakeIdentifiers("mismatched-receipt-operation");
  ExpectInvalidRequest(
      MismatchedReceiptOperationIds,
      {MakeScript(MismatchedReceiptOperationIds, ReceiptStatus::Succeeded,
                  ChannelErrorCode::None, true, false, std::nullopt,
                  OperationId{"foreign-operation-synthetic"})},
      "receipt OperationId mismatch must fail with current action correlation");

  const auto DuplicateIds = MakeIdentifiers("duplicate-script");
  ExpectInvalidRequest(
      DuplicateIds,
      {MakeScript(DuplicateIds, ReceiptStatus::Succeeded,
                  ChannelErrorCode::None, true),
       MakeScript(DuplicateIds, ReceiptStatus::Rejected,
                  ChannelErrorCode::PlatformRejected)},
      "duplicate exact script keys must fail without choosing either outcome");

  const auto MalformedIds = MakeIdentifiers("malformed-payload");
  ExpectInvalidRequest(
      MalformedIds,
      {MakeScript(MalformedIds, ReceiptStatus::Succeeded,
                  ChannelErrorCode::None, true,
                  false, ActionId{"foreign-action-synthetic"})},
      "mis-correlated receipt payload must be replaced by a correlated failure");
}

void TestOneShotScriptCannotBeReplayedOrUsedAfterExhaustion() {
  const auto FirstIds = MakeIdentifiers("one-shot-first");
  const auto SecondIds = MakeIdentifiers("one-shot-second");
  const std::vector<InboundChannelMessage> Events{
      MakeInboundMessage("dedup-one-shot-first", "sender-synthetic-001",
                         "first synthetic message"),
      MakeInboundMessage("dedup-one-shot-second", "sender-synthetic-001",
                         "second synthetic message"),
  };
  Fixture TestFixture(
      {MakeScript(FirstIds, ReceiptStatus::Succeeded,
                  ChannelErrorCode::None, true)},
      {FirstIds, SecondIds}, Events);

  const auto FirstResult = TestFixture.Process(0);
  Require(FirstResult.Status == CoreChatStatus::Completed,
          "the exact first script should be consumed successfully once");
  Require(TestFixture.Channel.GetSendActionCallCount() == 1,
          "the exact one-shot script must dispatch once");

  ScriptedActionIdSource ReplayIds{{FirstIds}};
  CoreChatService ReplayService(
      TestFixture.Policy, TestFixture.Runtime, TestFixture.Provider,
      TestFixture.ProviderProfile, TestFixture.ProviderLease,
      TestFixture.Channel, TestFixture.ChannelLease, ReplayIds);
  const auto ReplayedAction =
      ReplayService.Process(TestFixture.Sink.Events.at(0));
  Require(ReplayedAction.Status != CoreChatStatus::Completed &&
              ReplayIds.Calls == 1 &&
              TestFixture.Channel.GetSendActionCallCount() == 2,
          "a fresh Core service replaying the consumed tuple must fail closed");
  RequireCorrelatedInvalidRequest(
      TestFixture, FirstIds,
      "replayed action IDs must receive a correlated InvalidRequest receipt");

  const auto SecondResult = TestFixture.Process(1);
  Require(SecondResult.Status != CoreChatStatus::Completed,
          "an exhausted script must not authorize a later action");
  Require(TestFixture.Channel.GetSendActionCallCount() == 3 &&
              TestFixture.Ids.Calls == 2,
          "each distinct approved message should dispatch once without retry");
  RequireCorrelatedInvalidRequest(
      TestFixture, SecondIds,
      "an exhausted one-shot script must fail with the second action IDs");

  const auto ReplayedInbound = TestFixture.Process(0);
  Require(ReplayedInbound.Status == CoreChatStatus::Duplicate &&
              TestFixture.Channel.GetSendActionCallCount() == 3,
          "replayed inbound data must not replay a consumed receipt or send");
}

void TestAllConfiguredV1ReceiptStatusesRemainDistinctAndCorrelated() {
  struct StatusCase final {
    ReceiptStatus Status;
    ChannelErrorCode ErrorCode;
    bool HasPlatformReceipt;
    bool HasRetryAfter;
  };
  const std::array Cases{
      StatusCase{ReceiptStatus::Succeeded, ChannelErrorCode::None, true, false},
      StatusCase{ReceiptStatus::Rejected, ChannelErrorCode::PlatformRejected,
                 false, false},
      StatusCase{ReceiptStatus::UnsupportedCapability,
                 ChannelErrorCode::UnsupportedCapability, false, false},
      StatusCase{ReceiptStatus::Forbidden, ChannelErrorCode::Forbidden, false,
                 false},
      StatusCase{ReceiptStatus::RateLimited, ChannelErrorCode::RateLimited,
                 false, true},
      StatusCase{ReceiptStatus::Failed, ChannelErrorCode::PlatformRejected,
                 false, false},
      StatusCase{ReceiptStatus::UnknownOutcome, ChannelErrorCode::OutcomeUnknown,
                 false, false},
  };

  std::size_t Index = 0;
  for (const auto& Case : Cases) {
    const auto Identifiers = MakeIdentifiers("status-" + std::to_string(Index++));
    Fixture TestFixture(
        {MakeScript(Identifiers, Case.Status, Case.ErrorCode,
                    Case.HasPlatformReceipt, Case.HasRetryAfter)},
        {Identifiers});
    static_cast<void>(TestFixture.Process());
    const auto& MaybeReceipt = TestFixture.Channel.GetLastSendActionReceipt();
    Require(MaybeReceipt.has_value(),
            "every Core-dispatched action must return one receipt");
    const auto& Receipt = *MaybeReceipt;
    Require(Receipt.Status == Case.Status && Receipt.ErrorCode == Case.ErrorCode,
            "Mock must preserve the configured V1 receipt status and error");
    Require(Receipt.Action.Value == Identifiers.ActionId.Value &&
                Receipt.Operation.Value == Identifiers.OperationId.Value,
            "every configured V1 receipt must retain exact action correlation");
    Require(Receipt.PlatformReceipt.has_value() == Case.HasPlatformReceipt &&
                Receipt.RetryAfter.has_value() == Case.HasRetryAfter,
            "optional receipt metadata must remain distinct for each status");
    Require(TestFixture.Channel.GetSendActionCallCount() == 1,
            "receipt statuses, including UnknownOutcome, must never trigger retry");
  }
}

void TestInvalidSyntheticEnvelopeFailsBeforePublishing() {
  auto Events = MakeInboundEvents();
  Events[0].Envelope.StableDeduplication.Value.clear();
  const auto Profile = MakeProfile();
  MockChannelAdapter Adapter{MakeConfiguration(Profile, std::move(Events))};
  CollectingSink Sink;
  const TestChannelCredentialLease Lease;
  Require(Adapter.Start(Profile, Lease).Status == StartStatus::Started,
          "valid Mock profile should start");
  Require(Adapter.SubscribeEvents(Sink).Status == SubscriptionStatus::Failed,
          "invalid synthetic Envelope must be rejected");
  Require(Adapter.GetDeliveredEventCount() == 0 && Sink.Events.empty(),
          "validation must finish before any inbound event is published");
}

void TestMockProfileCannotImpersonateRealModes() {
  auto Profile = MakeProfile();
  auto Configuration = MakeConfiguration(Profile, MakeInboundEvents());
  Configuration.Profile.Mode = IntegrationMode{"QQBot"};
  MockChannelAdapter Adapter{std::move(Configuration)};
  const TestChannelCredentialLease Lease;
  Require(Adapter.Start(Profile, Lease).Status == StartStatus::Failed,
          "MockChannelAdapter must reject a non-Mock configured profile");
  Require(Adapter.GetCapabilities(Profile).Capabilities.empty(),
          "Mock must not claim capabilities for a real platform mode");
}

} // namespace

int main() {
  TestMockImplementsPortAndUsesStrongIdentityTypes();
  TestLifecycleAndOfflineInboundDeduplication();
  TestCoreServiceProducesOneExactCorrelatedReceipt();
  TestStoppedMockReturnsOneCorrelatedFailureReceipt();
  TestMissingMismatchedDuplicateAndMalformedScriptsFailClosed();
  TestOneShotScriptCannotBeReplayedOrUsedAfterExhaustion();
  TestAllConfiguredV1ReceiptStatusesRemainDistinctAndCorrelated();
  TestInvalidSyntheticEnvelopeFailsBeforePublishing();
  TestMockProfileCannotImpersonateRealModes();
  return 0;
}
