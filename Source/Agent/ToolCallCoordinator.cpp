#include "Source/Agent/ToolCallCoordinator.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace EmulativeDialog::Agent {
namespace {

bool IsTerminalLifecycleEvent(const AgentLifecycleEvent& Event) noexcept {
  return std::holds_alternative<AgentFinalEvent>(Event) ||
         std::holds_alternative<AgentRefusalEvent>(Event) ||
         std::holds_alternative<AgentToolHandoffEvent>(Event) ||
         std::holds_alternative<AgentOutputLimitedEvent>(Event) ||
         std::holds_alternative<AgentFailedEvent>(Event) ||
         std::holds_alternative<AgentCancelledEvent>(Event) ||
         std::holds_alternative<AgentOutcomeUnknownEvent>(Event);
}

} // namespace

ToolCallCoordinator::ToolCallCoordinator(
    const Core::BusinessActionRegistry& RegistryValue,
    ToolCallAuthorizationContext ContextValue, ReceiptSink SinkValue)
    : Registry(RegistryValue),
      Context(std::move(ContextValue)),
      Sink(std::move(SinkValue)) {}

std::optional<ToolCoordinatorError> ToolCallCoordinator::Accept(
    const AgentLifecycleEvent& Event) {
  if (TerminalSeen) {
    return ToolCoordinatorError{
        .Code = ToolCoordinatorErrorCode::EventAfterTerminal};
  }

  if (const auto* Ready = std::get_if<AgentToolCallReadyEvent>(&Event)) {
    if (Ready->Metadata.Turn.Value != Context.ActiveTurn.Value) {
      Emit(*Ready, ToolCallReceiptStatus::Rejected, "TurnMismatch");
      return std::nullopt;
    }
    if (Context.AuthorizedRequest.Value.empty() ||
        Ready->Metadata.Request.Value != Context.AuthorizedRequest.Value) {
      Emit(*Ready, ToolCallReceiptStatus::Rejected, "RequestMismatch");
      return std::nullopt;
    }
    Pending.push_back(*Ready);
    return std::nullopt;
  }

  if (const auto* Handoff = std::get_if<AgentToolHandoffEvent>(&Event)) {
    if (Handoff->Metadata.Turn.Value != Context.ActiveTurn.Value) {
      return ToolCoordinatorError{
          .Code = ToolCoordinatorErrorCode::ForeignHandoff};
    }
    if (Context.AuthorizedRequest.Value.empty() ||
        Handoff->Metadata.Request.Value.empty() ||
        Handoff->Metadata.Request.Value !=
            Context.AuthorizedRequest.Value) {
      return ToolCoordinatorError{
          .Code = ToolCoordinatorErrorCode::RequestMismatch};
    }
    TerminalSeen = true;
    ProcessPending();
    return std::nullopt;
  }

  if (IsTerminalLifecycleEvent(Event)) {
    TerminalSeen = true;
    DiscardPending();
  }
  return std::nullopt;
}

void ToolCallCoordinator::ProcessPending() {
  std::vector<AgentToolCallReadyEvent> ReadyCalls = std::move(Pending);
  Pending.clear();
  for (const AgentToolCallReadyEvent& Call : ReadyCalls) {
    HandleToolCall(Call);
  }
}

void ToolCallCoordinator::DiscardPending() noexcept { Pending.clear(); }

