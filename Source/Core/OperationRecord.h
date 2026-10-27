#pragma once

#include <cstdint>
#include <optional>

#include "Source/Channels/Contracts/ChannelAdapterPort.h"
#include "Source/Core/IdentityPolicy.h"
#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Core {

enum class OperationState : std::uint8_t {
  Prepared,
  Authorized,
  Dispatched,
  Succeeded,
  Rejected,
  Unsupported,
  Failed,
  UnknownOutcome,
};

class OperationRecord final {
public:
  OperationRecord(Channels::OperationId OperationId,
                  Channels::IdempotencyKey IdempotencyKey,
                  Providers::AgentTurnId AgentTurn,
                  Channels::ActionId ActionId,
                  ConversationScope Scope,
                  std::uint64_t PolicyVersion,
                  std::uint64_t CapabilitySnapshotVersion);

  [[nodiscard]] bool Authorize() noexcept;
  [[nodiscard]] bool MarkDispatched() noexcept;
  [[nodiscard]] bool FailBeforeDispatch(Channels::ChannelErrorCode ErrorCode) noexcept;
  [[nodiscard]] bool MarkUnknownOutcome(Channels::ChannelErrorCode ErrorCode) noexcept;
  [[nodiscard]] bool ApplyReceipt(const Channels::ChannelReceipt& Receipt) noexcept;

  [[nodiscard]] OperationState GetState() const noexcept;
  [[nodiscard]] const Channels::OperationId& GetOperationId() const noexcept;
  [[nodiscard]] const Channels::IdempotencyKey& GetIdempotencyKey() const noexcept;
  [[nodiscard]] const Providers::AgentTurnId& GetAgentTurnId() const noexcept;
  [[nodiscard]] const Channels::ActionId& GetActionId() const noexcept;
  [[nodiscard]] const ConversationScope& GetScope() const noexcept;
  [[nodiscard]] std::uint64_t GetPolicyVersion() const noexcept;
  [[nodiscard]] std::uint64_t GetCapabilitySnapshotVersion() const noexcept;
  [[nodiscard]] std::optional<Channels::ChannelErrorCode> GetErrorCode() const noexcept;

private:
  Channels::OperationId OperationId;
  Channels::IdempotencyKey IdempotencyKey;
  Providers::AgentTurnId AgentTurn;
  Channels::ActionId ActionId;
  ConversationScope Scope;
  std::uint64_t PolicyVersion{};
  std::uint64_t CapabilitySnapshotVersion{};
  OperationState State{OperationState::Prepared};
  std::optional<Channels::ChannelErrorCode> ErrorCode;
};

class OperationRecordObserver {
public:
  virtual ~OperationRecordObserver() = default;
  virtual void OnOperationRecordChanged(const OperationRecord& Record) noexcept = 0;
};

} // namespace EmulativeDialog::Core
