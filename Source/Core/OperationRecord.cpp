#include "Source/Core/OperationRecord.h"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace EmulativeDialog::Core {
namespace {

bool IsNonEmpty(std::string_view Value) noexcept {
  return !Value.empty();
}

} // namespace

OperationRecord::OperationRecord(Channels::OperationId OperationId,
                                 Channels::IdempotencyKey IdempotencyKey,
                                 Providers::AgentTurnId AgentTurn,
                                 Channels::ActionId ActionId,
                                 ConversationScope Scope,
                                 std::uint64_t PolicyVersion,
                                 std::uint64_t CapabilitySnapshotVersion)
    : OperationId(std::move(OperationId)),
      IdempotencyKey(std::move(IdempotencyKey)),
      AgentTurn(std::move(AgentTurn)),
      ActionId(std::move(ActionId)),
      Scope(std::move(Scope)),
      PolicyVersion(PolicyVersion),
      CapabilitySnapshotVersion(CapabilitySnapshotVersion) {
  if (!IsNonEmpty(this->OperationId.Value) ||
      !IsNonEmpty(this->IdempotencyKey.Value) ||
      !IsNonEmpty(this->AgentTurn.Value) || !IsNonEmpty(this->ActionId.Value) ||
      !IsNonEmpty(this->Scope.Principal.Value) ||
      !IsNonEmpty(this->Scope.Channel.Value) ||
      !IsNonEmpty(this->Scope.Account.Value) ||
      !IsNonEmpty(this->Scope.Mode.Value) ||
      !IsNonEmpty(this->Scope.Conversation.Value) ||
      !IsNonEmpty(this->Scope.Sender.Value) || this->PolicyVersion == 0 ||
      this->CapabilitySnapshotVersion == 0) {
    throw std::invalid_argument("OperationRecord requires complete scoped identifiers and versions");
  }
}

bool OperationRecord::Authorize() noexcept {
  if (State != OperationState::Prepared) {
    return false;
  }
  State = OperationState::Authorized;
  return true;
}

bool OperationRecord::MarkDispatched() noexcept {
  if (State != OperationState::Authorized) {
    return false;
  }
  State = OperationState::Dispatched;
  return true;
}

bool OperationRecord::FailBeforeDispatch(
    Channels::ChannelErrorCode ErrorCode) noexcept {
  if (State != OperationState::Prepared && State != OperationState::Authorized) {
    return false;
  }
  State = OperationState::Failed;
  this->ErrorCode = ErrorCode;
  return true;
}

bool OperationRecord::MarkUnknownOutcome(
    Channels::ChannelErrorCode ErrorCode) noexcept {
  if (State != OperationState::Dispatched) {
    return false;
  }
  State = OperationState::UnknownOutcome;
  this->ErrorCode = ErrorCode == Channels::ChannelErrorCode::None
                        ? Channels::ChannelErrorCode::OutcomeUnknown
                        : ErrorCode;
  return true;
}

bool OperationRecord::ApplyReceipt(const Channels::ChannelReceipt& Receipt) noexcept {
  if (State != OperationState::Dispatched) {
    return false;
  }
  if (Receipt.Action.Value != ActionId.Value ||
      Receipt.Operation.Value != OperationId.Value) {
    State = OperationState::UnknownOutcome;
    ErrorCode = Channels::ChannelErrorCode::OutcomeUnknown;
    return false;
  }

  switch (Receipt.Status) {
  case Channels::ReceiptStatus::Succeeded:
    State = OperationState::Succeeded;
    break;
  case Channels::ReceiptStatus::Rejected:
  case Channels::ReceiptStatus::Forbidden:
    State = OperationState::Rejected;
    break;
  case Channels::ReceiptStatus::UnsupportedCapability:
    State = OperationState::Unsupported;
    break;
  case Channels::ReceiptStatus::RateLimited:
  case Channels::ReceiptStatus::Failed:
    State = OperationState::Failed;
    break;
  case Channels::ReceiptStatus::UnknownOutcome:
    State = OperationState::UnknownOutcome;
    break;
  }

  if (State == OperationState::Succeeded) {
    ErrorCode.reset();
  } else if (State == OperationState::UnknownOutcome) {
    ErrorCode = Receipt.ErrorCode == Channels::ChannelErrorCode::None
                    ? Channels::ChannelErrorCode::OutcomeUnknown
                    : Receipt.ErrorCode;
  } else {
    ErrorCode = Receipt.ErrorCode == Channels::ChannelErrorCode::None
                    ? std::optional<Channels::ChannelErrorCode>{}
                    : std::optional<Channels::ChannelErrorCode>{Receipt.ErrorCode};
  }
  return true;
}

OperationState OperationRecord::GetState() const noexcept {
  return State;
}

const Channels::OperationId& OperationRecord::GetOperationId() const noexcept {
  return OperationId;
}

const Channels::IdempotencyKey& OperationRecord::GetIdempotencyKey() const noexcept {
  return IdempotencyKey;
}

const Providers::AgentTurnId& OperationRecord::GetAgentTurnId() const noexcept {
  return AgentTurn;
}

const Channels::ActionId& OperationRecord::GetActionId() const noexcept {
  return ActionId;
}

const ConversationScope& OperationRecord::GetScope() const noexcept {
  return Scope;
}

std::uint64_t OperationRecord::GetPolicyVersion() const noexcept {
  return PolicyVersion;
}

std::uint64_t OperationRecord::GetCapabilitySnapshotVersion() const noexcept {
  return CapabilitySnapshotVersion;
}

std::optional<Channels::ChannelErrorCode> OperationRecord::GetErrorCode() const noexcept {
  return ErrorCode;
}

} // namespace EmulativeDialog::Core
