#pragma once

#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <cstdint>
#include <optional>

namespace EmulativeDialog::Providers::OpenAIResponses {

struct ResponsesUsageFields final {
  std::optional<std::uint64_t> InputTokens;
  std::optional<std::uint64_t> OutputTokens;
  std::optional<std::uint64_t> TotalTokens;
  std::optional<std::uint64_t> CachedInputReadTokens;
  std::optional<std::uint64_t> ReasoningOutputTokens;
};

[[nodiscard]] UsageSnapshot NormalizeResponsesUsage(
    const std::optional<ResponsesUsageFields>& Fields,
    UsageCompleteness Completeness) noexcept;

} // namespace EmulativeDialog::Providers::OpenAIResponses
