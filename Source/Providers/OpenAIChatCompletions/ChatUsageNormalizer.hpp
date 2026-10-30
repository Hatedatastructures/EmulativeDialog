#pragma once

#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <cstdint>
#include <optional>

namespace EmulativeDialog::Providers::OpenAIChatCompletions {

struct ChatUsageFields final {
  std::optional<std::uint64_t> PromptTokens;
  std::optional<std::uint64_t> CompletionTokens;
  std::optional<std::uint64_t> TotalTokens;
  std::optional<std::uint64_t> CachedPromptTokens;
  std::optional<std::uint64_t> ReasoningTokens;
};

[[nodiscard]] UsageSnapshot NormalizeChatUsage(
    const std::optional<ChatUsageFields>& Fields,
    UsageCompleteness Completeness) noexcept;

} // namespace EmulativeDialog::Providers::OpenAIChatCompletions
