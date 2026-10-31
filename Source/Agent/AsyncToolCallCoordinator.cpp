#include "Source/Agent/AsyncToolCallCoordinator.hpp"

#include <limits>
#include <type_traits>
#include <utility>

namespace EmulativeDialog::Agent {

AsyncToolCallCoordinator::AsyncToolCallCoordinator(
    Providers::RequestId RequestValue, Providers::AgentTurnId TurnValue)
    : Request(std::move(RequestValue)), Turn(std::move(TurnValue)) {}

bool AsyncToolCallCoordinator::Accept(
    const Providers::CanonicalNonTerminalEvent& Event) {
  if (TerminalSeen || Invalid) {
    Invalid = true;
    return false;
  }
  return std::visit(
      [this](const auto& Value) {
        if (!AcceptMetadata(Value.Metadata)) {
          Invalid = true;
          return false;
        }
        using EventType = std::decay_t<decltype(Value)>;
        if constexpr (std::is_same_v<EventType,
                                     Providers::ToolCallReadyEvent>) {
          if (Value.Call.Value.empty() || Value.ToolName.empty() ||
              !SeenCallIds.emplace(Value.Call.Value).second) {
            Invalid = true;
            return false;
          }
          Pending.push_back(Providers::FunctionCall{
              .Id = Value.Call,
              .Name = Value.ToolName,
              .Arguments = Value.Arguments,
          });
        }
        return true;
      },
      Event);
}

AsyncToolHandoffDecision AsyncToolCallCoordinator::Complete(
    const Providers::CanonicalTerminalEvent& Terminal,
    bool OutputBudgetAllowsDispatch) {
  if (TerminalSeen || Invalid) {
    Invalid = true;
    Pending.clear();
    return AsyncToolHandoffDecision::Invalid;
  }
  TerminalSeen = true;
  const bool MetadataValid = std::visit(
      [this](const auto& Value) {
        return AcceptMetadata(Value.Metadata);
      },
      Terminal);
  if (!MetadataValid) {
    Invalid = true;
    Pending.clear();
    return AsyncToolHandoffDecision::Invalid;
  }

  const auto* Completed =
      std::get_if<Providers::ResponseCompletedEvent>(&Terminal);
  if (Completed == nullptr ||
      Completed->Kind != Providers::CompletionKind::ToolHandoff) {
    Pending.clear();
    return AsyncToolHandoffDecision::ContinueWithoutDispatch;
  }
  if (!OutputBudgetAllowsDispatch) {
    Pending.clear();
    return AsyncToolHandoffDecision::BudgetRejected;
  }
  if (Pending.empty()) {
    return AsyncToolHandoffDecision::Invalid;
  }
  return AsyncToolHandoffDecision::Dispatch;
}

std::vector<Providers::FunctionCall>
AsyncToolCallCoordinator::TakePending() {
  if (!TerminalSeen || Invalid) {
    return {};
  }
  return std::exchange(Pending, {});
}

bool AsyncToolCallCoordinator::AcceptMetadata(
    const Providers::ProviderEventMetadata& Metadata) noexcept {
  if (Request.Value.empty() || Turn.Value.empty() ||
      Metadata.Request.Value != Request.Value ||
      Metadata.TurnId.Value != Turn.Value ||
      Metadata.SequenceNumber != NextSequence ||
      NextSequence == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  ++NextSequence;
  return true;
}

} // namespace EmulativeDialog::Agent
