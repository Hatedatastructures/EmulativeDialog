#include "Source/Providers/OpenAIChatCompletions/ChatUsageNormalizer.hpp"

namespace EmulativeDialog::Providers::OpenAIChatCompletions {

UsageSnapshot NormalizeChatUsage(
    const std::optional<ChatUsageFields>& Fields,
    UsageCompleteness Completeness) noexcept {
  if (!Fields.has_value() || Completeness == UsageCompleteness::Unavailable) {
    return UsageSnapshot{};
  }
  return UsageSnapshot{
      .InputTokens = Fields->PromptTokens,
      .OutputTokens = Fields->CompletionTokens,
      .TotalTokens = Fields->TotalTokens,
      .CachedInputReadTokens = Fields->CachedPromptTokens,
      .CachedInputWriteTokens = std::nullopt,
      .ReasoningOutputTokens = Fields->ReasoningTokens,
      .Completeness = Completeness,
  };
}

} // namespace EmulativeDialog::Providers::OpenAIChatCompletions
