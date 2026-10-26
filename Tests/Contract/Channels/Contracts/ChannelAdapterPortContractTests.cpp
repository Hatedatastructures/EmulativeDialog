#include "Source/Channels/Contracts/ChannelAdapterPort.h"

#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>

namespace EmulativeDialog::Core {

// Test-only stand-ins keep this port contract test independent of Core runtime.
class CredentialLease final {};
class CoreAuthorizedAction final {};

} // namespace EmulativeDialog::Core

namespace {

using namespace EmulativeDialog::Channels;

void Require(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

template <typename Type>
concept HasChannelIdentity = requires(const Type& Value) { Value.Channel; };

template <typename Type>
concept HasAccountIdentity = requires(const Type& Value) { Value.Account; };

template <typename Type>
concept HasConversationIdentity = requires(const Type& Value) { Value.Conversation; };

template <typename Type>
concept HasSenderIdentity = requires(const Type& Value) { Value.Sender; };

using SendActionSignature = ChannelReceipt (ChannelAdapterPort::*)(
    const EmulativeDialog::Core::CoreAuthorizedAction&,
    const EmulativeDialog::Core::CredentialLease&);
using StartSignature = StartResult (ChannelAdapterPort::*)(
    const AccountProfile&, const EmulativeDialog::Core::CredentialLease&);

static_assert(!std::is_convertible_v<ChannelId, AccountId>);
static_assert(!std::is_convertible_v<ConversationId, SenderId>);
static_assert(HasChannelIdentity<ChannelEnvelope>);
static_assert(HasAccountIdentity<ChannelEnvelope>);
static_assert(HasConversationIdentity<ChannelEnvelope>);
static_assert(HasSenderIdentity<ChannelEnvelope>);
static_assert(!HasChannelIdentity<CanonicalMessage>);
static_assert(!HasAccountIdentity<CanonicalMessage>);
static_assert(!HasConversationIdentity<CanonicalMessage>);
static_assert(!HasSenderIdentity<CanonicalMessage>);
static_assert(!std::is_convertible_v<CanonicalMessage, ChannelEnvelope>);
static_assert(!std::is_convertible_v<CanonicalMessage,
                                    EmulativeDialog::Core::CoreAuthorizedAction>);
static_assert(std::is_same_v<decltype(&ChannelAdapterPort::SendAction),
                             SendActionSignature>);
static_assert(std::is_same_v<decltype(&ChannelAdapterPort::Start),
                             StartSignature>);
static_assert(std::is_abstract_v<ChannelAdapterPort>);

class TestSink final : public ChannelEventSink {
public:
  void OnChannelMessage(const InboundChannelMessage& Event) override {
    LastEvent = Event;
    ++ReceivedCount;
  }

  InboundChannelMessage LastEvent{};
  int ReceivedCount = 0;
};

class TestPort final : public ChannelAdapterPort {
public:
  CapabilitySnapshot GetCapabilities(const AccountProfile& Profile) override {
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
                .Evidence = std::nullopt,
            },
            CapabilityObservation{
                .Capability = ChannelCapability::SendSticker,
                .Status = CapabilityStatus::Unsupported,
                .EvidenceStatus = VerificationStatus::Unknown,
                .ObservedAt = Timestamp{},
                .ValidUntil = std::nullopt,
                .Evidence = std::nullopt,
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

  StartResult Start(const AccountProfile&,
                    const EmulativeDialog::Core::CredentialLease&) override {
    if (Started) {
      return StartResult{StartStatus::AlreadyStarted, ChannelErrorCode::None};
    }
    Started = true;
    return StartResult{StartStatus::Started, ChannelErrorCode::None};
  }

  StopResult Stop() override {
    if (!Started) {
      return StopResult{StopStatus::AlreadyStopped, ChannelErrorCode::None};
    }
    Started = false;
    Sink = nullptr;
    return StopResult{StopStatus::Stopped, ChannelErrorCode::None};
  }

  SubscriptionResult SubscribeEvents(ChannelEventSink& EventSink) override {
    if (Sink == &EventSink) {
      return SubscriptionResult{SubscriptionStatus::AlreadySubscribed,
                                ChannelErrorCode::None};
    }
    Sink = &EventSink;
    return SubscriptionResult{SubscriptionStatus::Subscribed,
                              ChannelErrorCode::None};
  }

  ChannelReceipt SendAction(
      const EmulativeDialog::Core::CoreAuthorizedAction&,
      const EmulativeDialog::Core::CredentialLease&) override {
    ++SendCalls;
    return ChannelReceipt{
        .Action = ActionId{"action-test"},
        .Operation = OperationId{"operation-test"},
        .PlatformReceipt = NextStatus == ReceiptStatus::Succeeded
                               ? std::optional<PlatformReceiptId>{
                                     PlatformReceiptId{"platform-receipt-test"}}
                               : std::nullopt,
        .OccurredAt = Timestamp{},
        .Status = NextStatus,
        .ErrorCode = NextError,
        .RetryAfter = NextStatus == ReceiptStatus::RateLimited
                          ? std::optional<Timestamp>{Timestamp{}}
                          : std::nullopt,
    };
  }

  HealthSnapshot GetHealthSnapshot() const override {
    return HealthSnapshot{
        .Connection = Started ? ConnectionStatus::Connected
                              : ConnectionStatus::Stopped,
        .Authentication = AuthenticationStatus::Valid,
        .RateLimit = RateLimitStatus::Normal,
        .CapturedAt = Timestamp{},
        .RetryAfter = std::nullopt,
        .ErrorCode = ChannelErrorCode::None,
    };
  }

  void EmitForTest(const InboundChannelMessage& Event) {
    if (Sink != nullptr) {
      Sink->OnChannelMessage(Event);
    }
  }

  bool Started = false;
  int SendCalls = 0;
  ChannelEventSink* Sink = nullptr;
  ReceiptStatus NextStatus = ReceiptStatus::Succeeded;
  ChannelErrorCode NextError = ChannelErrorCode::None;
};

void TestCapabilitiesAndLifecycle() {
  TestPort Port;
  const AccountProfile Profile{
      .Channel = ChannelId{"Mock"},
      .Account = AccountId{"account-test"},
      .Mode = IntegrationMode{"Mock"},
  };
  const auto Snapshot = Port.GetCapabilities(Profile);
  Require(Snapshot.Channel.Value == "Mock", "capability snapshot lost ChannelId");
  Require(Snapshot.Account.Value == "account-test",
          "capability snapshot lost AccountId");
  Require(Snapshot.Capabilities.size() == 3,
          "capability snapshot must retain all evidence states");
  Require(Snapshot.Capabilities[0].Status == CapabilityStatus::Supported,
          "supported capability state missing");
  Require(Snapshot.Capabilities[1].Status == CapabilityStatus::Unsupported,
          "unsupported capability state missing");
  Require(Snapshot.Capabilities[2].Status == CapabilityStatus::Unknown,
          "unknown capability state missing");

  const EmulativeDialog::Core::CredentialLease Lease;
  Require(Port.Start(Profile, Lease).Status == StartStatus::Started,
          "Start should report Started on first call");
  Require(Port.Start(Profile, Lease).Status == StartStatus::AlreadyStarted,
          "Start should be idempotent");
  Require(Port.Stop().Status == StopStatus::Stopped,
          "Stop should report Stopped on first call");
  Require(Port.Stop().Status == StopStatus::AlreadyStopped,
          "Stop should be idempotent");
  Require(Port.GetHealthSnapshot().Connection == ConnectionStatus::Stopped,
          "health snapshot should report redacted connection state");
}

void TestInboundIdentitySeparation() {
  TestPort Port;
  TestSink Sink;
  Require(Port.SubscribeEvents(Sink).Status == SubscriptionStatus::Subscribed,
          "event sink should subscribe");

  const InboundChannelMessage Event{
      .Envelope = ChannelEnvelope{
          .Channel = ChannelId{"Mock"},
          .Mode = IntegrationMode{"Mock"},
          .Account = AccountId{"account-test"},
          .Conversation = ConversationId{"conversation-test"},
          .ConversationType = ConversationKind::Private,
          .Sender = SenderId{"sender-test"},
          .Message = MessageId{"message-test"},
          .PlatformEvent = PlatformEventId{"event-test"},
          .OccurredAt = Timestamp{},
          .StableDeduplication = StableDeduplicationId{"dedup-test"},
      },
      .Message = CanonicalMessage{
          .MessageParts = {TextPart{"Synthetic untrusted message body."}},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      },
  };
  Port.EmitForTest(Event);
  Require(Sink.ReceivedCount == 1, "verified inbound envelope was not delivered");
  Require(Sink.LastEvent.Envelope.Sender.Value == "sender-test",
          "trusted sender id must remain in the Envelope");
  Require(Sink.LastEvent.Message.MessageParts.size() == 1,
          "canonical message body was not preserved separately");
}

void TestReceiptOutcomeStates() {
  TestPort Port;
  const EmulativeDialog::Core::CoreAuthorizedAction Action;
  const EmulativeDialog::Core::CredentialLease Lease;

  const auto Success = Port.SendAction(Action, Lease);
  Require(Success.Status == ReceiptStatus::Succeeded,
          "success receipt status missing");
  Require(Success.PlatformReceipt.has_value(),
          "successful receipt should allow a platform receipt id");

  Port.NextStatus = ReceiptStatus::UnsupportedCapability;
  Port.NextError = ChannelErrorCode::UnsupportedCapability;
  const auto Unsupported = Port.SendAction(Action, Lease);
  Require(Unsupported.Status == ReceiptStatus::UnsupportedCapability,
          "unsupported receipt must be explicit");
  Require(!Unsupported.PlatformReceipt.has_value(),
          "unsupported receipt must not look successful");

  Port.NextStatus = ReceiptStatus::Failed;
  Port.NextError = ChannelErrorCode::TransportFailure;
  Require(Port.SendAction(Action, Lease).Status == ReceiptStatus::Failed,
          "failed receipt status missing");

  Port.NextStatus = ReceiptStatus::UnknownOutcome;
  Port.NextError = ChannelErrorCode::OutcomeUnknown;
  Require(Port.SendAction(Action, Lease).Status == ReceiptStatus::UnknownOutcome,
          "unknown outcome must be distinct from failure or success");
}

} // namespace

int main() {
  TestCapabilitiesAndLifecycle();
  TestInboundIdentitySeparation();
  TestReceiptOutcomeStates();
  return 0;
}