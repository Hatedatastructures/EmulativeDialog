#include "Source/Core/TurnRecoveryCoordinator.hpp"

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace EmulativeDialog::Core {
namespace {

Storage::ScopeState FindScopeState(const Storage::CheckpointSnapshot& Snapshot,
                                   std::string_view ScopeRef) {
  const auto Match = Snapshot.LatestScopeById.find(std::string(ScopeRef));
  if (Match == Snapshot.LatestScopeById.end()) {
    return Storage::ScopeState::Active;
  }
  return Match->second.State;
}

bool IsInactive(Storage::ScopeState State) noexcept {
  return State != Storage::ScopeState::Active;
}

bool HasUnsafeDispatch(const Storage::TurnCheckpoint& Checkpoint) noexcept {
  if (Checkpoint.UnknownSideEffect) {
    return true;
  }
  if (Checkpoint.ToolState == Storage::ToolExecutionState::Dispatched ||
      Checkpoint.ToolState == Storage::ToolExecutionState::UnknownOutcome) {
    return true;
  }
  if (Checkpoint.Operation == Storage::OperationState::Dispatched ||
      Checkpoint.Operation == Storage::OperationState::UnknownOutcome) {
    return true;
  }
  switch (Checkpoint.Phase) {
    case Storage::TurnPhase::ProviderDispatched:
    case Storage::TurnPhase::ProviderStreaming:
    case Storage::TurnPhase::Sending:
      return true;
    case Storage::TurnPhase::Created:
    case Storage::TurnPhase::ReadyToDispatch:
    case Storage::TurnPhase::ProviderCompleted:
    case Storage::TurnPhase::ToolDispatched:
    case Storage::TurnPhase::Completed:
    case Storage::TurnPhase::Cancelled:
    case Storage::TurnPhase::Failed:
    case Storage::TurnPhase::Unsupported:
    case Storage::TurnPhase::OutcomeUnknown:
      return false;
  }
  return true;
}

bool HasDefiniteOperationOutcome(
    const Storage::TurnCheckpoint& Checkpoint) noexcept {
  if (Checkpoint.UnknownSideEffect ||
      Checkpoint.ToolState == Storage::ToolExecutionState::Dispatched ||
      Checkpoint.ToolState == Storage::ToolExecutionState::UnknownOutcome) {
    return false;
  }
  switch (Checkpoint.Operation) {
    case Storage::OperationState::Succeeded:
    case Storage::OperationState::Rejected:
    case Storage::OperationState::Unsupported:
    case Storage::OperationState::Failed:
      return true;
    case Storage::OperationState::None:
    case Storage::OperationState::Prepared:
    case Storage::OperationState::Authorized:
      break;
    case Storage::OperationState::Dispatched:
    case Storage::OperationState::UnknownOutcome:
      return false;
  }
  return Checkpoint.ToolState == Storage::ToolExecutionState::Completed ||
         Checkpoint.ToolState == Storage::ToolExecutionState::Failed;
}

std::string_view StableCode(RecoveryDisposition Disposition) noexcept {
  switch (Disposition) {
    case RecoveryDisposition::SafeToResume:
      return "SafeToResume";
    case RecoveryDisposition::IsolatedOutcomeUnknown:
      return "OutcomeUnknown";
    case RecoveryDisposition::OperationReconciliationRequired:
      return "OperationReconciliationRequired";
    case RecoveryDisposition::Completed:
      return "Completed";
    case RecoveryDisposition::Cancelled:
      return "Cancelled";
    case RecoveryDisposition::Failed:
      return "Failed";
    case RecoveryDisposition::Unsupported:
      return "Unsupported";
    case RecoveryDisposition::OutcomeUnknown:
      return "OutcomeUnknown";
    case RecoveryDisposition::SkippedInactiveScope:
      return "InactiveScope";
  }
  return "Failed";
}

std::optional<RecoveryDisposition> TerminalDisposition(
    Storage::TurnPhase Phase) noexcept {
  switch (Phase) {
    case Storage::TurnPhase::Completed:
      return RecoveryDisposition::Completed;
    case Storage::TurnPhase::Cancelled:
      return RecoveryDisposition::Cancelled;
    case Storage::TurnPhase::Failed:
      return RecoveryDisposition::Failed;
    case Storage::TurnPhase::Unsupported:
      return RecoveryDisposition::Unsupported;
    case Storage::TurnPhase::OutcomeUnknown:
      return RecoveryDisposition::OutcomeUnknown;
    default:
      return std::nullopt;
  }
}

std::optional<Storage::TurnCheckpoint> BuildIsolationCheckpoint(
    const Storage::TurnCheckpoint& Checkpoint) {
  if (Checkpoint.Revision == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  Storage::TurnCheckpoint Isolated = Checkpoint;
  Isolated.Phase = Storage::TurnPhase::OutcomeUnknown;
  Isolated.UnknownSideEffect = true;
  Isolated.UpdatedAt = std::chrono::system_clock::now();
  ++Isolated.Revision;
  if (Isolated.ToolState != Storage::ToolExecutionState::NotRequested) {
    if (Isolated.ToolState == Storage::ToolExecutionState::Dispatched ||
        Isolated.ToolState == Storage::ToolExecutionState::UnknownOutcome) {
      Isolated.ToolState = Storage::ToolExecutionState::UnknownOutcome;
    }
  }
  const bool ProviderRequestInFlight =
      Checkpoint.Phase == Storage::TurnPhase::ProviderDispatched ||
      Checkpoint.Phase == Storage::TurnPhase::ProviderStreaming;
  // An unresolved provider request does not erase a separate known action result.
  const bool PreserveSuccessfulOperation =
      ProviderRequestInFlight &&
      Checkpoint.Operation == Storage::OperationState::Succeeded;
  if (!PreserveSuccessfulOperation &&
      (Isolated.Operation == Storage::OperationState::Dispatched ||
       Isolated.Operation == Storage::OperationState::UnknownOutcome ||
       ((Checkpoint.UnknownSideEffect ||
         Checkpoint.ToolState == Storage::ToolExecutionState::Dispatched ||
         Checkpoint.ToolState == Storage::ToolExecutionState::UnknownOutcome) &&
        Isolated.Operation != Storage::OperationState::None))) {
    Isolated.Operation = Storage::OperationState::UnknownOutcome;
  }
  return Isolated;
}

std::string RedactIfForbidden(std::string Value) {
  if (Storage::TurnCheckpointStore::ContainsForbiddenContent(Value)) {
    return "<redacted>";
  }
  return Value;
}

}  // namespace

TurnRecoveryCoordinator::TurnRecoveryCoordinator(
    Storage::TurnCheckpointStore& Store, EventSink EventSink)
    : StoreRef(Store), Sink(std::move(EventSink)) {}

std::variant<RecoveryReport, RecoveryError> TurnRecoveryCoordinator::Recover(
    std::string_view ScopeRef) {
  if (!Storage::TurnCheckpointStore::IsValidIdentifier(ScopeRef) ||
      !ScopeRef.starts_with("scope_")) {
    return RecoveryError{
        .Code = RecoveryErrorCode::InvalidScope,
        .SanitizedDescription = "Recovery scope identifier is invalid."};
  }

  const Storage::CheckpointResult<Storage::CheckpointSnapshot> Loaded =
      StoreRef.Load();
  if (const auto* Failure = std::get_if<Storage::CheckpointStoreError>(&Loaded);
      Failure != nullptr) {
    return RecoveryError{.Code = RecoveryErrorCode::StoreFailure,
                         .SanitizedDescription = Failure->SanitizedDescription};
  }

  const Storage::CheckpointSnapshot& Snapshot =
      std::get<Storage::CheckpointSnapshot>(Loaded);
  const Storage::ScopeState State = FindScopeState(Snapshot, ScopeRef);
  RecoveryReport Report{
      .ScopeRef = std::string(ScopeRef),
      .ScopeState = State,
      .Decisions = {},
      .CheckpointsPersisted = 0,
      .DispatchCount = 0,
  };

  for (const auto& [TurnId, Checkpoint] : Snapshot.LatestByTurn) {
    if (Checkpoint.ScopeRef != ScopeRef) {
      continue;
    }

    RecoveryDisposition Disposition = RecoveryDisposition::Failed;
    bool ReplayAllowed = false;
    if (IsInactive(State)) {
      Disposition = RecoveryDisposition::SkippedInactiveScope;
    } else if (const auto Terminal = TerminalDisposition(Checkpoint.Phase);
               Terminal.has_value() &&
               *Terminal != RecoveryDisposition::OutcomeUnknown &&
               HasUnsafeDispatch(Checkpoint)) {
      return RecoveryError{.Code = RecoveryErrorCode::StoreFailure,
                           .SanitizedDescription =
                               "Determinate terminal checkpoint contains an "
                               "unresolved dispatch."};
    } else if (const auto Terminal = TerminalDisposition(Checkpoint.Phase);
               Terminal.has_value()) {
      Disposition = *Terminal;
    } else if (HasUnsafeDispatch(Checkpoint)) {
      const std::optional<Storage::TurnCheckpoint> Isolated =
          BuildIsolationCheckpoint(Checkpoint);
      if (!Isolated.has_value()) {
        return RecoveryError{
            .Code = RecoveryErrorCode::StoreFailure,
            .SanitizedDescription =
                "Checkpoint revision cannot be advanced safely."};
      }
      const Storage::CheckpointResult<std::monostate> Persisted =
          StoreRef.Append(*Isolated);
      if (const auto* Failure =
              std::get_if<Storage::CheckpointStoreError>(&Persisted);
          Failure != nullptr) {
        return RecoveryError{
            .Code = RecoveryErrorCode::StoreFailure,
            .SanitizedDescription = Failure->SanitizedDescription};
      }
      ++Report.CheckpointsPersisted;
      Disposition = RecoveryDisposition::IsolatedOutcomeUnknown;
    } else if (HasDefiniteOperationOutcome(Checkpoint)) {
      // The operation result is durable and known. Surface its identity and
      // idempotency key for reconciliation, but never replay it automatically.
      // An unresolved Provider or Channel dispatch was handled above and must
      // not be hidden by this independent operation result.
      Disposition = RecoveryDisposition::OperationReconciliationRequired;
    } else {
      Disposition = RecoveryDisposition::SafeToResume;
      ReplayAllowed = true;
    }

    RecoveryDecision Decision{
        .TurnId = TurnId,
        .RequestId = Checkpoint.RequestId,
        .ContextReference = Checkpoint.ContextReference,
        .OriginalPhase = Checkpoint.Phase,
        .OriginalToolState = Checkpoint.ToolState,
        .Operation = Checkpoint.Operation,
        .OperationId = Checkpoint.OperationId,
        .IdempotencyKey = Checkpoint.IdempotencyKey,
        .ActionId = Checkpoint.ActionId,
        .Disposition = Disposition,
        .ReplayAllowed = ReplayAllowed,
        .StableCode = std::string(StableCode(Disposition)),
    };
    Report.Decisions.push_back(Decision);
    const std::optional<RecoveryError> Delivery = Deliver(Decision);
    if (Delivery.has_value()) {
      return *Delivery;
    }
  }
  return Report;
}

std::optional<RecoveryError> TurnRecoveryCoordinator::Deliver(
    const RecoveryDecision& Decision) {
  if (!Sink) {
    return std::nullopt;
  }
  try {
    Sink(RecoveryEvent{
        .SequenceNumber = NextEventSequence++,
        .Timestamp = std::chrono::system_clock::now(),
        .TurnId = RedactIfForbidden(Decision.TurnId),
        .RequestId = RedactIfForbidden(Decision.RequestId),
        .ContextReference = RedactIfForbidden(Decision.ContextReference),
        .OriginalPhase = Decision.OriginalPhase,
        .OriginalToolState = Decision.OriginalToolState,
        .Operation = Decision.Operation,
        .OperationId = RedactIfForbidden(Decision.OperationId),
        .IdempotencyKey = RedactIfForbidden(Decision.IdempotencyKey),
        .ActionId = RedactIfForbidden(Decision.ActionId),
        .Disposition = Decision.Disposition,
        .ReplayAllowed = Decision.ReplayAllowed,
        .StableCode = Decision.StableCode,
    });
  } catch (...) {
    return RecoveryError{
        .Code = RecoveryErrorCode::EventDeliveryFailure,
        .SanitizedDescription = "Recovery event delivery failed."};
  }
  return std::nullopt;
}

}  // namespace EmulativeDialog::Core
