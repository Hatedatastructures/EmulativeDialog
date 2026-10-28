#include "Source/Providers/OpenAIChatCompletions/ChatUsageNormalizer.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>

namespace EmulativeDialog::Providers::OpenAIChatCompletions {
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

bool TotalIsConsistent(const ChatWireUsage& Usage) {
  if (!Usage.PromptTokens.has_value() ||
      !Usage.CompletionTokens.has_value() ||
      !Usage.TotalTokens.has_value()) {
    return true;
  }
  const auto Input = static_cast<std::uint64_t>(*Usage.PromptTokens);
  const auto Output = static_cast<std::uint64_t>(*Usage.CompletionTokens);
  if (Input >
      std::numeric_limits<std::uint64_t>::max() - Output) {
    return false;
  }
  return static_cast<std::uint64_t>(*Usage.TotalTokens) == Input + Output;
}

} // namespace

UsageNormalizationResult ChatUsageNormalizer::Normalize(
    const ChatWireUsage& Usage) noexcept {
  if (HasNegative({&Usage.PromptTokens, &Usage.CompletionTokens,
                   &Usage.TotalTokens, &Usage.CachedInputTokens,
                   &Usage.ReasoningOutputTokens})) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::NegativeValue};
  }
  if (Usage.PromptTokens.has_value() &&
      Usage.CachedInputTokens.has_value() &&
      *Usage.CachedInputTokens > *Usage.PromptTokens) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::CachedExceedsInput};
  }
  if (Usage.CompletionTokens.has_value() &&
      Usage.ReasoningOutputTokens.has_value() &&
      *Usage.ReasoningOutputTokens > *Usage.CompletionTokens) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::ReasoningExceedsOutput};
  }
  if (!TotalIsConsistent(Usage)) {
    return UsageNormalizationError{
        UsageNormalizationErrorCode::InconsistentTotal};
  }

  return Providers::UsageSnapshot{
      .InputTokens = ToUnsigned(Usage.PromptTokens),
      .OutputTokens = ToUnsigned(Usage.CompletionTokens),
      .TotalTokens = ToUnsigned(Usage.TotalTokens),
      .CachedInputReadTokens = ToUnsigned(Usage.CachedInputTokens),
      .CachedInputWriteTokens = std::nullopt,
      .ReasoningOutputTokens = ToUnsigned(Usage.ReasoningOutputTokens),
      .Completeness = Usage.Completeness,
  };
}

} // namespace EmulativeDialog::Providers::OpenAIChatCompletions
