#include "Tests/Contract/Channels/Contracts/MockChannelAdapter.h"

#include <algorithm>
#include <utility>

namespace EmulativeDialog::Channels::Testing {

MockChannelAdapter::MockChannelAdapter(MockChannelConfiguration Configuration)
    : Configuration(std::move(Configuration)) {}

CapabilitySnapshot MockChannelAdapter::GetCapabilities(
    const AccountProfile& Profile) {
  if (!HasValidMockConfiguration() || !MatchesProfile(Profile)) {
    return MakeUnknownCapabilitySnapshot(Profile);
  }
  return Configuration.Capabilities;
}

StartResult MockChannelAdapter::Start(
    const AccountProfile& Profile,
    const Core::CredentialLease& Lease) {
  static_cast<void>(Lease);
  if (!HasValidMockConfiguration() || !MatchesProfile(Profile)) {
    return StartResult{StartStatus::Failed, ChannelErrorCode::InvalidProfile};
  }
  if (Started) {
    return StartResult{StartStatus::AlreadyStarted, ChannelErrorCode::None};
  }
  Started = true;
  return StartResult{StartStatus::Started, ChannelErrorCode::None};
}

StopResult MockChannelAdapter::Stop() {
  if (!Started) {
    return StopResult{StopStatus::AlreadyStopped, ChannelErrorCode::None};
  }
  Started = false;
  ActiveSink = nullptr;
  return StopResult{StopStatus::Stopped, ChannelErrorCode::None};
}

SubscriptionResult MockChannelAdapter::SubscribeEvents(ChannelEventSink& Sink) {
  if (!Started || !HasValidMockConfiguration()) {
    return SubscriptionResult{SubscriptionStatus::Failed,
                              ChannelErrorCode::InvalidProfile};
  }
  if (ActiveSink == &Sink) {
    return SubscriptionResult{SubscriptionStatus::AlreadySubscribed,
                              ChannelErrorCode::None};
  }
  if (ActiveSink != nullptr) {
    return SubscriptionResult{SubscriptionStatus::Failed,
                              ChannelErrorCode::InvalidRequest};
  }
  for (const auto& Event : Configuration.InboundEvents) {
    if (!IsValidInboundEvent(Event)) {
      return SubscriptionResult{SubscriptionStatus::Failed,
                                ChannelErrorCode::InvalidRequest};
    }
  }

  ActiveSink = &Sink;
  for (const auto& Event : Configuration.InboundEvents) {
    const auto [Position, Inserted] = DeliveredDeduplicationIds.insert(
        Event.Envelope.StableDeduplication.Value);
    static_cast<void>(Position);
    if (!Inserted) {
      continue;
    }
    Sink.OnChannelMessage(Event);
    ++DeliveredEventCount;
  }
  return SubscriptionResult{SubscriptionStatus::Subscribed,
                            ChannelErrorCode::None};
}

ChannelReceipt MockChannelAdapter::SendAction(
    const Core::CoreAuthorizedAction& Action,
    const Core::CredentialLease& Lease) {
  static_cast<void>(Lease);
  ++SendActionCallCount;
  if (!Started || !HasValidMockConfiguration()) {
    return StoreSendActionReceipt(
        MakeUnscriptedFailureReceipt(Action, ChannelErrorCode::InvalidProfile));
  }

  const auto MatchesAction = [&Action](const MockActionReceipt& Candidate) {
    return Candidate.Action.Value == Action.GetActionId().Value &&
           Candidate.Operation.Value == Action.GetOperationId().Value;
  };
  const std::size_t MatchCount = static_cast<std::size_t>(std::count_if(
      Configuration.SendActionResults.begin(),
      Configuration.SendActionResults.end(), MatchesAction));
  if (MatchCount != 1) {
    // Duplicate keys are invalid as a group. Consume them so a later replay
    // cannot accidentally select one of the conflicting scripted outcomes.
    if (MatchCount > 1) {
      const auto NewEnd = std::remove_if(
          Configuration.SendActionResults.begin(),
          Configuration.SendActionResults.end(), MatchesAction);
      Configuration.SendActionResults.erase(NewEnd,
                                             Configuration.SendActionResults.end());
    }
    return StoreSendActionReceipt(
        MakeUnscriptedFailureReceipt(Action, ChannelErrorCode::InvalidRequest));
  }

  const auto Result = std::find_if(Configuration.SendActionResults.begin(),
                                   Configuration.SendActionResults.end(),
                                   MatchesAction);
  const ChannelReceipt Receipt = Result->Receipt;
  Configuration.SendActionResults.erase(Result);
  if (Receipt.Action.Value != Action.GetActionId().Value ||
      Receipt.Operation.Value != Action.GetOperationId().Value) {
    return StoreSendActionReceipt(
        MakeUnscriptedFailureReceipt(Action, ChannelErrorCode::InvalidRequest));
  }
  return StoreSendActionReceipt(Receipt);
}

HealthSnapshot MockChannelAdapter::GetHealthSnapshot() const {
  return HealthSnapshot{
      .Connection = Started ? ConnectionStatus::Connected
                            : ConnectionStatus::Stopped,
      .Authentication = AuthenticationStatus::Unknown,
      .RateLimit = RateLimitStatus::Unknown,
      .CapturedAt = Timestamp{},
      .RetryAfter = std::nullopt,
      .ErrorCode = ChannelErrorCode::None,
  };
}

std::size_t MockChannelAdapter::GetDeliveredEventCount() const noexcept {
  return DeliveredEventCount;
}

std::size_t MockChannelAdapter::GetSendActionCallCount() const noexcept {
  return SendActionCallCount;
}

const std::optional<ChannelReceipt>&
MockChannelAdapter::GetLastSendActionReceipt() const noexcept {
  return LastSendActionReceipt;
}

bool MockChannelAdapter::IsStarted() const noexcept {
  return Started;
}

bool MockChannelAdapter::HasValidMockConfiguration() const noexcept {
  return Configuration.Profile.Channel.Value == "Mock" &&
         Configuration.Profile.Mode.Value == "Mock" &&
         !Configuration.Profile.Account.Value.empty() &&
         Configuration.Capabilities.Channel.Value ==
             Configuration.Profile.Channel.Value &&
         Configuration.Capabilities.Account.Value ==
             Configuration.Profile.Account.Value &&
         Configuration.Capabilities.Mode.Value ==
             Configuration.Profile.Mode.Value;
}

bool MockChannelAdapter::MatchesProfile(
    const AccountProfile& Profile) const noexcept {
  return Profile.Channel.Value == Configuration.Profile.Channel.Value &&
         Profile.Account.Value == Configuration.Profile.Account.Value &&
         Profile.Mode.Value == Configuration.Profile.Mode.Value;
}

bool MockChannelAdapter::IsValidInboundEvent(
    const InboundChannelMessage& Event) const noexcept {
  const auto& Envelope = Event.Envelope;
  return Envelope.Channel.Value == Configuration.Profile.Channel.Value &&
         Envelope.Account.Value == Configuration.Profile.Account.Value &&
         Envelope.Mode.Value == Configuration.Profile.Mode.Value &&
         Envelope.Mode.Value == "Mock" &&
         !Envelope.Conversation.Value.empty() &&
         !Envelope.Sender.Value.empty() &&
         !Envelope.Message.Value.empty() &&
         !Envelope.PlatformEvent.Value.empty() &&
         !Envelope.StableDeduplication.Value.empty();
}

CapabilitySnapshot MockChannelAdapter::MakeUnknownCapabilitySnapshot(
    const AccountProfile& Profile) const {
  return CapabilitySnapshot{
      .Channel = Profile.Channel,
      .Account = Profile.Account,
      .Mode = Profile.Mode,
      .SnapshotVersion = 0,
      .CapturedAt = Timestamp{},
      .ValidUntil = std::nullopt,
      .AuthenticationStatus = VerificationStatus::Unknown,
      .AuditStatus = VerificationStatus::Unknown,
      .Capabilities = {},
  };
}

ChannelReceipt MockChannelAdapter::MakeUnscriptedFailureReceipt(
    const Core::CoreAuthorizedAction& Action,
    ChannelErrorCode ErrorCode) const {
  return ChannelReceipt{
      .Action = Action.GetActionId(),
      .Operation = Action.GetOperationId(),
      .PlatformReceipt = std::nullopt,
      .OccurredAt = Timestamp{},
      .Status = ReceiptStatus::Failed,
      .ErrorCode = ErrorCode,
      .RetryAfter = std::nullopt,
  };
}

ChannelReceipt MockChannelAdapter::StoreSendActionReceipt(
    ChannelReceipt Receipt) {
  LastSendActionReceipt = Receipt;
  return Receipt;
}

} // namespace EmulativeDialog::Channels::Testing
