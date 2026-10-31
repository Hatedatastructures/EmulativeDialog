#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Agent {

enum class AsyncToolHandoffDecision : std::uint8_t {
  ContinueWithoutDispatch,
  BudgetRejected,
  Dispatch,
  Invalid,
};

// Buffers complete canonical call proposals while a request is streaming.
// Calls become visible to Core only after the matching ToolHandoff terminal
// and the caller's authoritative output-budget check.
class AsyncToolCallCoordinator final {
public:
  AsyncToolCallCoordinator(Providers::RequestId Request,
                           Providers::AgentTurnId Turn);

  [[nodiscard]] bool Accept(
      const Providers::CanonicalNonTerminalEvent& Event);

  [[nodiscard]] AsyncToolHandoffDecision Complete(
      const Providers::CanonicalTerminalEvent& Terminal,
      bool OutputBudgetAllowsDispatch);

  [[nodiscard]] std::vector<Providers::FunctionCall> TakePending();
  [[nodiscard]] bool IsInvalid() const noexcept { return Invalid; }
  [[nodiscard]] bool HasToolCalls() const noexcept {
    return !Pending.empty();
  }

private:
  [[nodiscard]] bool AcceptMetadata(
      const Providers::ProviderEventMetadata& Metadata) noexcept;

  Providers::RequestId Request;
  Providers::AgentTurnId Turn;
  std::uint64_t NextSequence{1};
  std::vector<Providers::FunctionCall> Pending;
  std::unordered_set<std::string> SeenCallIds;
  bool Invalid{false};
  bool TerminalSeen{false};
};

} // namespace EmulativeDialog::Agent
