#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Source/Agent/AgentEventProjector.hpp"
#include "Source/Core/AgentRuntimePort.h"
#include "Source/Core/BusinessActionRegistry.hpp"

namespace EmulativeDialog::Agent {

struct ToolCallAuthorizationContext final {
  Core::PrincipalId Principal;
  Core::ConversationScope Scope;
  Providers::AgentTurnId ActiveTurn;
  std::vector<std::string> GrantedCapabilities;
};

enum class ToolCoordinatorErrorCode : std::uint8_t {
  EventAfterTerminal,
};

struct ToolCoordinatorError final {
  ToolCoordinatorErrorCode Code{ToolCoordinatorErrorCode::EventAfterTerminal};

  friend bool operator==(const ToolCoordinatorError&,
                         const ToolCoordinatorError&) = default;
};

enum class ToolCallReceiptStatus : std::uint8_t {
  Succeeded,
  Failed,
  UnknownOutcome,
  Rejected,
};

struct ToolCallReceipt final {
  std::string ReceiptId;
  std::string ToolCallId;
  std::string ActionName;
  Providers::AgentTurnId Turn;
  ToolCallReceiptStatus Status{ToolCallReceiptStatus::Rejected};
  std::string ResultCode;
};

// Core-owned gate for model-proposed business actions. ToolCallReady events are
// buffered until the projector delivers a ToolHandoff terminal; only then are
// registered actions validated, authorized once per ToolCallId, and executed.
// No generic Shell/filesystem/network tool can be dispatched through here.
class ToolCallCoordinator final {
public:
  using ReceiptSink = std::function<void(const ToolCallReceipt&)>;

  ToolCallCoordinator(const Core::BusinessActionRegistry& Registry,
                      ToolCallAuthorizationContext Context,
                      ReceiptSink Sink);

  [[nodiscard]] std::optional<ToolCoordinatorError> Accept(
      const AgentLifecycleEvent& Event);

  [[nodiscard]] bool HasTerminal() const noexcept { return TerminalSeen; }
  [[nodiscard]] std::size_t HandlerExecutionCount() const noexcept {
    return HandlerExecutions;
  }
  [[nodiscard]] std::size_t ReceiptCount() const noexcept {
    return ReceiptsEmitted;
  }

private:
  void ProcessPending();
  void DiscardPending() noexcept;
  void HandleToolCall(const AgentToolCallReadyEvent& Call);
  void Emit(const AgentToolCallReadyEvent& Call,
            ToolCallReceiptStatus Status, std::string ResultCode);
  [[nodiscard]] bool HasCapability(std::string_view Capability) const noexcept;
  [[nodiscard]] bool ScopeMatches(
      const Core::BusinessActionScopeRequirement& Requirement) const noexcept;

  const Core::BusinessActionRegistry& Registry;
  ToolCallAuthorizationContext Context;
  ReceiptSink Sink;
  std::vector<AgentToolCallReadyEvent> Pending;
  std::unordered_set<std::string> SeenToolCallIds;
  std::uint64_t ReceiptSequence{1};
  std::size_t HandlerExecutions{0};
  std::size_t ReceiptsEmitted{0};
  bool TerminalSeen{false};
};

} // namespace EmulativeDialog::Agent
