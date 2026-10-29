#pragma once

#include "Source/Storage/TurnCheckpointStore.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace EmulativeDialog::Core {

enum class RecoveryDisposition : std::uint8_t {
  SafeToResume,
  IsolatedOutcomeUnknown,
  OperationReconciliationRequired,
  Completed,
  Cancelled,
  Failed,
  Unsupported,
  OutcomeUnknown,
  SkippedInactiveScope
};

struct RecoveryDecision final {
  std::string TurnId;
  std::string RequestId;
  std::string ContextReference;
  Storage::TurnPhase OriginalPhase{Storage::TurnPhase::Created};
  Storage::ToolExecutionState OriginalToolState{
      Storage::ToolExecutionState::NotRequested};
  Storage::OperationState Operation{Storage::OperationState::None};
  std::string OperationId;
  std::string IdempotencyKey;
  std::string ActionId;
  RecoveryDisposition Disposition{RecoveryDisposition::SafeToResume};
  bool ReplayAllowed{false};
  std::string StableCode;
};

struct RecoveryEvent final {
  std::uint64_t SequenceNumber{0};
  std::chrono::system_clock::time_point Timestamp{};
  std::string TurnId;
  std::string RequestId;
  std::string ContextReference;
  Storage::TurnPhase OriginalPhase{Storage::TurnPhase::Created};
  Storage::ToolExecutionState OriginalToolState{
      Storage::ToolExecutionState::NotRequested};
  Storage::OperationState Operation{Storage::OperationState::None};
  std::string OperationId;
  std::string IdempotencyKey;
  std::string ActionId;
  RecoveryDisposition Disposition{RecoveryDisposition::SafeToResume};
  bool ReplayAllowed{false};
  std::string StableCode;
};

enum class RecoveryErrorCode : std::uint8_t {
  StoreFailure,
  InvalidScope,
  EventDeliveryFailure
};

struct RecoveryError final {
  RecoveryErrorCode Code{RecoveryErrorCode::StoreFailure};
  // Stable, sanitized diagnostics only. Never include identifiers, message
  // text, credentials, prompts, tool arguments, or store input.
  std::string SanitizedDescription;
};

struct RecoveryReport final {
  std::string ScopeRef;
  Storage::ScopeState ScopeState{Storage::ScopeState::Active};
  std::vector<RecoveryDecision> Decisions;
  std::uint64_t CheckpointsPersisted{0};
  // Recovery exposes decisions to Core and never dispatches Provider,
  // Channel, network, or tool operations itself.
  std::uint64_t DispatchCount{0};
};

// Scans append-only turn checkpoints after restart. Turns that never crossed a
// Provider or external-action dispatch boundary may be resumed. Any request,
// stream, tool, or send state whose completion cannot be proven is persisted
// once as OutcomeUnknown and is never replayed automatically.
class TurnRecoveryCoordinator final {
public:
  using EventSink = std::function<void(const RecoveryEvent&)>;

  explicit TurnRecoveryCoordinator(Storage::TurnCheckpointStore& Store,
                                   EventSink Sink = {});

  [[nodiscard]] std::variant<RecoveryReport, RecoveryError> Recover(
      std::string_view ScopeRef);

private:
  [[nodiscard]] std::optional<RecoveryError> Deliver(
      const RecoveryDecision& Decision);

  Storage::TurnCheckpointStore& StoreRef;
  EventSink Sink;
  std::uint64_t NextEventSequence{1};
};

} // namespace EmulativeDialog::Core
