#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Providers::OpenAIChatCompletions {

// Decoded Chat Completions usage object. Missing fields stay missing.
struct ChatWireUsage final {
  std::optional<std::int64_t> PromptTokens;
  std::optional<std::int64_t> CompletionTokens;
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

class ChatUsageNormalizer final {
public:
  [[nodiscard]] static UsageNormalizationResult Normalize(
      const ChatWireUsage& Usage) noexcept;
};

} // namespace EmulativeDialog::Providers::OpenAIChatCompletions
