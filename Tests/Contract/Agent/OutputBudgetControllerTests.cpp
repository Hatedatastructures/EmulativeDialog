#include "Source/Agent/OutputBudgetController.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << "FAIL: " << Message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

using EmulativeDialog::Agent::OutputBudgetController;
using EmulativeDialog::Agent::OutputBudgetRequestResult;
using EmulativeDialog::Providers::AgentTurnId;
using EmulativeDialog::Providers::RequestId;
using EmulativeDialog::Providers::UsageCompleteness;
using EmulativeDialog::Providers::UsageSnapshot;

UsageSnapshot MakeUsage(std::optional<std::uint64_t> OutputTokens,
                        UsageCompleteness Completeness,
                        std::optional<std::uint64_t> InputTokens =
                            std::nullopt) {
  return UsageSnapshot{
      .InputTokens = InputTokens,
      .OutputTokens = OutputTokens,
      .TotalTokens = std::nullopt,
      .CachedInputReadTokens = std::nullopt,
      .CachedInputWriteTokens = std::nullopt,
      .ReasoningOutputTokens = std::nullopt,
      .Completeness = Completeness,
  };
}

void TestMissingAndUnavailableUsageFailClosed() {
  auto& Ledger = OutputBudgetController::Shared();
  const AgentTurnId Turn{"budget-missing-usage"};
  const RequestId Request{"budget-missing-request"};
  Check(Ledger.BeginTurn(Turn, 100), "new turn should be registered");
  Check(Ledger.BeginRequest(Turn, Request) ==
            OutputBudgetRequestResult::Accepted,
        "the first request should be admitted");
  Check(Ledger.BeginRequest(Turn, Request) ==
            OutputBudgetRequestResult::RequestAlreadyStarted,
        "a second provider request must not self-authorize continuation");
  Check(Ledger.CompleteRequest(Turn, Request, std::nullopt, false, false),
        "request without any usage event should still be finalized");
  const auto Missing = Ledger.Inspect(Turn);
  Check(Missing.has_value() &&
            Missing->UsageCompleteness == UsageCompleteness::Unavailable &&
            !Missing->UsageObserved && !Missing->OutputTokens,
        "missing usage must remain unavailable and unknown");
  Check(!Ledger.CanDispatchTool(Turn, Request),
        "missing usage cannot authorize tool dispatch");
  Check(!Ledger.CanContinue(Turn),
        "an absent Core continuation grant must always deny continuation");
  Ledger.EndTurn(Turn);
  Check(!Ledger.HasActiveTurn(Turn), "ended turn must be removed");

  const AgentTurnId UnavailableTurn{"budget-unavailable-usage"};
  const RequestId UnavailableRequest{"budget-unavailable-request"};
  Check(Ledger.BeginTurn(UnavailableTurn, std::nullopt),
        "turn without a configured maximum should still be tracked");
  Check(Ledger.BeginRequest(UnavailableTurn, UnavailableRequest) ==
            OutputBudgetRequestResult::Accepted,
        "first request should be admitted without a configured maximum");
  Check(Ledger.ObserveUsage(
            UnavailableTurn, UnavailableRequest,
            MakeUsage(8, UsageCompleteness::Unavailable, 3)),
        "unavailable observation should be recorded, not trusted");
  Check(Ledger.CompleteRequest(
            UnavailableTurn, UnavailableRequest,
            MakeUsage(8, UsageCompleteness::Unavailable),
            false, false),
        "unavailable terminal usage should close the request");
  const auto Unavailable = Ledger.Inspect(UnavailableTurn);
  Check(Unavailable.has_value() && Unavailable->UsageObserved &&
            Unavailable->UsageCompleteness == UsageCompleteness::Unavailable &&
            !Unavailable->OutputTokens,
        "Unavailable usage values must be discarded instead of reused");
  Check(!Ledger.CanDispatchTool(UnavailableTurn, UnavailableRequest),
        "unavailable usage cannot authorize a tool call");
  Ledger.EndTurn(UnavailableTurn);
}

void TestFinalUsageAllowsOnlyInBudgetCompletedHandoff() {
  auto& Ledger = OutputBudgetController::Shared();
  const AgentTurnId Turn{"budget-final-usage"};
  const RequestId Request{"budget-final-request"};
  Check(Ledger.BeginTurn(Turn, 100), "turn should begin");
  Check(Ledger.BeginRequest(Turn, Request) ==
            OutputBudgetRequestResult::Accepted,
        "first request should begin");
  Check(Ledger.ObserveUsage(
            Turn, Request,
            MakeUsage(12, UsageCompleteness::Partial)),
        "partial usage should be accepted as a snapshot");
  Check(!Ledger.CanDispatchTool(Turn, Request),
        "partial usage cannot authorize tool dispatch");
  Check(Ledger.CompleteRequest(
            Turn, Request,
            MakeUsage(30, UsageCompleteness::Final),
            false, true),
        "final usage should close the request");
  const auto Final = Ledger.Inspect(Turn);
  Check(Final.has_value() && Final->UsageCompleteness == UsageCompleteness::Final &&
            Final->OutputTokens == 30 && !Final->OutputBudgetExhausted,
        "final snapshot replaces partial snapshot without summing");
  Check(Ledger.CanDispatchTool(Turn, Request),
        "known final usage below the configured budget can pass the budget gate");
  Check(!Ledger.CanContinue(Turn),
        "known usage still cannot create an absent Core continuation grant");
  Ledger.EndTurn(Turn);
}

