#include "Source/Providers/AnthropicMessages/AnthropicUsageNormalizer.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>

namespace EmulativeDialog::Providers::AnthropicMessages {
namespace {

bool HasNegative(
    std::initializer_list<const std::optional<std::int64_t>*> Values) {
  for (const auto* Value : Values) {
    if (Value->has_value() && **Value < 0) {
      return true;
    }
  }
  return false;
}

std::optional<std::uint64_t> ToUnsigned(
    const std::optional<std::int64_t>& Value) {
  if (!Value.has_value()) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*Value);
}

} // namespace

UsageNormalizationResult AnthropicUsageNormalizer::Normalize(
    const AnthropicWireUsage& Usage) noexcept {
  if (HasNegative({&Usage.InputTokens, &Usage.OutputTokens,
                   &Usage.CacheCreationInputTokens,
                   &Usage.CacheReadInputTokens})) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::NegativeValue};
  }

  return Providers::UsageSnapshot{
      .InputTokens = ToUnsigned(Usage.InputTokens),
      .OutputTokens = ToUnsigned(Usage.OutputTokens),
      .TotalTokens = std::nullopt,
      .CachedInputReadTokens = ToUnsigned(Usage.CacheReadInputTokens),
      .CachedInputWriteTokens = ToUnsigned(Usage.CacheCreationInputTokens),
      .ReasoningOutputTokens = std::nullopt,
      .Completeness = Usage.Completeness,
  };
}

} // namespace EmulativeDialog::Providers::AnthropicMessages
