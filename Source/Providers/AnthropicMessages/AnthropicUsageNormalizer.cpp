#include "Source/Providers/AnthropicMessages/AnthropicUsageNormalizer.hpp"

namespace EmulativeDialog::Providers::AnthropicMessages {

UsageSnapshot NormalizeAnthropicUsage(
    const std::optional<AnthropicUsageFields>& Fields,
    UsageCompleteness Completeness) noexcept {
  if (!Fields.has_value() || Completeness == UsageCompleteness::Unavailable) {
    return UsageSnapshot{};
  }
  return UsageSnapshot{
      .InputTokens = Fields->InputTokens,
      .OutputTokens = Fields->OutputTokens,
      .TotalTokens = std::nullopt,
      .CachedInputReadTokens = Fields->CacheReadInputTokens,
      .CachedInputWriteTokens = Fields->CacheCreationInputTokens,
      .ReasoningOutputTokens = std::nullopt,
      .Completeness = Completeness,
  };
}

} // namespace EmulativeDialog::Providers::AnthropicMessages
