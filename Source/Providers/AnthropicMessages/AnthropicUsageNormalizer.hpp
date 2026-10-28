#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Providers::AnthropicMessages {

// Decoded Anthropic usage object. Anthropic does not report a total and
// cache_read_input_tokens may legitimately exceed input_tokens.
struct AnthropicWireUsage final {
  std::optional<std::int64_t> InputTokens;
  std::optional<std::int64_t> OutputTokens;
  std::optional<std::int64_t> CacheCreationInputTokens;
  std::optional<std::int64_t> CacheReadInputTokens;
  UsageCompleteness Completeness{UsageCompleteness::Partial};
};

enum class UsageNormalizationErrorCode : std::uint8_t {
  NegativeValue,
};

struct UsageNormalizationError final {
  UsageNormalizationErrorCode Code{UsageNormalizationErrorCode::NegativeValue};

  friend bool operator==(const UsageNormalizationError&,
                         const UsageNormalizationError&) = default;
};

using UsageNormalizationResult =
    std::variant<Providers::UsageSnapshot, UsageNormalizationError>;

class AnthropicUsageNormalizer final {
public:
  [[nodiscard]] static UsageNormalizationResult Normalize(
      const AnthropicWireUsage& Usage) noexcept;
};

} // namespace EmulativeDialog::Providers::AnthropicMessages
