#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Agent {

struct OutputBudgetLimits final {
  std::optional<std::uint64_t> MaximumOutputTokens;
  std::optional<std::uint64_t> ContextWindowTokens;
};

struct OutputBudgetState final {
  std::uint64_t InputTokensUsed{0};
  std::uint64_t OutputTokensUsed{0};
  std::uint64_t TotalTokensUsed{0};
  std::optional<std::uint64_t> RemainingOutputTokens;
  std::optional<std::uint64_t> RemainingContextTokens;
  bool OutputLimitReached{false};
  bool ContextWindowLimitReached{false};
  bool CanContinueSampling{true};
  Providers::UsageCompleteness Completeness{
      Providers::UsageCompleteness::Unavailable};
};

enum class OutputBudgetEventCode : std::uint8_t {
  UsageAccumulated,
  OutputLimitReached,
  ContextWindowLimitReached,
  ProviderOutputLimited,
};

struct OutputBudgetEvent final {
  OutputBudgetEventCode Code{OutputBudgetEventCode::UsageAccumulated};
  OutputBudgetState State;
};

enum class OutputBudgetErrorCode : std::uint8_t {
  InconsistentUsage,
};

struct OutputBudgetError final {
  OutputBudgetErrorCode Code{OutputBudgetErrorCode::InconsistentUsage};

  friend bool operator==(const OutputBudgetError&,
                         const OutputBudgetError&) = default;
};

// Accumulates per-sample canonical usage against Core-approved profile limits.
// Arithmetic saturates instead of wrapping; an exhausted budget never reports
// that another sampling request may be dispatched. All mutations emit at most
// one structured event per accumulation plus one event per limit transition.
class OutputBudgetController final {
public:
  using EventSink = std::function<void(const OutputBudgetEvent&)>;

  explicit OutputBudgetController(OutputBudgetLimits Limits = {},
                                  EventSink Sink = {});

  // Adds the usage of one sampling request. Each call must carry only the new
  // usage for that sample, never a cumulative provider snapshot already added.
  [[nodiscard]] std::optional<OutputBudgetError> Accumulate(
      const Providers::UsageSnapshot& Usage);

  // Links a provider ResponseOutputLimited terminal into the local budget so
  // later sampling can never be dispatched without remaining budget.
  [[nodiscard]] std::optional<OutputBudgetError> RecordProviderOutputLimited(
      Providers::LimitKind Kind);

  [[nodiscard]] const OutputBudgetState& GetState() const noexcept {
    return State;
  }
  [[nodiscard]] bool CanDispatchSampling() const noexcept {
    return State.CanContinueSampling;
  }
  [[nodiscard]] std::size_t EventCount() const noexcept { return Events; }
  [[nodiscard]] std::size_t AccumulatedSamples() const noexcept {
    return Samples;
  }

private:
  void UpdateDerivedState();
  void Emit(OutputBudgetEventCode Code);

  OutputBudgetLimits Limits;
  EventSink Sink;
  OutputBudgetState State;
  std::size_t Events{0};
  std::size_t Samples{0};
  bool HasUsage{false};
};

} // namespace EmulativeDialog::Agent
