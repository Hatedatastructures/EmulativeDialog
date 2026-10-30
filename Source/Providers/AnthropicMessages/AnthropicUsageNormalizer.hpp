#pragma once

#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <cstdint>
#include <optional>

namespace EmulativeDialog::Providers::AnthropicMessages {

struct AnthropicUsageFields final {
  std::optional<std::uint64_t> InputTokens;
  std::optional<std::uint64_t> OutputTokens;
  std::optional<std::uint64_t> CacheReadInputTokens;
  std::optional<std::uint64_t> CacheCreationInputTokens;
};

[[nodiscard]] UsageSnapshot NormalizeAnthropicUsage(
    const std::optional<AnthropicUsageFields>& Fields,
    UsageCompleteness Completeness) noexcept;

} // namespace EmulativeDialog::Providers::AnthropicMessages
