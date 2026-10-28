#include "Source/Providers/AnthropicMessages/AnthropicUsageNormalizer.hpp"
#include "Source/Providers/OpenAIChatCompletions/ChatUsageNormalizer.hpp"
#include "Source/Providers/OpenAIResponses/ResponsesUsageNormalizer.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <variant>

namespace {

using namespace EmulativeDialog;

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

Providers::OpenAIResponses::ResponsesWireUsage ResponsesUsage(
    std::optional<std::int64_t> Input = std::nullopt,
    std::optional<std::int64_t> Output = std::nullopt,
    std::optional<std::int64_t> Total = std::nullopt,
    std::optional<std::int64_t> Cached = std::nullopt,
    std::optional<std::int64_t> Reasoning = std::nullopt,
    Providers::UsageCompleteness Completeness =
        Providers::UsageCompleteness::Partial) {
  return Providers::OpenAIResponses::ResponsesWireUsage{
      .InputTokens = Input,
      .OutputTokens = Output,
      .TotalTokens = Total,
      .CachedInputTokens = Cached,
      .ReasoningOutputTokens = Reasoning,
      .Completeness = Completeness,
  };
}

Providers::OpenAIChatCompletions::ChatWireUsage ChatUsage(
    std::optional<std::int64_t> Prompt = std::nullopt,
    std::optional<std::int64_t> Completion = std::nullopt,
    std::optional<std::int64_t> Total = std::nullopt,
    std::optional<std::int64_t> Cached = std::nullopt,
    std::optional<std::int64_t> Reasoning = std::nullopt,
    Providers::UsageCompleteness Completeness =
        Providers::UsageCompleteness::Partial) {
  return Providers::OpenAIChatCompletions::ChatWireUsage{
      .PromptTokens = Prompt,
      .CompletionTokens = Completion,
      .TotalTokens = Total,
      .CachedInputTokens = Cached,
      .ReasoningOutputTokens = Reasoning,
      .Completeness = Completeness,
  };
}

Providers::AnthropicMessages::AnthropicWireUsage AnthropicUsage(
    std::optional<std::int64_t> Input = std::nullopt,
    std::optional<std::int64_t> Output = std::nullopt,
    std::optional<std::int64_t> CacheCreation = std::nullopt,
    std::optional<std::int64_t> CacheRead = std::nullopt,
    Providers::UsageCompleteness Completeness =
        Providers::UsageCompleteness::Partial) {
  return Providers::AnthropicMessages::AnthropicWireUsage{
      .InputTokens = Input,
      .OutputTokens = Output,
      .CacheCreationInputTokens = CacheCreation,
      .CacheReadInputTokens = CacheRead,
      .Completeness = Completeness,
  };
}

const Providers::UsageSnapshot& Snapshot(
    const Providers::OpenAIResponses::UsageNormalizationResult& Result) {
  const auto* Value = std::get_if<Providers::UsageSnapshot>(&Result);
  Check(Value != nullptr, "expected successful Responses usage");
  return *Value;
}

const Providers::UsageSnapshot& Snapshot(
    const Providers::OpenAIChatCompletions::UsageNormalizationResult& Result) {
  const auto* Value = std::get_if<Providers::UsageSnapshot>(&Result);
  Check(Value != nullptr, "expected successful Chat usage");
  return *Value;
}

const Providers::UsageSnapshot& Snapshot(
    const Providers::AnthropicMessages::UsageNormalizationResult& Result) {
  const auto* Value = std::get_if<Providers::UsageSnapshot>(&Result);
  Check(Value != nullptr, "expected successful Anthropic usage");
  return *Value;
}

void ResponsesMapsFieldsAndPreservesMissing() {
  using Providers::OpenAIResponses::ResponsesUsageNormalizer;
  using Providers::OpenAIResponses::UsageNormalizationErrorCode;

  const auto Full = ResponsesUsageNormalizer::Normalize(ResponsesUsage(
      100, 40, 140, 20, 10, Providers::UsageCompleteness::Final));
  const auto& SnapshotValue = Snapshot(Full);
  Check(SnapshotValue.InputTokens == 100U &&
            SnapshotValue.OutputTokens == 40U &&
            SnapshotValue.TotalTokens == 140U &&
            SnapshotValue.CachedInputReadTokens == 20U &&
            !SnapshotValue.CachedInputWriteTokens.has_value() &&
            SnapshotValue.ReasoningOutputTokens == 10U &&
            SnapshotValue.Completeness == Providers::UsageCompleteness::Final,
        "Responses usage must map canonical fields");

  const auto Missing = ResponsesUsageNormalizer::Normalize(ResponsesUsage(
      7, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      Providers::UsageCompleteness::Partial));
  const auto& MissingValue = Snapshot(Missing);
  Check(MissingValue.InputTokens == 7U &&
            !MissingValue.OutputTokens.has_value() &&
            !MissingValue.TotalTokens.has_value() &&
            !MissingValue.CachedInputReadTokens.has_value() &&
            !MissingValue.ReasoningOutputTokens.has_value(),
        "missing Responses fields must stay missing");

  const auto Unavailable = ResponsesUsageNormalizer::Normalize(ResponsesUsage(
      std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      Providers::UsageCompleteness::Unavailable));
  Check(Snapshot(Unavailable).Completeness ==
            Providers::UsageCompleteness::Unavailable,
        "Responses unavailability must be preserved");

  const auto Negative =
      ResponsesUsageNormalizer::Normalize(ResponsesUsage(-1));
  Check(std::get_if<Providers::OpenAIResponses::UsageNormalizationError>(
            &Negative)
                ->Code == UsageNormalizationErrorCode::NegativeValue,
        "negative Responses usage must fail");

  const auto Cached = ResponsesUsageNormalizer::Normalize(
      ResponsesUsage(5, std::nullopt, std::nullopt, 6));
  Check(std::get_if<Providers::OpenAIResponses::UsageNormalizationError>(
            &Cached)
                ->Code == UsageNormalizationErrorCode::CachedExceedsInput,
        "cached tokens above input must fail");

  const auto Reasoning = ResponsesUsageNormalizer::Normalize(
      ResponsesUsage(std::nullopt, 5, std::nullopt, std::nullopt, 6));
  Check(std::get_if<Providers::OpenAIResponses::UsageNormalizationError>(
            &Reasoning)
                ->Code ==
            UsageNormalizationErrorCode::ReasoningExceedsOutput,
        "reasoning tokens above output must fail");

  const auto Inconsistent =
      ResponsesUsageNormalizer::Normalize(ResponsesUsage(1, 2, 4));
  Check(std::get_if<Providers::OpenAIResponses::UsageNormalizationError>(
            &Inconsistent)
                ->Code == UsageNormalizationErrorCode::InconsistentTotal,
        "inconsistent Responses total must fail");
}

void ChatMapsFieldsAndPreservesMissing() {
  using Providers::OpenAIChatCompletions::ChatUsageNormalizer;
  using Providers::OpenAIChatCompletions::UsageNormalizationErrorCode;

  const auto Full = ChatUsageNormalizer::Normalize(ChatUsage(
      80, 30, 110, 15, 8, Providers::UsageCompleteness::Final));
  const auto& SnapshotValue = Snapshot(Full);
  Check(SnapshotValue.InputTokens == 80U &&
            SnapshotValue.OutputTokens == 30U &&
            SnapshotValue.TotalTokens == 110U &&
            SnapshotValue.CachedInputReadTokens == 15U &&
            SnapshotValue.ReasoningOutputTokens == 8U &&
            !SnapshotValue.CachedInputWriteTokens.has_value(),
        "Chat usage must map canonical fields");

  const auto Missing = ChatUsageNormalizer::Normalize(ChatUsage(
      std::nullopt, 9, std::nullopt, std::nullopt, std::nullopt,
      Providers::UsageCompleteness::Unavailable));
  const auto& MissingValue = Snapshot(Missing);
  Check(!MissingValue.InputTokens.has_value() &&
            MissingValue.OutputTokens == 9U &&
            MissingValue.Completeness ==
                Providers::UsageCompleteness::Unavailable,
        "missing Chat fields must stay missing");

  const auto Partial = ChatUsageNormalizer::Normalize(ChatUsage(
      12, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      Providers::UsageCompleteness::Partial));
  Check(Snapshot(Partial).Completeness ==
            Providers::UsageCompleteness::Partial,
        "Chat partial usage completeness must be preserved");

  const auto Negative = ChatUsageNormalizer::Normalize(ChatUsage(
      std::nullopt, -3));
  Check(std::get_if<Providers::OpenAIChatCompletions::
                        UsageNormalizationError>(&Negative)
                ->Code == UsageNormalizationErrorCode::NegativeValue,
        "negative Chat usage must fail");

  const auto Cached = ChatUsageNormalizer::Normalize(
      ChatUsage(5, std::nullopt, std::nullopt, 6));
  Check(std::get_if<Providers::OpenAIChatCompletions::
                        UsageNormalizationError>(&Cached)
                ->Code == UsageNormalizationErrorCode::CachedExceedsInput,
        "Chat cached tokens above prompt must fail");

  const auto Reasoning = ChatUsageNormalizer::Normalize(
      ChatUsage(std::nullopt, 5, std::nullopt, std::nullopt, 6));
  Check(std::get_if<Providers::OpenAIChatCompletions::
                        UsageNormalizationError>(&Reasoning)
                ->Code ==
            UsageNormalizationErrorCode::ReasoningExceedsOutput,
        "Chat reasoning tokens above completion must fail");

  const auto Inconsistent =
      ChatUsageNormalizer::Normalize(ChatUsage(2, 3, 6));
  Check(std::get_if<Providers::OpenAIChatCompletions::
                        UsageNormalizationError>(&Inconsistent)
                ->Code == UsageNormalizationErrorCode::InconsistentTotal,
        "inconsistent Chat total must fail");
}

void AnthropicMapsCacheFieldsWithoutSynthesizingTotal() {
  using Providers::AnthropicMessages::AnthropicUsageNormalizer;
  using Providers::AnthropicMessages::UsageNormalizationErrorCode;

  const auto Full = AnthropicUsageNormalizer::Normalize(AnthropicUsage(
      10, 5, 30, 70, Providers::UsageCompleteness::Final));
  const auto& SnapshotValue = Snapshot(Full);
  Check(SnapshotValue.InputTokens == 10U &&
            SnapshotValue.OutputTokens == 5U &&
            !SnapshotValue.TotalTokens.has_value() &&
            SnapshotValue.CachedInputWriteTokens == 30U &&
            SnapshotValue.CachedInputReadTokens == 70U &&
            !SnapshotValue.ReasoningOutputTokens.has_value(),
        "Anthropic cache fields must map without synthesized total");

  const auto Empty = AnthropicUsageNormalizer::Normalize(AnthropicUsage(
      std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      Providers::UsageCompleteness::Unavailable));
  const auto& EmptyValue = Snapshot(Empty);
  Check(!EmptyValue.InputTokens.has_value() &&
            !EmptyValue.OutputTokens.has_value() &&
            !EmptyValue.CachedInputReadTokens.has_value() &&
            !EmptyValue.CachedInputWriteTokens.has_value() &&
            EmptyValue.Completeness ==
                Providers::UsageCompleteness::Unavailable,
        "empty Anthropic usage must stay null and unavailable");

  const auto Partial = AnthropicUsageNormalizer::Normalize(AnthropicUsage(
      std::nullopt, 4, std::nullopt, std::nullopt,
      Providers::UsageCompleteness::Partial));
  Check(Snapshot(Partial).Completeness ==
            Providers::UsageCompleteness::Partial,
        "Anthropic partial usage completeness must be preserved");

  const auto Negative = AnthropicUsageNormalizer::Normalize(
      AnthropicUsage(std::nullopt, std::nullopt, std::nullopt, -1));
  Check(std::get_if<Providers::AnthropicMessages::UsageNormalizationError>(
            &Negative)
                ->Code == UsageNormalizationErrorCode::NegativeValue,
        "negative Anthropic usage must fail");
}

} // namespace

int main() {
  ResponsesMapsFieldsAndPreservesMissing();
  ChatMapsFieldsAndPreservesMissing();
  AnthropicMapsCacheFieldsWithoutSynthesizingTotal();
  std::cout << "UsageNormalization contract tests passed\n";
  return 0;
}
