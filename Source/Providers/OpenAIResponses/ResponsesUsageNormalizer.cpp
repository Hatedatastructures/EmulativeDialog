#include "Source/Providers/OpenAIResponses/ResponsesUsageNormalizer.hpp"

namespace EmulativeDialog::Providers::OpenAIResponses {

UsageSnapshot NormalizeResponsesUsage(
    const std::optional<ResponsesUsageFields>& Fields,
    UsageCompleteness Completeness) noexcept {
  if (!Fields.has_value() || Completeness == UsageCompleteness::Unavailable) {
    return UsageSnapshot{};
  }
  return UsageSnapshot{
      .InputTokens = Fields->InputTokens,
      .OutputTokens = Fields->OutputTokens,
      .TotalTokens = Fields->TotalTokens,
      .CachedInputReadTokens = Fields->CachedInputReadTokens,
      .CachedInputWriteTokens = std::nullopt,
      .ReasoningOutputTokens = Fields->ReasoningOutputTokens,
      .Completeness = Completeness,
  };
}

} // namespace EmulativeDialog::Providers::OpenAIResponses
