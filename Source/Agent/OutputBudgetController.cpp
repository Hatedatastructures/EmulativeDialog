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
  const bool IsFinalUsage =
      Usage.Completeness == Providers::UsageCompleteness::Final;
  const bool SampleHasExactTotal = IsFinalUsage &&
      (Usage.TotalTokens.has_value() ||
       (Usage.InputTokens.has_value() && Usage.OutputTokens.has_value()));
  std::optional<std::uint64_t> SampleTotal;
  if (Usage.TotalTokens.has_value()) {
    SampleTotal = *Usage.TotalTokens;
  } else if (Usage.InputTokens.has_value() &&
             Usage.OutputTokens.has_value()) {
    SampleTotal =
        SaturatingAdd(*Usage.InputTokens, *Usage.OutputTokens);
  }

  if (Usage.InputTokens.has_value()) {
    State.InputTokensUsed =
        SaturatingAdd(State.InputTokensUsed, *Usage.InputTokens);
  }
  if (Usage.OutputTokens.has_value()) {
    State.OutputTokensUsed =
        SaturatingAdd(State.OutputTokensUsed, *Usage.OutputTokens);
  }
  if (SampleTotal.has_value()) {
    State.TotalTokensUsed =
        SaturatingAdd(State.TotalTokensUsed, *SampleTotal);
  }
  TotalUsageExact = TotalUsageExact && SampleHasExactTotal;
  OutputUsageExact = OutputUsageExact && IsFinalUsage &&
                     Usage.OutputTokens.has_value();
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
    ProviderOutputLimitTerminal = true;
    State.OutputLimitReached = true;
  } else {
    ProviderContextLimitTerminal = true;
    State.ContextWindowLimitReached = true;
  }
  UpdateDerivedState();
  if (!AlreadyReached) {
    Emit(OutputBudgetEventCode::ProviderOutputLimited);
  }
  return std::nullopt;
}

void OutputBudgetController::UpdateDerivedState() {
  if (!Limits.MaximumOutputTokens.has_value()) {
    State.RemainingOutputTokens = std::nullopt;
  } else if (ProviderOutputLimitTerminal) {
    State.RemainingOutputTokens = 0;
  } else if (!HasUsage) {
    State.RemainingOutputTokens = *Limits.MaximumOutputTokens;
  } else if (OutputUsageExact) {
    State.RemainingOutputTokens = SaturatingRemaining(
        *Limits.MaximumOutputTokens, State.OutputTokensUsed);
  } else {
    State.RemainingOutputTokens = std::nullopt;
  }
  if (!Limits.ContextWindowTokens.has_value()) {
    State.RemainingContextTokens = std::nullopt;
  } else if (ProviderContextLimitTerminal) {
    State.RemainingContextTokens = 0;
  } else if (!HasUsage) {
    State.RemainingContextTokens = *Limits.ContextWindowTokens;
  } else if (TotalUsageExact) {
    State.RemainingContextTokens = SaturatingRemaining(
        *Limits.ContextWindowTokens, State.TotalTokensUsed);
  } else {
    State.RemainingContextTokens = std::nullopt;
  }
  if (Limits.MaximumOutputTokens.has_value() &&
      State.OutputTokensUsed >= *Limits.MaximumOutputTokens) {
    State.OutputLimitReached = true;
  }
  if (Limits.ContextWindowTokens.has_value() &&
      State.TotalTokensUsed >= *Limits.ContextWindowTokens) {
    State.ContextWindowLimitReached = true;
  }
  const bool OutputBudgetUnknown =
      Limits.MaximumOutputTokens.has_value() && HasUsage &&
      !OutputUsageExact;
  const bool ContextBudgetUnknown =
      Limits.ContextWindowTokens.has_value() && HasUsage &&
      !TotalUsageExact;
  State.BudgetConfidenceInsufficient =
      OutputBudgetUnknown || ContextBudgetUnknown;
  State.CanContinueSampling =
      !State.OutputLimitReached && !State.ContextWindowLimitReached &&
      !State.BudgetConfidenceInsufficient;
}

void OutputBudgetController::Emit(OutputBudgetEventCode Code) {
  ++Events;
  if (Sink) {
    Sink(OutputBudgetEvent{.Code = Code, .State = State});
  }
}

void TurnBudgetLedger::ConfigureTurn(Providers::AgentTurnId Turn,
                                    OutputBudgetLimits LimitsValue) {
  std::scoped_lock Lock(Mutex);
  const auto [Iterator, Inserted] =
      Turns.try_emplace(Turn.Value);
  if (Inserted) {
    Iterator->second.Limits = std::move(LimitsValue);
  }
}