void TestExhaustedAndOutputLimitedRequestsFailClosed() {
  auto& Ledger = OutputBudgetController::Shared();
  const AgentTurnId ExhaustedTurn{"budget-exhausted"};
  const RequestId ExhaustedRequest{"budget-exhausted-request"};
  Check(Ledger.BeginTurn(ExhaustedTurn, 20), "turn should begin");
  Check(Ledger.BeginRequest(ExhaustedTurn, ExhaustedRequest) ==
            OutputBudgetRequestResult::Accepted,
        "first request should begin");
  Check(Ledger.CompleteRequest(
            ExhaustedTurn, ExhaustedRequest,
            MakeUsage(20, UsageCompleteness::Final),
            false, true),
        "exactly exhausted output should be captured");
  const auto Exhausted = Ledger.Inspect(ExhaustedTurn);
  Check(Exhausted.has_value() && Exhausted->OutputBudgetExhausted,
        "reaching the configured output limit exhausts remaining budget");
  Check(!Ledger.CanDispatchTool(ExhaustedTurn, ExhaustedRequest),
        "exhausted output budget cannot authorize tool dispatch");
  Ledger.EndTurn(ExhaustedTurn);

  const AgentTurnId LimitedTurn{"budget-output-limited"};
  const RequestId LimitedRequest{"budget-output-limited-request"};
  Check(Ledger.BeginTurn(LimitedTurn, 40), "turn should begin");
  Check(Ledger.BeginRequest(LimitedTurn, LimitedRequest) ==
            OutputBudgetRequestResult::Accepted,
        "first request should begin");
  Check(Ledger.CompleteRequest(
            LimitedTurn, LimitedRequest,
            MakeUsage(40, UsageCompleteness::Partial),
            true, false),
        "provider output-limited terminal should close its request");
  const auto Limited = Ledger.Inspect(LimitedTurn);
  Check(Limited.has_value() && Limited->OutputLimited,
        "output-limited terminal must be recorded");
  Check(!Ledger.CanDispatchTool(LimitedTurn, LimitedRequest),
        "output-limited response cannot authorize tool dispatch");
  Ledger.EndTurn(LimitedTurn);
}

void TestInvalidAndUnknownRequestPathsFailClosed() {
  auto& Ledger = OutputBudgetController::Shared();
  const AgentTurnId UnknownTurn{"budget-unknown-turn"};
  const RequestId UnknownRequest{"budget-unknown-request"};
  Check(Ledger.BeginRequest(UnknownTurn, UnknownRequest) ==
            OutputBudgetRequestResult::UnknownTurn,
        "request for an unregistered turn must be rejected");
  Check(Ledger.BeginRequest(AgentTurnId{}, UnknownRequest) ==
            OutputBudgetRequestResult::InvalidIdentifier,
        "empty turn id must be rejected");
  const AgentTurnId Turn{"budget-request-mismatch"};
  const RequestId Request{"budget-request-id"};
  const RequestId OtherRequest{"budget-other-request"};
  Check(!Ledger.BeginTurn(AgentTurnId{}, 10), "empty turn id must not register");
  Check(Ledger.BeginTurn(Turn, 10), "valid turn should register");
  Check(Ledger.BeginRequest(Turn, Request) ==
            OutputBudgetRequestResult::Accepted,
        "valid request should begin");
  Check(!Ledger.ObserveUsage(
            Turn, OtherRequest,
            MakeUsage(1, UsageCompleteness::Final)),
        "usage from a different request must be rejected");
  Check(!Ledger.CompleteRequest(Turn, OtherRequest, std::nullopt, false, false),
        "terminal from a different request must be rejected");
  Check(!Ledger.CanDispatchTool(Turn, OtherRequest),
        "tool authorization must be bound to exact request id");
  Check(!Ledger.BeginTurn(Turn, 10), "duplicate active turn id must fail closed");
  Ledger.EndTurn(Turn);
  Check(!Ledger.Inspect(Turn), "EndTurn must erase all state");
}

} // namespace

int main() {
  TestMissingAndUnavailableUsageFailClosed();
  TestFinalUsageAllowsOnlyInBudgetCompletedHandoff();
  TestExhaustedAndOutputLimitedRequestsFailClosed();
  TestInvalidAndUnknownRequestPathsFailClosed();
  std::cout << "Output budget controller contract tests passed\n";
  return EXIT_SUCCESS;
}
