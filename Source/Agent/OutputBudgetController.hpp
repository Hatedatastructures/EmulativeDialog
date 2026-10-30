#pragma once

#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace EmulativeDialog::Agent {

enum class OutputBudgetRequestResult : std::uint8_t {
  Accepted,
  UnknownTurn,
  InvalidIdentifier,
  RequestAlreadyStarted,
};

struct OutputBudgetTurnSnapshot final {
  bool Active{};
  bool RequestStarted{};
  bool RequestCompleted{};
  bool OutputLimited{};
  bool ToolHandoff{};
  bool UsageObserved{};
  bool OutputBudgetExhausted{};
  bool ContinuationAuthorized{};
  std::optional<std::uint32_t> MaximumOutputTokens;
  std::optional<std::uint64_t> OutputTokens;
  Providers::UsageCompleteness UsageCompleteness{
      Providers::UsageCompleteness::Unavailable};
};

// Shared state for one synchronous Core turn. Core owns the turn lifetime;
// AgentRuntime records provider usage while the request is active.
class OutputBudgetController final {
public:
  OutputBudgetController() = default;
  OutputBudgetController(const OutputBudgetController&) = delete;
  OutputBudgetController& operator=(const OutputBudgetController&) = delete;

  [[nodiscard]] static OutputBudgetController& Shared() noexcept;

  [[nodiscard]] bool BeginTurn(
      const Providers::AgentTurnId& Turn,
      std::optional<std::uint32_t> MaximumOutputTokens);
  void EndTurn(const Providers::AgentTurnId& Turn) noexcept;

  [[nodiscard]] OutputBudgetRequestResult BeginRequest(
      const Providers::AgentTurnId& Turn,
      const Providers::RequestId& Request);
  [[nodiscard]] bool ObserveUsage(
      const Providers::AgentTurnId& Turn,
      const Providers::RequestId& Request,
      const Providers::UsageSnapshot& Usage) noexcept;
  [[nodiscard]] bool CompleteRequest(
      const Providers::AgentTurnId& Turn,
      const Providers::RequestId& Request,
      const std::optional<Providers::UsageSnapshot>& FinalUsage,
      bool OutputLimited,
      bool ToolHandoff) noexcept;

  [[nodiscard]] bool CanDispatchTool(
      const Providers::AgentTurnId& Turn,
      const Providers::RequestId& Request) const noexcept;
  [[nodiscard]] bool CanContinue(
      const Providers::AgentTurnId& Turn) const noexcept;
  [[nodiscard]] bool HasActiveTurn(
      const Providers::AgentTurnId& Turn) const noexcept;
  [[nodiscard]] std::optional<OutputBudgetTurnSnapshot> Inspect(
      const Providers::AgentTurnId& Turn) const noexcept;

private:
  struct TurnState final {
    std::optional<std::uint32_t> MaximumOutputTokens;
    std::string ActiveRequest;
    Providers::UsageSnapshot LatestUsage;
    bool RequestStarted{};
    bool RequestCompleted{};
    bool OutputLimited{};
    bool ToolHandoff{};
    bool UsageObserved{};
    bool OutputBudgetExhausted{};
  };

  void ApplyUsage(TurnState& State,
                  const Providers::UsageSnapshot& Usage) noexcept;

  mutable std::mutex Mutex;
  std::unordered_map<std::string, TurnState> Turns;
};

} // namespace EmulativeDialog::Agent
