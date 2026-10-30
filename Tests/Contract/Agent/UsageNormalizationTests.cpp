#include "Source/Providers/AnthropicMessages/AnthropicUsageNormalizer.hpp"
#include "Source/Providers/OpenAIChatCompletions/ChatUsageNormalizer.hpp"
#include "Source/Providers/OpenAIResponses/ResponsesUsageNormalizer.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << "FAIL: " << Message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

using EmulativeDialog::Providers::UsageCompleteness;

void TestResponsesUsagePreservesOnlyProvidedFields() {
  using namespace EmulativeDialog::Providers::OpenAIResponses;
  const auto Partial = NormalizeResponsesUsage(
      ResponsesUsageFields{
          .InputTokens = 71,
          .OutputTokens = std::nullopt,
          .TotalTokens = std::nullopt,
          .CachedInputReadTokens = 12,
          .ReasoningOutputTokens = std::nullopt,
      },
      UsageCompleteness::Partial);
  Check(Partial.Completeness == UsageCompleteness::Partial,
        "Responses partial completeness must be preserved");
  Check(Partial.InputTokens == 71 &&
            Partial.CachedInputReadTokens == 12 &&
            !Partial.OutputTokens.has_value() &&
            !Partial.TotalTokens.has_value() &&
            !Partial.CachedInputWriteTokens.has_value() &&
            !Partial.ReasoningOutputTokens.has_value(),
        "Responses fields must not be inferred or filled with zero");

  const auto Final = NormalizeResponsesUsage(
      ResponsesUsageFields{
          .InputTokens = 100,
          .OutputTokens = 20,
          .TotalTokens = 120,
          .CachedInputReadTokens = 30,
          .ReasoningOutputTokens = 5,
      },
      UsageCompleteness::Final);
  Check(Final.Completeness == UsageCompleteness::Final &&
            Final.InputTokens == 100 && Final.OutputTokens == 20 &&
            Final.TotalTokens == 120 &&
            Final.CachedInputReadTokens == 30 &&
            Final.ReasoningOutputTokens == 5,
        "Responses final protocol fields must map to canonical usage");
  const auto Missing = NormalizeResponsesUsage(std::nullopt,
                                                UsageCompleteness::Final);
  Check(Missing.Completeness == UsageCompleteness::Unavailable &&
            !Missing.InputTokens && !Missing.OutputTokens && !Missing.TotalTokens,
        "Missing Responses usage must become unavailable, not zero");
}

void TestChatCompletionsUsageMapsPromptAndCompletion() {
  using namespace EmulativeDialog::Providers::OpenAIChatCompletions;
  const auto Final = NormalizeChatUsage(
      ChatUsageFields{
          .PromptTokens = 44,
          .CompletionTokens = 19,
          .TotalTokens = 63,
          .CachedPromptTokens = 8,
          .ReasoningTokens = 4,
      },
      UsageCompleteness::Final);
  Check(Final.Completeness == UsageCompleteness::Final &&
            Final.InputTokens == 44 && Final.OutputTokens == 19 &&
            Final.TotalTokens == 63 &&
            Final.CachedInputReadTokens == 8 &&
            Final.ReasoningOutputTokens == 4 &&
            !Final.CachedInputWriteTokens,
        "Chat usage must preserve protocol values without fabricating cache writes");

  const auto Partial = NormalizeChatUsage(
      ChatUsageFields{.PromptTokens = 20,
                      .CompletionTokens = std::nullopt,
                      .TotalTokens = std::nullopt,
                      .CachedPromptTokens = std::nullopt,
                      .ReasoningTokens = std::nullopt},
      UsageCompleteness::Partial);
  Check(Partial.Completeness == UsageCompleteness::Partial &&
            Partial.InputTokens == 20 && !Partial.OutputTokens &&
            !Partial.TotalTokens,
        "Chat partial usage must leave absent counts unknown");
  const auto Unavailable = NormalizeChatUsage(
      ChatUsageFields{.PromptTokens = 20,
                      .CompletionTokens = std::nullopt,
                      .TotalTokens = std::nullopt,
                      .CachedPromptTokens = std::nullopt,
                      .ReasoningTokens = std::nullopt},
      UsageCompleteness::Unavailable);
  Check(Unavailable.Completeness == UsageCompleteness::Unavailable &&
            !Unavailable.InputTokens,
        "Unavailable Chat usage must not carry stale numeric values");

  const auto Missing = NormalizeChatUsage(std::nullopt,
                                          UsageCompleteness::Final);
  Check(Missing.Completeness == UsageCompleteness::Unavailable &&
            !Missing.InputTokens && !Missing.OutputTokens &&
            !Missing.TotalTokens && !Missing.CachedInputReadTokens &&
            !Missing.CachedInputWriteTokens &&
            !Missing.ReasoningOutputTokens,
        "Absent Chat usage must become unavailable with all counts unknown");
}

void TestAnthropicUsageIsCumulativeAndNeverSynthesizesTotal() {
  using namespace EmulativeDialog::Providers::AnthropicMessages;
  const auto Earlier = NormalizeAnthropicUsage(
      AnthropicUsageFields{.InputTokens = 31,
                           .OutputTokens = std::nullopt,
                           .CacheReadInputTokens = 7,
                           .CacheCreationInputTokens = 2},
      UsageCompleteness::Partial);
  const auto Later = NormalizeAnthropicUsage(
      AnthropicUsageFields{.InputTokens = 31,
                           .OutputTokens = 11,
                           .CacheReadInputTokens = 9,
                           .CacheCreationInputTokens = 3},
      UsageCompleteness::Final);
  Check(Earlier.InputTokens == 31 && !Earlier.OutputTokens &&
            Earlier.CachedInputReadTokens == 7 &&
            Earlier.CachedInputWriteTokens == 2 &&
            Earlier.Completeness == UsageCompleteness::Partial,
        "Anthropic partial cumulative snapshot must preserve only known values");
  Check(Later.InputTokens == 31 && Later.OutputTokens == 11 &&
            Later.CachedInputReadTokens == 9 &&
            Later.CachedInputWriteTokens == 3 && !Later.TotalTokens &&
            !Later.ReasoningOutputTokens &&
            Later.Completeness == UsageCompleteness::Final,
        "Anthropic later cumulative snapshot replaces fields without a synthesized total");
  const auto Missing = NormalizeAnthropicUsage(
      std::nullopt, UsageCompleteness::Partial);
  Check(Missing.Completeness == UsageCompleteness::Unavailable &&
            !Missing.InputTokens && !Missing.CachedInputReadTokens,
        "Missing Anthropic usage must be unavailable");
}

} // namespace

int main() {
  TestResponsesUsagePreservesOnlyProvidedFields();
  TestChatCompletionsUsageMapsPromptAndCompletion();
  TestAnthropicUsageIsCumulativeAndNeverSynthesizesTotal();
  std::cout << "Provider usage normalization contract tests passed\n";
  return EXIT_SUCCESS;
}
