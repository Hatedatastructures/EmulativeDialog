#include "Tests/Contract/Channels/Contracts/MockChannelAdapter.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace EmulativeDialog::Core {

// Test-only credential object; the mock never inspects or retains the lease.
class CredentialLease final {};

} // namespace EmulativeDialog::Core

namespace {

using namespace EmulativeDialog::Channels;
using EmulativeDialog::Channels::Testing::MockActionReceipt;
using EmulativeDialog::Channels::Testing::MockChannelAdapter;
using EmulativeDialog::Channels::Testing::MockChannelConfiguration;

void Require(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

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
              .Evidence = EvidenceReference{"mock-evidence-001"},
          },
          CapabilityObservation{
              .Capability = ChannelCapability::SendSticker,
              .Status = CapabilityStatus::Unsupported,
              .EvidenceStatus = VerificationStatus::Verified,
              .ObservedAt = Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = EvidenceReference{"mock-evidence-002"},
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

ChannelReceipt MakeReceipt(const EmulativeDialog::Core::CoreAuthorizedAction& Action,
                           ReceiptStatus Status,
                           ChannelErrorCode ErrorCode,
                           bool IncludeRetryAfter = false) {
  return ChannelReceipt{
      .Action = Action.Action,
      .Operation = Action.Operation,
      .PlatformReceipt = Status == ReceiptStatus::Succeeded
                             ? std::optional<PlatformReceiptId>{PlatformReceiptId{
                                   "receipt-" + Action.Action.Value}}
                             : std::nullopt,
      .OccurredAt = Timestamp{},
      .Status = Status,
      .ErrorCode = ErrorCode,
      .RetryAfter = IncludeRetryAfter
                        ? std::optional<Timestamp>{Timestamp{}}
                        : std::nullopt,
  };
}

MockChannelConfiguration MakeConfiguration() {
  const auto Profile = MakeProfile();
  return MockChannelConfiguration{
      .Profile = Profile,
      .Capabilities = MakeCapabilities(Profile),
      .InboundEvents = {
          MakeInboundMessage("dedup-synthetic-001", "sender-synthetic-001",
                             "Synthetic body says sender=forged; that text is untrusted."),
          MakeInboundMessage("dedup-synthetic-001", "sender-forged-002",
                             "Duplicate platform event with different body must not redeliver."),
      },
      .SendActionResults = {},
  };
}

void TestMockImplementsPortAndUsesStrongIdentityTypes() {
  static_assert(std::is_base_of_v<ChannelAdapterPort, MockChannelAdapter>);
  static_assert(!std::is_convertible_v<ChannelId, AccountId>);
  static_assert(!std::is_convertible_v<ConversationId, SenderId>);
  static_assert(!std::is_convertible_v<CanonicalMessage, ChannelEnvelope>);
  static_assert(!std::is_convertible_v<CanonicalMessage,
                                      EmulativeDialog::Core::CoreAuthorizedAction>);

  MockChannelAdapter Adapter{MakeConfiguration()};
  const auto Snapshot = Adapter.GetCapabilities(MakeProfile());
  Require(Snapshot.Capabilities.size() == 3,
          "mock should return the configured capability snapshot");
  Require(Snapshot.Capabilities[0].Status == CapabilityStatus::Supported,
          "supported capability state must be preserved");
  Require(Snapshot.Capabilities[1].Status == CapabilityStatus::Unsupported,
          "unsupported capability state must be preserved");
  Require(Snapshot.Capabilities[2].Status == CapabilityStatus::Unknown,
          "unknown capability state must remain distinct");

  auto WrongProfile = MakeProfile();
  WrongProfile.Account = AccountId{"account-other-synthetic"};
  const auto Unknown = Adapter.GetCapabilities(WrongProfile);
  Require(Unknown.Capabilities.empty(),
          "unmatched account must not inherit another account's capabilities");
  Require(Unknown.AuthenticationStatus == VerificationStatus::Unknown,
          "unmatched account authentication must be unknown");
}

void TestLifecycleAndOfflineInboundDeduplication() {
  MockChannelAdapter Adapter{MakeConfiguration()};
  const auto Profile = MakeProfile();
  const EmulativeDialog::Core::CredentialLease Lease;
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

void TestReceiptBranchesAndCorrelation() {
  MockChannelConfiguration Configuration = MakeConfiguration();
  EmulativeDialog::Core::CoreAuthorizedAction SuccessAction{
      ActionId{"action-synthetic-success"}, OperationId{"operation-success"}};
  EmulativeDialog::Core::CoreAuthorizedAction MismatchedCorrelationAction{
      ActionId{"action-synthetic-mismatch"},
      OperationId{"operation-mismatch"}};
  EmulativeDialog::Core::CoreAuthorizedAction ForeignReceiptAction{
      ActionId{"action-foreign-receipt"},
      OperationId{"operation-foreign-receipt"}};
  EmulativeDialog::Core::CoreAuthorizedAction RejectedAction{
      ActionId{"action-synthetic-rejected"}, OperationId{"operation-rejected"}};
  EmulativeDialog::Core::CoreAuthorizedAction UnsupportedAction{
      ActionId{"action-synthetic-unsupported"},
      OperationId{"operation-unsupported"}};
  EmulativeDialog::Core::CoreAuthorizedAction ForbiddenAction{
      ActionId{"action-synthetic-forbidden"}, OperationId{"operation-forbidden"}};
  EmulativeDialog::Core::CoreAuthorizedAction RateLimitedAction{
      ActionId{"action-synthetic-rate-limited"},
      OperationId{"operation-rate-limited"}};
  EmulativeDialog::Core::CoreAuthorizedAction FailedAction{
      ActionId{"action-synthetic-failed"}, OperationId{"operation-failed"}};
  EmulativeDialog::Core::CoreAuthorizedAction UnknownAction{
      ActionId{"action-synthetic-unknown"}, OperationId{"operation-unknown"}};

  Configuration.SendActionResults = {
      MockActionReceipt{&SuccessAction,
                        MakeReceipt(SuccessAction, ReceiptStatus::Succeeded,
                                    ChannelErrorCode::None)},
      MockActionReceipt{&MismatchedCorrelationAction,
                        MakeReceipt(ForeignReceiptAction,
                                    ReceiptStatus::Succeeded,
                                    ChannelErrorCode::None)},
      MockActionReceipt{&RejectedAction,
                        MakeReceipt(RejectedAction, ReceiptStatus::Rejected,
                                    ChannelErrorCode::PlatformRejected)},
      MockActionReceipt{&UnsupportedAction,
                        MakeReceipt(UnsupportedAction,
                                    ReceiptStatus::UnsupportedCapability,
                                    ChannelErrorCode::UnsupportedCapability)},
      MockActionReceipt{&ForbiddenAction,
                        MakeReceipt(ForbiddenAction, ReceiptStatus::Forbidden,
                                    ChannelErrorCode::Forbidden)},
      MockActionReceipt{&RateLimitedAction,
                        MakeReceipt(RateLimitedAction,
                                    ReceiptStatus::RateLimited,
                                    ChannelErrorCode::RateLimited, true)},
      MockActionReceipt{&FailedAction,
                        MakeReceipt(FailedAction, ReceiptStatus::Failed,
                                    ChannelErrorCode::PlatformRejected)},
      MockActionReceipt{&UnknownAction,
                        MakeReceipt(UnknownAction, ReceiptStatus::UnknownOutcome,
                                    ChannelErrorCode::OutcomeUnknown)},
  };

  MockChannelAdapter Adapter{std::move(Configuration)};
  const EmulativeDialog::Core::CredentialLease Lease;
  const auto Profile = MakeProfile();
  const auto Unstarted = Adapter.SendAction(SuccessAction, Lease);
  Require(Unstarted.Status == ReceiptStatus::Failed &&
              Unstarted.Action.Value.empty() && Unstarted.Operation.Value.empty(),
          "pre-start fallback must be visibly uncorrelated and non-successful");
  Require(Adapter.Start(Profile, Lease).Status == StartStatus::Started,
          "Mock must start before scripted action results");

  const auto Success = Adapter.SendAction(SuccessAction, Lease);
  Require(Success.Status == ReceiptStatus::Succeeded &&
              Success.PlatformReceipt.has_value(),
          "scripted success must return its supplied receipt");
  Require(Success.Action.Value == SuccessAction.Action.Value &&
              Success.Operation.Value == SuccessAction.Operation.Value,
          "success receipt must correlate to the exact input action and operation");

  const auto Mismatched = Adapter.SendAction(MismatchedCorrelationAction, Lease);
  Require(Mismatched.Status == ReceiptStatus::Failed &&
              Mismatched.ErrorCode == ChannelErrorCode::InvalidRequest &&
              Mismatched.Action.Value.empty() &&
              Mismatched.Operation.Value.empty() &&
              !Mismatched.PlatformReceipt.has_value(),
          "mismatched scripted success must fail with no correlated receipt");

  const auto Rejected = Adapter.SendAction(RejectedAction, Lease);
  Require(Rejected.Status == ReceiptStatus::Rejected &&
              Rejected.Action.Value == RejectedAction.Action.Value &&
              Rejected.Operation.Value == RejectedAction.Operation.Value &&
              !Rejected.PlatformReceipt.has_value(),
          "Rejected must remain distinct and correlate without a success receipt");

  const auto Unsupported = Adapter.SendAction(UnsupportedAction, Lease);
  Require(Unsupported.Status == ReceiptStatus::UnsupportedCapability &&
              Unsupported.Action.Value == UnsupportedAction.Action.Value &&
              Unsupported.Operation.Value == UnsupportedAction.Operation.Value &&
              !Unsupported.PlatformReceipt.has_value(),
          "UnsupportedCapability must not fabricate a success receipt");

  const auto Forbidden = Adapter.SendAction(ForbiddenAction, Lease);
  Require(Forbidden.Status == ReceiptStatus::Forbidden &&
              Forbidden.Action.Value == ForbiddenAction.Action.Value &&
              Forbidden.Operation.Value == ForbiddenAction.Operation.Value &&
              !Forbidden.PlatformReceipt.has_value(),
          "Forbidden must remain a correlated non-success receipt");

  const auto RateLimited = Adapter.SendAction(RateLimitedAction, Lease);
  Require(RateLimited.Status == ReceiptStatus::RateLimited &&
              RateLimited.RetryAfter.has_value() &&
              !RateLimited.PlatformReceipt.has_value() &&
              RateLimited.Action.Value == RateLimitedAction.Action.Value &&
              RateLimited.Operation.Value == RateLimitedAction.Operation.Value,
          "RateLimited must preserve RetryAfter and action correlation");

  const auto Failed = Adapter.SendAction(FailedAction, Lease);
  Require(Failed.Status == ReceiptStatus::Failed &&
              !Failed.PlatformReceipt.has_value() &&
              Failed.Action.Value == FailedAction.Action.Value &&
              Failed.Operation.Value == FailedAction.Operation.Value,
          "Failed must be distinct and correlated");

  const auto Unknown = Adapter.SendAction(UnknownAction, Lease);
  Require(Unknown.Status == ReceiptStatus::UnknownOutcome &&
              Unknown.ErrorCode == ChannelErrorCode::OutcomeUnknown &&
              !Unknown.PlatformReceipt.has_value() &&
              Unknown.Action.Value == UnknownAction.Action.Value &&
              Unknown.Operation.Value == UnknownAction.Operation.Value,
          "UnknownOutcome must remain distinct and correlated");
  Require(Adapter.GetSendActionCallCount() == 9,
          "Mock must not automatically retry UnknownOutcome");
}

void TestUnscriptedFallbackIsUncorrelatedAndNonSuccess() {
  auto Configuration = MakeConfiguration();
  EmulativeDialog::Core::CoreAuthorizedAction Action{
      ActionId{"action-not-scripted"}, OperationId{"operation-not-scripted"}};
  MockChannelAdapter Adapter{std::move(Configuration)};
  const EmulativeDialog::Core::CredentialLease Lease;
  Require(Adapter.Start(MakeProfile(), Lease).Status == StartStatus::Started,
          "Mock profile should start for unscripted fallback test");
  const auto Receipt = Adapter.SendAction(Action, Lease);
  Require(Receipt.Status == ReceiptStatus::Failed &&
              Receipt.ErrorCode == ChannelErrorCode::InvalidRequest,
          "unscripted action must fail closed");
  Require(Receipt.Action.Value.empty() && Receipt.Operation.Value.empty() &&
              !Receipt.PlatformReceipt.has_value(),
          "unscripted fallback must not fabricate correlated or platform receipt IDs");
  Require(Adapter.GetSendActionCallCount() == 1,
          "unscripted failure must not retry");
}

void TestInvalidSyntheticEnvelopeFailsBeforePublishing() {
  auto Configuration = MakeConfiguration();
  Configuration.InboundEvents[0].Envelope.StableDeduplication.Value.clear();
  MockChannelAdapter Adapter{std::move(Configuration)};
  CollectingSink Sink;
  const EmulativeDialog::Core::CredentialLease Lease;
  Require(Adapter.Start(MakeProfile(), Lease).Status == StartStatus::Started,
          "valid Mock profile should start");
  Require(Adapter.SubscribeEvents(Sink).Status == SubscriptionStatus::Failed,
          "invalid synthetic Envelope must be rejected");
  Require(Adapter.GetDeliveredEventCount() == 0 && Sink.Events.empty(),
          "validation must finish before any inbound event is published");
}

void TestMockProfileCannotImpersonateRealModes() {
  auto Configuration = MakeConfiguration();
  Configuration.Profile.Mode = IntegrationMode{"QQBot"};
  MockChannelAdapter Adapter{std::move(Configuration)};
  const EmulativeDialog::Core::CredentialLease Lease;
  Require(Adapter.Start(MakeProfile(), Lease).Status == StartStatus::Failed,
          "MockChannelAdapter must reject a non-Mock profile");
  Require(Adapter.GetCapabilities(MakeProfile()).Capabilities.empty(),
          "Mock must not claim capabilities for a real platform mode");
}

} // namespace

int main() {
  TestMockImplementsPortAndUsesStrongIdentityTypes();
  TestLifecycleAndOfflineInboundDeduplication();
  TestReceiptBranchesAndCorrelation();
  TestUnscriptedFallbackIsUncorrelatedAndNonSuccess();
  TestInvalidSyntheticEnvelopeFailsBeforePublishing();
  TestMockProfileCannotImpersonateRealModes();
  return 0;
}
