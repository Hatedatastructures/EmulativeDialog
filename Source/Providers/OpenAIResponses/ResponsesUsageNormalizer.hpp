#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Providers::OpenAIResponses {

// Decoded OpenAI Responses usage object. Missing fields stay missing; a wire
// null is represented as std::nullopt rather than zero.
struct ResponsesWireUsage final {
  std::optional<std::int64_t> InputTokens;
  std::optional<std::int64_t> OutputTokens;
  std::optional<std::int64_t> TotalTokens;
  std::optional<std::int64_t> CachedInputTokens;
  std::optional<std::int64_t> ReasoningOutputTokens;
  UsageCompleteness Completeness{UsageCompleteness::Partial};
};

enum class UsageNormalizationErrorCode : std::uint8_t {
  NegativeValue,
  InconsistentTotal,
  CachedExceedsInput,
  ReasoningExceedsOutput,
};

struct UsageNormalizationError final {
  UsageNormalizationErrorCode Code{UsageNormalizationErrorCode::NegativeValue};

  friend bool operator==(const UsageNormalizationError&,
                         const UsageNormalizationError&) = default;
};

using UsageNormalizationResult =
    std::variant<Providers::UsageSnapshot, UsageNormalizationError>;

class ResponsesUsageNormalizer final {
public:
  [[nodiscard]] static UsageNormalizationResult Normalize(
      const ResponsesWireUsage& Usage) noexcept;
};

} // namespace EmulativeDialog::Providers::OpenAIResponses