void ToolCallCoordinator::HandleToolCall(
    const AgentToolCallReadyEvent& Call) {
  if (SeenToolCallIds.contains(Call.Call.Value)) {
    Emit(Call, ToolCallReceiptStatus::Rejected, "ReplayRejected");
    return;
  }

  const Core::BusinessActionDefinition* Definition =
      Registry.Find(Call.ToolName);
  if (Definition == nullptr) {
    Emit(Call, ToolCallReceiptStatus::Rejected, "UnknownAction");
    return;
  }
  if (!Core::BusinessActionRegistry::ValidateArguments(
          Call.Arguments.JsonText, Definition->RequiredArgumentFields)) {
    Emit(Call, ToolCallReceiptStatus::Rejected, "InvalidArguments");
    return;
  }
  const bool HasCapabilities = std::all_of(
      Definition->RequiredCapabilities.begin(),
      Definition->RequiredCapabilities.end(),
      [this](const std::string& Capability) {
        return HasCapability(Capability);
      });
  if (!HasCapabilities) {
    Emit(Call, ToolCallReceiptStatus::Rejected, "MissingCapability");
    return;
  }
  if (!ScopeMatches(Definition->Scope)) {
    Emit(Call, ToolCallReceiptStatus::Rejected, "ScopeMismatch");
    return;
  }

  // Mark before dispatch so reentrant or duplicate deliveries can never run
  // the business action twice.
  SeenToolCallIds.insert(Call.Call.Value);
  ++HandlerExecutions;

  Core::BusinessActionInvocation Invocation{
      .Name = Call.ToolName,
      .ToolCallId = Call.Call.Value,
      .Arguments = Call.Arguments,
      .Principal = Context.Principal,
      .Scope = Context.Scope,
      .Turn = Context.ActiveTurn,
  };

  try {
    const Core::BusinessActionExecutionResult Result =
        Definition->Handler(Invocation);
    switch (Result.Status) {
    case Core::BusinessActionExecutionStatus::Succeeded:
      Emit(Call, ToolCallReceiptStatus::Succeeded,
           Result.SanitizedCode.empty() ? "Succeeded"
                                        : Result.SanitizedCode);
      return;
    case Core::BusinessActionExecutionStatus::Failed:
      Emit(Call, ToolCallReceiptStatus::Failed,
           Result.SanitizedCode.empty() ? "ActionFailed"
                                        : Result.SanitizedCode);
      return;
    case Core::BusinessActionExecutionStatus::UnknownOutcome:
      Emit(Call, ToolCallReceiptStatus::UnknownOutcome,
           Result.SanitizedCode.empty() ? "UnknownOutcome"
                                        : Result.SanitizedCode);
      return;
    }
  } catch (...) {
    // Dispatch has already occurred, so an exception cannot prove that the
    // external action had no effect. Preserve the uncertain side effect and
    // prohibit callers from treating this as a retryable, definite failure.
    Emit(Call, ToolCallReceiptStatus::UnknownOutcome, "HandlerException");
    return;
  }
}

void ToolCallCoordinator::Emit(const AgentToolCallReadyEvent& Call,
                               ToolCallReceiptStatus Status,
                               std::string ResultCode) {
  ToolCallReceipt Receipt{
      .ReceiptId = "tool-receipt-" + std::to_string(ReceiptSequence),
      .ToolCallId = Call.Call.Value,
      .ActionName = Call.ToolName,
      .Turn = Context.ActiveTurn,
      .Status = Status,
      .ResultCode = std::move(ResultCode),
  };
  ++ReceiptSequence;
  ++ReceiptsEmitted;
  if (Sink) {
    Sink(Receipt);
  }
}

bool ToolCallCoordinator::HasCapability(
    std::string_view Capability) const noexcept {
  return std::any_of(
      Context.GrantedCapabilities.begin(),
      Context.GrantedCapabilities.end(),
      [Capability](const std::string& Granted) {
        return Granted == Capability;
      });
}

bool ToolCallCoordinator::ScopeMatches(
    const Core::BusinessActionScopeRequirement& Requirement) const noexcept {
  if (Context.Scope.Principal.Value != Context.Principal.Value) {
    return false;
  }
  if (Requirement.ChannelId.has_value() &&
      Context.Scope.Channel.Value != *Requirement.ChannelId) {
    return false;
  }
  if (Requirement.ConversationId.has_value() &&
      Context.Scope.Conversation.Value != *Requirement.ConversationId) {
    return false;
  }
  return true;
}

} // namespace EmulativeDialog::Agent
