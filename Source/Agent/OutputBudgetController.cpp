#include "Source/Agent/OutputBudgetController.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace EmulativeDialog::Agent {
namespace {

std::uint64_t SaturatingAdd(std::uint64_t Left,
                            std::uint64_t Right) noexcept {
  if (Left > std::numeric_limits<std::uint64_t>::max() - Right) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return Left + Right;
}

std::uint64_t SaturatingRemaining(std::uint64_t Limit,
                                  std::uint64_t Used) noexcept {
  return Used >= Limit ? 0 : Limit - Used;
}

unsigned CompletenessRank(Providers::UsageCompleteness Completeness) noexcept {
  switch (Completeness) {
  case Providers::UsageCompleteness::Final:
    return 0;
  case Providers::UsageCompleteness::Partial:
    return 1;
  case Providers::UsageCompleteness::Unavailable:
    return 2;
  }
  return 2;
}

Providers::UsageCompleteness MergeCompleteness(
    Providers::UsageCompleteness Current,
    Providers::UsageCompleteness Next) noexcept {
  return CompletenessRank(Current) >= CompletenessRank(Next) ? Current : Next;
}

bool UsageIsConsistent(const Providers::UsageSnapshot& Usage) noexcept {
  if (!Usage.InputTokens.has_value() || !Usage.OutputTokens.has_value() ||
      !Usage.TotalTokens.has_value()) {
    return true;
  }
  const std::uint64_t Sum = SaturatingAdd(*Usage.InputTokens,
                                          *Usage.OutputTokens);
  return Sum == *Usage.TotalTokens;
}

} // namespace

OutputBudgetController::OutputBudgetController(OutputBudgetLimits LimitsValue,
                                               EventSink SinkValue)
    : Limits(std::move(LimitsValue)), Sink(std::move(SinkValue)) {
  UpdateDerivedState();
}

std::optional<OutputBudgetError> OutputBudgetController::Accumulate(
    const Providers::UsageSnapshot& Usage) {
  if (!UsageIsConsistent(Usage)) {
    return OutputBudgetError{
        .Code = OutputBudgetErrorCode::InconsistentUsage};
  }

  const bool PreviousOutputReached = State.OutputLimitReached;
  const bool PreviousContextReached = State.ContextWindowLimitReached;

  if (Usage.InputTokens.has_value()) {
    State.InputTokensUsed =
        SaturatingAdd(State.InputTokensUsed, *Usage.InputTokens);
  }
  if (Usage.OutputTokens.has_value()) {
    State.OutputTokensUsed =
        SaturatingAdd(State.OutputTokensUsed, *Usage.OutputTokens);
  }
  State.Completeness =
      HasUsage ? MergeCompleteness(State.Completeness, Usage.Completeness)
               : Usage.Completeness;
  HasUsage = true;
  ++Samples;

  UpdateDerivedState();
  Emit(OutputBudgetEventCode::UsageAccumulated);
  if (!PreviousOutputReached && State.OutputLimitReached) {
    Emit(OutputBudgetEventCode::OutputLimitReached);
  }
  if (!PreviousContextReached && State.ContextWindowLimitReached) {
    Emit(OutputBudgetEventCode::ContextWindowLimitReached);
  }
  return std::nullopt;
}

std::optional<OutputBudgetError> OutputBudgetController::RecordProviderOutputLimited(
    Providers::LimitKind Kind) {
  const bool AlreadyReached =
      Kind == Providers::LimitKind::OutputTokens
          ? State.OutputLimitReached
          : State.ContextWindowLimitReached;
  if (Kind == Providers::LimitKind::OutputTokens) {
    State.OutputLimitReached = true;
  } else {
    State.ContextWindowLimitReached = true;
  }
  UpdateDerivedState();
  if (!AlreadyReached) {
    Emit(OutputBudgetEventCode::ProviderOutputLimited);
  }
  return std::nullopt;
}

void OutputBudgetController::UpdateDerivedState() {
  State.TotalTokensUsed =
      SaturatingAdd(State.InputTokensUsed, State.OutputTokensUsed);
  State.RemainingOutputTokens =
      Limits.MaximumOutputTokens.has_value()
          ? std::optional<std::uint64_t>{SaturatingRemaining(
                *Limits.MaximumOutputTokens, State.OutputTokensUsed)}
          : std::nullopt;
  State.RemainingContextTokens =
      Limits.ContextWindowTokens.has_value()
          ? std::optional<std::uint64_t>{SaturatingRemaining(
                *Limits.ContextWindowTokens, State.TotalTokensUsed)}
          : std::nullopt;
  if (Limits.MaximumOutputTokens.has_value() &&
      State.OutputTokensUsed >= *Limits.MaximumOutputTokens) {
    State.OutputLimitReached = true;
  }
  if (Limits.ContextWindowTokens.has_value() &&
      State.TotalTokensUsed >= *Limits.ContextWindowTokens) {
    State.ContextWindowLimitReached = true;
  }
  State.CanContinueSampling =
      !State.OutputLimitReached && !State.ContextWindowLimitReached;
}

void OutputBudgetController::Emit(OutputBudgetEventCode Code) {
  ++Events;
  if (Sink) {
    Sink(OutputBudgetEvent{.Code = Code, .State = State});
  }
}

} // namespace EmulativeDialog::Agent
