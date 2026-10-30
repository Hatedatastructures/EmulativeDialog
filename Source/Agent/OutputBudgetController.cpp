#include "Source/Agent/OutputBudgetController.hpp"

#include <utility>

namespace EmulativeDialog::Agent {

OutputBudgetController& OutputBudgetController::Shared() noexcept {
  static OutputBudgetController Controller;
  return Controller;
}

bool OutputBudgetController::BeginTurn(
    const Providers::AgentTurnId& Turn,
    std::optional<std::uint32_t> MaximumOutputTokens) {
  if (Turn.Value.empty()) {
    return false;
  }
  std::lock_guard Lock(Mutex);
  TurnState State;
  State.MaximumOutputTokens = MaximumOutputTokens;
  return Turns.emplace(Turn.Value, std::move(State)).second;
}

void OutputBudgetController::EndTurn(
    const Providers::AgentTurnId& Turn) noexcept {
  try {
    std::lock_guard Lock(Mutex);
    Turns.erase(Turn.Value);
  } catch (...) {
  }
}

OutputBudgetRequestResult OutputBudgetController::BeginRequest(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request) {
  if (Turn.Value.empty() || Request.Value.empty()) {
    return OutputBudgetRequestResult::InvalidIdentifier;
  }
  std::lock_guard Lock(Mutex);
  const auto Entry = Turns.find(Turn.Value);
  if (Entry == Turns.end()) {
    return OutputBudgetRequestResult::UnknownTurn;
  }
  TurnState& State = Entry->second;
  if (State.RequestStarted) {
    return OutputBudgetRequestResult::RequestAlreadyStarted;
  }
  State.RequestStarted = true;
  State.ActiveRequest = Request.Value;
  return OutputBudgetRequestResult::Accepted;
}

bool OutputBudgetController::ObserveUsage(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request,
    const Providers::UsageSnapshot& Usage) noexcept {
  try {
    std::lock_guard Lock(Mutex);
    const auto Entry = Turns.find(Turn.Value);
    if (Entry == Turns.end() || !Entry->second.RequestStarted ||
        Entry->second.RequestCompleted ||
        Entry->second.ActiveRequest != Request.Value) {
      return false;
    }
    ApplyUsage(Entry->second, Usage);
    return true;
  } catch (...) {
    return false;
  }
}

bool OutputBudgetController::CompleteRequest(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request,
    const std::optional<Providers::UsageSnapshot>& FinalUsage,
    bool OutputLimited,
    bool ToolHandoff) noexcept {
  try {
    std::lock_guard Lock(Mutex);
    const auto Entry = Turns.find(Turn.Value);
    if (Entry == Turns.end() || !Entry->second.RequestStarted ||
        Entry->second.RequestCompleted ||
        Entry->second.ActiveRequest != Request.Value) {
      return false;
    }
    TurnState& State = Entry->second;
    if (FinalUsage.has_value()) {
      ApplyUsage(State, *FinalUsage);
    }
    State.OutputLimited = OutputLimited;
    State.ToolHandoff = ToolHandoff;
    State.RequestCompleted = true;
    return true;
  } catch (...) {
    return false;
  }
}

bool OutputBudgetController::CanDispatchTool(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request) const noexcept {
  try {
    std::lock_guard Lock(Mutex);
    const auto Entry = Turns.find(Turn.Value);
    if (Entry == Turns.end()) {
      return false;
    }
    const TurnState& State = Entry->second;
    if (!State.RequestCompleted || State.OutputLimited || !State.ToolHandoff ||
        State.ActiveRequest != Request.Value ||
        State.LatestUsage.Completeness != Providers::UsageCompleteness::Final ||
        !State.LatestUsage.OutputTokens.has_value() ||
        State.OutputBudgetExhausted) {
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool OutputBudgetController::CanContinue(
    const Providers::AgentTurnId& Turn) const noexcept {
  static_cast<void>(Turn);
  // R5 deliberately has no Core-approved compression/continuation seam.
  return false;
}

bool OutputBudgetController::HasActiveTurn(
    const Providers::AgentTurnId& Turn) const noexcept {
  try {
    std::lock_guard Lock(Mutex);
    return Turns.contains(Turn.Value);
  } catch (...) {
    return false;
  }
}

std::optional<OutputBudgetTurnSnapshot> OutputBudgetController::Inspect(
    const Providers::AgentTurnId& Turn) const noexcept {
  try {
    std::lock_guard Lock(Mutex);
    const auto Entry = Turns.find(Turn.Value);
    if (Entry == Turns.end()) {
      return std::nullopt;
    }
    const TurnState& State = Entry->second;
    return OutputBudgetTurnSnapshot{
        .Active = true,
        .RequestStarted = State.RequestStarted,
        .RequestCompleted = State.RequestCompleted,
        .OutputLimited = State.OutputLimited,
        .ToolHandoff = State.ToolHandoff,
        .UsageObserved = State.UsageObserved,
        .OutputBudgetExhausted = State.OutputBudgetExhausted,
        .ContinuationAuthorized = false,
        .MaximumOutputTokens = State.MaximumOutputTokens,
        .OutputTokens = State.LatestUsage.OutputTokens,
        .UsageCompleteness = State.LatestUsage.Completeness,
    };
  } catch (...) {
    return std::nullopt;
  }
}

void OutputBudgetController::ApplyUsage(
    TurnState& State,
    const Providers::UsageSnapshot& Usage) noexcept {
  State.UsageObserved = true;
  if (Usage.Completeness == Providers::UsageCompleteness::Unavailable) {
    State.LatestUsage = Providers::UsageSnapshot{};
    return;
  }
  // Snapshots replace prior snapshots; they are never summed or inferred.
  State.LatestUsage = Usage;
  if (State.MaximumOutputTokens.has_value() &&
      Usage.OutputTokens.has_value() &&
      *Usage.OutputTokens >= *State.MaximumOutputTokens) {
    State.OutputBudgetExhausted = true;
  }
}

} // namespace EmulativeDialog::Agent
