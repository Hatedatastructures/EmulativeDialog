#include "Source/Agent/OutputBudgetController.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>

namespace {

using namespace EmulativeDialog;

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

struct EventCollector final {
  std::vector<Agent::OutputBudgetEvent> Events;

  Agent::OutputBudgetController::EventSink Sink() {
    return [this](const Agent::OutputBudgetEvent& Event) {
      Events.push_back(Event);
    };
  }
};

Providers::UsageSnapshot Usage(std::optional<std::uint64_t> Input,
                               std::optional<std::uint64_t> Output,
                               std::optional<std::uint64_t> Total = std::nullopt,
                               Providers::UsageCompleteness Completeness =
                                   Providers::UsageCompleteness::Final) {
  return Providers::UsageSnapshot{
      .InputTokens = Input,
      .OutputTokens = Output,
      .TotalTokens = Total,
      .CachedInputReadTokens = std::nullopt,
      .CachedInputWriteTokens = std::nullopt,
      .ReasoningOutputTokens = std::nullopt,
      .Completeness = Completeness,
  };
}

Agent::OutputBudgetLimits Limits(
    std::optional<std::uint64_t> MaximumOutput = std::nullopt,
    std::optional<std::uint64_t> ContextWindow = std::nullopt) {
  return Agent::OutputBudgetLimits{
      .MaximumOutputTokens = MaximumOutput,
      .ContextWindowTokens = ContextWindow,
  };
}

void AccumulatesWithoutLimits() {
  EventCollector Collected;
  Agent::OutputBudgetController Controller({}, Collected.Sink());
  Check(!Controller.Accumulate(Usage(10, 20, 30)).has_value(),
        "usage without limits must accumulate");
  const auto& State = Controller.GetState();
  Check(State.InputTokensUsed == 10U && State.OutputTokensUsed == 20U &&
            State.TotalTokensUsed == 30U,
        "usage counters must sum");
  Check(!State.RemainingOutputTokens.has_value() &&
            !State.RemainingContextTokens.has_value(),
        "unlimited budgets must not report remaining values");
  Check(State.CanContinueSampling && Controller.CanDispatchSampling(),
        "unlimited budgets may continue");
  Check(State.Completeness == Providers::UsageCompleteness::Final,
        "completeness must follow the first sample");
  Check(Collected.Events.size() == 1U &&
            Collected.Events[0].Code ==
                Agent::OutputBudgetEventCode::UsageAccumulated,
        "accumulation must emit one usage event");
}

void OutputLimitTransitionsBlockSampling() {
  EventCollector Collected;
  Agent::OutputBudgetController Controller(Limits(100), Collected.Sink());
  Check(!Controller.Accumulate(Usage(50, 40)).has_value(),
        "first sample must accumulate");
  Check(Controller.GetState().RemainingOutputTokens == 60U,
        "remaining output must subtract used output");
  Check(Controller.CanDispatchSampling(),
        "budget below the limit may continue");

  Check(!Controller.Accumulate(Usage(50, 60)).has_value(),
        "second sample must accumulate");
  Check(Controller.GetState().OutputLimitReached &&
            !Controller.CanDispatchSampling(),
        "reaching the output limit must block new sampling");
  Check(Controller.GetState().RemainingOutputTokens == 0U,
        "exhausted output budget must report zero");
  Check(Collected.Events.size() == 3U &&
            Collected.Events[2].Code ==
                Agent::OutputBudgetEventCode::OutputLimitReached,
        "limit transition must emit exactly one event");

  Check(!Controller.Accumulate(Usage(std::nullopt, 10)).has_value(),
        "post-limit usage may still be recorded");
  Check(Collected.Events.size() == 4U &&
            Controller.GetState().OutputTokensUsed == 110U,
        "post-limit accumulation must not repeat the transition event");
}

void ContextWindowTransitionsBlockSampling() {
  EventCollector Collected;
  Agent::OutputBudgetController Controller(
      Limits(std::nullopt, 100), Collected.Sink());
  Check(!Controller.Accumulate(Usage(60, 20)).has_value(),
        "first context sample must accumulate");
  Check(Controller.GetState().RemainingContextTokens == 20U &&
            Controller.CanDispatchSampling(),
        "context budget must report remaining tokens");
  Check(!Controller.Accumulate(Usage(0, 20)).has_value(),
        "second context sample must accumulate");
  Check(Controller.GetState().ContextWindowLimitReached &&
            !Controller.CanDispatchSampling(),
        "context exhaustion must block new sampling");
  Check(Collected.Events.back().Code ==
            Agent::OutputBudgetEventCode::ContextWindowLimitReached,
        "context exhaustion must emit a context event");
}

void SaturatesInsteadOfOverflowing() {
  constexpr auto Maximum = std::numeric_limits<std::uint64_t>::max();
  EventCollector Collected;
  Agent::OutputBudgetController Controller(
      Limits(Maximum, Maximum), Collected.Sink());
  Check(!Controller.Accumulate(Usage(Maximum - 1U, 5U)).has_value(),
        "saturating sample must accumulate");
  const auto& State = Controller.GetState();
  Check(State.InputTokensUsed == Maximum - 1U &&
            State.OutputTokensUsed == 5U &&
            State.TotalTokensUsed == Maximum,
        "total usage must saturate at the maximum value");
  Check(State.RemainingContextTokens == 0U &&
            State.ContextWindowLimitReached &&
            !Controller.CanDispatchSampling(),
        "saturated context use must exhaust the window");
  Check(!Controller.Accumulate(Usage(Maximum, Maximum)).has_value(),
        "second saturating sample must accumulate");
  Check(Controller.GetState().InputTokensUsed == Maximum &&
            Controller.GetState().OutputTokensUsed == Maximum &&
            Controller.GetState().TotalTokensUsed == Maximum,
        "all counters must remain saturated");
}

void RejectsInconsistentTotalsWithoutMutation() {
  EventCollector Collected;
  Agent::OutputBudgetController Controller({}, Collected.Sink());
  const auto Error = Controller.Accumulate(Usage(1, 2, 4));
  Check(Error.has_value() &&
            Error->Code == Agent::OutputBudgetErrorCode::InconsistentUsage,
        "inconsistent totals must fail closed");
  Check(Controller.GetState().TotalTokensUsed == 0U &&
            Controller.AccumulatedSamples() == 0U &&
            Controller.EventCount() == 0U && Collected.Events.empty(),
        "rejected usage must not mutate or emit");
}

void ProviderOutputLimitedLinksToBudget() {
  EventCollector Collected;
  Agent::OutputBudgetController Controller(
      Limits(100, 100), Collected.Sink());
  Check(!Controller.RecordProviderOutputLimited(
                Providers::LimitKind::OutputTokens)
             .has_value(),
        "provider output limit must be accepted");
  Check(Controller.GetState().OutputLimitReached &&
            !Controller.CanDispatchSampling(),
        "provider output limit must block sampling");
  Check(Collected.Events.size() == 1U &&
            Collected.Events.back().Code ==
                Agent::OutputBudgetEventCode::ProviderOutputLimited,
        "provider output limit must emit one structured event");
  Check(!Controller.RecordProviderOutputLimited(
                Providers::LimitKind::OutputTokens)
             .has_value(),
        "repeated provider output limit must be idempotent");
  Check(Collected.Events.size() == 1U,
        "repeated provider output limit must not emit again");
  Check(!Controller.RecordProviderOutputLimited(
                Providers::LimitKind::ContextWindow)
             .has_value(),
        "provider context limit must be accepted");
  Check(Controller.GetState().ContextWindowLimitReached &&
            Collected.Events.size() == 2U,
        "provider context limit must emit once");
}

void MultiRoundUsageAccumulatesAndMergesCompleteness() {
  EventCollector Collected;
  Agent::OutputBudgetController Controller({}, Collected.Sink());
  Check(!Controller.Accumulate(
                Usage(10, 20, std::nullopt,
                      Providers::UsageCompleteness::Final))
             .has_value(),
        "first round must accumulate");
  Check(!Controller.Accumulate(
                Usage(5, 10, std::nullopt,
                      Providers::UsageCompleteness::Partial))
             .has_value(),
        "second round must accumulate");
  Check(!Controller.Accumulate(
                Usage(5, 5, std::nullopt,
                      Providers::UsageCompleteness::Unavailable))
             .has_value(),
        "third round must accumulate");
  const auto& State = Controller.GetState();
  Check(State.InputTokensUsed == 20U &&
            State.OutputTokensUsed == 35U &&
            State.TotalTokensUsed == 55U &&
            Controller.AccumulatedSamples() == 3U,
        "multi-round usage must accumulate");
  Check(State.Completeness == Providers::UsageCompleteness::Unavailable,
        "completeness must keep the least complete sample");
  Check(Collected.Events.size() == 3U,
        "each round must emit a usage event");
}

void MissingFieldsKeepBudgetsNullable() {
  Agent::OutputBudgetController Controller(Limits(100));
  Check(!Controller.Accumulate(
                Usage(10, std::nullopt, std::nullopt,
                      Providers::UsageCompleteness::Partial))
             .has_value(),
        "input-only usage must accumulate");
  Check(Controller.GetState().OutputTokensUsed == 0U &&
            Controller.GetState().RemainingOutputTokens == 100U &&
            Controller.GetState().Completeness ==
                Providers::UsageCompleteness::Partial,
        "missing output must not be treated as a known zero total");
  Check(!Controller.Accumulate(
                Usage(std::nullopt, 20, std::nullopt,
                      Providers::UsageCompleteness::Final))
             .has_value(),
        "output-only usage must accumulate");
  Check(Controller.GetState().RemainingOutputTokens == 80U &&
            Controller.GetState().Completeness ==
                Providers::UsageCompleteness::Partial,
        "remaining output must update from known samples");
}

void SinklessControllerStillTracksEvents() {
  Agent::OutputBudgetController Controller(Limits(10));
  Check(!Controller.Accumulate(Usage(1, 10)).has_value(),
        "sinkless accumulation must succeed");
  Check(Controller.EventCount() == 2U &&
            Controller.GetState().OutputLimitReached,
        "sinkless controller must still count usage and limit events");
}

} // namespace

int main() {
  AccumulatesWithoutLimits();
  OutputLimitTransitionsBlockSampling();
  ContextWindowTransitionsBlockSampling();
  SaturatesInsteadOfOverflowing();
  RejectsInconsistentTotalsWithoutMutation();
  ProviderOutputLimitedLinksToBudget();
  MultiRoundUsageAccumulatesAndMergesCompleteness();
  MissingFieldsKeepBudgetsNullable();
  SinklessControllerStillTracksEvents();
  std::cout << "OutputBudgetController contract tests passed\n";
  return 0;
}