std::optional<TurnBudgetError> TurnBudgetLedger::BeginRequest(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request) {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::TurnNotConfigured};
  }
  if (Request.Value.empty()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::InvalidRequestId};
  }

  TurnState& Current = Iterator->second;
  if (Current.SeenRequests.contains(Request.Value)) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::RequestIdReused};
  }
  if (Current.CurrentRequest.has_value() && Current.Budget.has_value() &&
      !Current.Budget->CanDispatchSampling()) {
    if (!Current.ApprovedContinuationFrom.has_value() ||
        *Current.ApprovedContinuationFrom != *Current.CurrentRequest) {
      return TurnBudgetError{
          .Code = TurnBudgetErrorCode::ContinuationNotApproved};
    }
    Current.ApprovedContinuationFrom.reset();
  }

  Current.SeenRequests.insert(Request.Value);
  Current.CurrentRequest = Request.Value;
  Current.Budget.emplace(Current.Limits);
  return std::nullopt;
}

std::optional<TurnBudgetError> TurnBudgetLedger::ApproveContinuation(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& PreviousRequest) {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::TurnNotConfigured};
  }
  TurnState& Current = Iterator->second;
  if (!Current.CurrentRequest.has_value() ||
      *Current.CurrentRequest != PreviousRequest.Value) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::RequestNotCurrent};
  }
  if (!Current.Budget.has_value() ||
      Current.Budget->CanDispatchSampling()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::ContinuationNotRequired};
  }
  Current.ApprovedContinuationFrom = PreviousRequest.Value;
  return std::nullopt;
}

std::optional<TurnBudgetError> TurnBudgetLedger::Accumulate(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request,
    const Providers::UsageSnapshot& Usage) {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::TurnNotConfigured};
  }
  TurnState& Current = Iterator->second;
  if (!Current.CurrentRequest.has_value() ||
      *Current.CurrentRequest != Request.Value ||
      !Current.Budget.has_value()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::RequestNotCurrent};
  }
  if (Current.Budget->Accumulate(Usage).has_value()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::InconsistentUsage};
  }
  return std::nullopt;
}

std::optional<TurnBudgetError>
TurnBudgetLedger::RecordProviderOutputLimited(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request,
    Providers::LimitKind Kind) {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::TurnNotConfigured};
  }
  TurnState& Current = Iterator->second;
  if (!Current.CurrentRequest.has_value() ||
      *Current.CurrentRequest != Request.Value ||
      !Current.Budget.has_value()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::RequestNotCurrent};
  }
  if (Current.Budget->RecordProviderOutputLimited(Kind).has_value()) {
    return TurnBudgetError{
        .Code = TurnBudgetErrorCode::InconsistentUsage};
  }
  return std::nullopt;
}

bool TurnBudgetLedger::CanDispatchSampling(
    const Providers::AgentTurnId& Turn) const {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end()) {
    return true;
  }
  const TurnState& Current = Iterator->second;
  return !Current.Budget.has_value() ||
         Current.Budget->CanDispatchSampling();
}

bool TurnBudgetLedger::CanDispatchTool(
    const Providers::AgentTurnId& Turn,
    const Providers::RequestId& Request) const {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end()) {
    return true;
  }
  const TurnState& Current = Iterator->second;
  return Current.CurrentRequest.has_value() &&
         *Current.CurrentRequest == Request.Value &&
         Current.Budget.has_value() &&
         Current.Budget->CanDispatchSampling();
}

std::optional<Providers::RequestId> TurnBudgetLedger::CurrentRequest(
    const Providers::AgentTurnId& Turn) const {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end() ||
      !Iterator->second.CurrentRequest.has_value()) {
    return std::nullopt;
  }
  return Providers::RequestId{
      *Iterator->second.CurrentRequest};
}

std::optional<OutputBudgetState> TurnBudgetLedger::GetState(
    const Providers::AgentTurnId& Turn) const {
  std::scoped_lock Lock(Mutex);
  const auto Iterator = Turns.find(Turn.Value);
  if (Iterator == Turns.end() ||
      !Iterator->second.Budget.has_value()) {
    return std::nullopt;
  }
  return Iterator->second.Budget->GetState();
}

void TurnBudgetLedger::EraseTurn(
    const Providers::AgentTurnId& Turn) noexcept {
  std::scoped_lock Lock(Mutex);
  Turns.erase(Turn.Value);
}

void TurnBudgetLedger::Reset() noexcept {
  std::scoped_lock Lock(Mutex);
  Turns.clear();
}

TurnBudgetLedger& SharedTurnBudgetLedger() {
  static TurnBudgetLedger Ledger;
  return Ledger;
}

} // namespace EmulativeDialog::Agent
