#include "Source/Providers/OpenAIResponses/ResponsesUsageNormalizer.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>

namespace EmulativeDialog::Providers::OpenAIResponses {
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

bool TotalIsConsistent(const ResponsesWireUsage& Usage) {
  if (!Usage.InputTokens.has_value() || !Usage.OutputTokens.has_value() ||
      !Usage.TotalTokens.has_value()) {
    return true;
  }
  const auto Input = static_cast<std::uint64_t>(*Usage.InputTokens);
  const auto Output = static_cast<std::uint64_t>(*Usage.OutputTokens);
  if (Input >
      std::numeric_limits<std::uint64_t>::max() - Output) {
    return false;
  }
  return static_cast<std::uint64_t>(*Usage.TotalTokens) == Input + Output;
}

} // namespace

UsageNormalizationResult ResponsesUsageNormalizer::Normalize(
    const ResponsesWireUsage& Usage) noexcept {
  if (HasNegative({&Usage.InputTokens, &Usage.OutputTokens,
                   &Usage.TotalTokens, &Usage.CachedInputTokens,
                   &Usage.ReasoningOutputTokens})) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::NegativeValue};
  }
  if (Usage.InputTokens.has_value() &&
      Usage.CachedInputTokens.has_value() &&
      *Usage.CachedInputTokens > *Usage.InputTokens) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::CachedExceedsInput};
  }
  if (Usage.OutputTokens.has_value() &&
      Usage.ReasoningOutputTokens.has_value() &&
      *Usage.ReasoningOutputTokens > *Usage.OutputTokens) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::ReasoningExceedsOutput};
  }
  if (!TotalIsConsistent(Usage)) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::InconsistentTotal};
  }

  return Providers::UsageSnapshot{
      .InputTokens = ToUnsigned(Usage.InputTokens),
      .OutputTokens = ToUnsigned(Usage.OutputTokens),
      .TotalTokens = ToUnsigned(Usage.TotalTokens),
      .CachedInputReadTokens = ToUnsigned(Usage.CachedInputTokens),
      .CachedInputWriteTokens = std::nullopt,
      .ReasoningOutputTokens = ToUnsigned(Usage.ReasoningOutputTokens),
      .Completeness = Usage.Completeness,
  };
}

} // namespace EmulativeDialog::Providers::OpenAIResponses
