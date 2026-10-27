#include "Source/Context/ContextSelector.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>

namespace EmulativeDialog::Context {
namespace {

constexpr std::string_view SelectedSystemId = "context-system-instructions";
constexpr std::string_view SelectedDeveloperId =
    "context-developer-instructions";
constexpr std::string_view SelectedCurrentMessageId =
    "context-current-user-message";

bool IsValidUtf8(std::string_view Text) {
  std::size_t Index = 0;
  while (Index < Text.size()) {
    const auto First = static_cast<unsigned char>(Text[Index]);
    if (First <= 0x7FU) {
      ++Index;
      continue;
    }

    std::size_t ContinuationCount = 0;
    std::uint32_t CodePoint = 0;
    std::uint32_t MinimumCodePoint = 0;
    if (First >= 0xC2U && First <= 0xDFU) {
      ContinuationCount = 1;
      CodePoint = First & 0x1FU;
      MinimumCodePoint = 0x80U;
    } else if (First >= 0xE0U && First <= 0xEFU) {
      ContinuationCount = 2;
      CodePoint = First & 0x0FU;
      MinimumCodePoint = 0x800U;
    } else if (First >= 0xF0U && First <= 0xF4U) {
      ContinuationCount = 3;
      CodePoint = First & 0x07U;
      MinimumCodePoint = 0x10000U;
    } else {
      return false;
    }

    if (Text.size() - Index - 1U < ContinuationCount) {
      return false;
    }
    for (std::size_t Offset = 1; Offset <= ContinuationCount; ++Offset) {
      const auto Next = static_cast<unsigned char>(Text[Index + Offset]);
      if ((Next & 0xC0U) != 0x80U) {
        return false;
      }
      CodePoint = (CodePoint << 6U) | (Next & 0x3FU);
    }
    if (CodePoint < MinimumCodePoint || CodePoint > 0x10FFFFU ||
        (CodePoint >= 0xD800U && CodePoint <= 0xDFFFU)) {
      return false;
    }
    Index += ContinuationCount + 1U;
  }
  return true;
}

bool HasSourceReference(const ContextSource& Source) {
  return !Source.SourceMessageId.empty() || !Source.SourceOperationId.empty();
}

bool IsReservedKind(ContextItemKind Kind) {
  return Kind == ContextItemKind::SystemInstructions ||
         Kind == ContextItemKind::DeveloperInstructions ||
         Kind == ContextItemKind::IdentityPolicy ||
         Kind == ContextItemKind::CurrentUserMessage;
}

bool HasValidPriorityAndTrust(const ContextCandidate& Candidate) {
  switch (Candidate.Kind) {
  case ContextItemKind::SessionContext:
    return Candidate.Priority == ContextPriorityTier::Tier3 &&
           Candidate.Trust == ContextTrustClass::SessionContext;
  case ContextItemKind::ConversationHistory:
    return Candidate.Priority == ContextPriorityTier::Tier3 &&
           Candidate.Trust == ContextTrustClass::LowTrustHistory;
  case ContextItemKind::ActiveChatTask:
    return Candidate.Priority == ContextPriorityTier::Tier4 &&
           Candidate.Trust == ContextTrustClass::AuthorizedMemory;
  case ContextItemKind::IdentityPreference:
    return Candidate.Priority == ContextPriorityTier::Tier5 &&
           Candidate.Trust == ContextTrustClass::AuthorizedMemory;
  case ContextItemKind::GroupMemory:
    return Candidate.Priority == ContextPriorityTier::Tier6 &&
           Candidate.Trust == ContextTrustClass::AuthorizedMemory;
  case ContextItemKind::IdentityLinkMemory:
    return Candidate.Priority == ContextPriorityTier::Tier7 &&
           Candidate.Trust == ContextTrustClass::AuthorizedMemory;
  case ContextItemKind::SystemInstructions:
  case ContextItemKind::DeveloperInstructions:
  case ContextItemKind::IdentityPolicy:
  case ContextItemKind::CurrentUserMessage:
    return false;
  }
  return false;
}

int KindOrder(ContextItemKind Kind) {
  switch (Kind) {
  case ContextItemKind::SessionContext:
    return 0;
  case ContextItemKind::ConversationHistory:
    return 1;
  case ContextItemKind::ActiveChatTask:
    return 2;
  case ContextItemKind::IdentityPreference:
    return 3;
  case ContextItemKind::GroupMemory:
    return 4;
  case ContextItemKind::IdentityLinkMemory:
    return 5;
  case ContextItemKind::SystemInstructions:
  case ContextItemKind::DeveloperInstructions:
  case ContextItemKind::IdentityPolicy:
  case ContextItemKind::CurrentUserMessage:
    return 6;
  }
  return 6;
}

ContextSelectionFailure Failure(ContextSelectionErrorCode Code) {
  return ContextSelectionFailure{.Code = Code};
}

bool HasValidScope(const ConversationScopeKey& Scope) {
  return !Scope.ChannelId.empty() && !Scope.AccountId.empty() &&
         !Scope.ConversationId.empty() && !Scope.PrincipalId.empty();
}

std::optional<ContextSelectionFailure> ValidateRequiredText(
    const TrustedContextText& Text) {
  if (Text.Utf8Text.empty() || !HasSourceReference(Text.Source)) {
    return Failure(ContextSelectionErrorCode::MissingRequiredContext);
  }
  if (!IsValidUtf8(Text.Utf8Text)) {
    return Failure(ContextSelectionErrorCode::InvalidUtf8);
  }
  return std::nullopt;
}

SelectedContextItem MakeRequiredItem(
    ContextItemId Id,
    ContextItemKind Kind,
    ContextPriorityTier Priority,
    ProviderMessageRole Role,
    ContextTrustClass Trust,
    const ConversationScopeKey& Scope,
    TrustedContextText Text) {
  const std::uint64_t ByteCost =
      static_cast<std::uint64_t>(Text.Utf8Text.size());
  return SelectedContextItem{
      .Id = std::move(Id),
      .Kind = Kind,
      .Priority = Priority,
      .ProviderRole = Role,
      .Trust = Trust,
      .Scope = Scope,
      .Source = std::move(Text.Source),
      .Utf8Text = std::move(Text.Utf8Text),
      .TokenCost = Text.TokenCost,
      .ByteCost = ByteCost,
  };
}

} // namespace

ContextSelectionResult ContextSelector::Select(
    const ContextSelectionRequest& Request,
    std::chrono::sys_seconds CurrentTime) const {
  if (!HasValidScope(Request.Scope) || Request.IdentityPolicyVersion == 0) {
    return Failure(ContextSelectionErrorCode::InvalidScope);
  }
  if (Request.Budget.TokenBudget == 0 || Request.Budget.ByteBudget == 0) {
    return Failure(ContextSelectionErrorCode::InvalidBudget);
  }

  for (const TrustedContextText* Text :
       {&Request.SystemInstructions,
        &Request.DeveloperInstructions,
        &Request.CurrentUserMessage}) {
    if (const auto RequiredFailure = ValidateRequiredText(*Text);
        RequiredFailure.has_value()) {
      return *RequiredFailure;
    }
  }

  std::vector<SelectedContextItem> RequiredItems;
  RequiredItems.reserve(3);
  RequiredItems.push_back(MakeRequiredItem(
      ContextItemId{std::string(SelectedSystemId)},
      ContextItemKind::SystemInstructions, ContextPriorityTier::Tier1,
      ProviderMessageRole::System, ContextTrustClass::TrustedInstruction,
      Request.Scope, Request.SystemInstructions));
  RequiredItems.push_back(MakeRequiredItem(
      ContextItemId{std::string(SelectedDeveloperId)},
      ContextItemKind::DeveloperInstructions, ContextPriorityTier::Tier1,
      ProviderMessageRole::Developer, ContextTrustClass::TrustedInstruction,
      Request.Scope, Request.DeveloperInstructions));
  RequiredItems.push_back(MakeRequiredItem(
      ContextItemId{std::string(SelectedCurrentMessageId)},
      ContextItemKind::CurrentUserMessage, ContextPriorityTier::Tier2,
      ProviderMessageRole::User, ContextTrustClass::TrustedCurrentUser,
      Request.Scope, Request.CurrentUserMessage));

  std::uint64_t RequiredTokens = 0;
  std::uint64_t RequiredBytes = 0;
  for (const SelectedContextItem& Item : RequiredItems) {
    if (Item.TokenCost >
            std::numeric_limits<std::uint64_t>::max() - RequiredTokens ||
        Item.ByteCost >
            std::numeric_limits<std::uint64_t>::max() - RequiredBytes) {
      return Failure(ContextSelectionErrorCode::InvalidBudget);
    }
    RequiredTokens += Item.TokenCost;
    RequiredBytes += Item.ByteCost;
  }
  if (RequiredTokens > Request.Budget.TokenBudget ||
      RequiredBytes > Request.Budget.ByteBudget) {
    return Failure(ContextSelectionErrorCode::BudgetExhausted);
  }

  ContextBundle Bundle;
  Bundle.IdentityPolicyVersion = Request.IdentityPolicyVersion;
  Bundle.SystemInstructionsPreserved = true;
  Bundle.DeveloperInstructionsPreserved = true;
  Bundle.CurrentUserMessagePreserved = true;
  Bundle.Items = std::move(RequiredItems);
  Bundle.SelectedTokenTotal = RequiredTokens;
  Bundle.SelectedByteTotal = RequiredBytes;

  std::vector<ContextCandidate> Eligible;
  Eligible.reserve(Request.Candidates.size());
  std::vector<ContextItemId> SeenIds;
  SeenIds.reserve(Request.Candidates.size() + Bundle.Items.size());
  for (const SelectedContextItem& Item : Bundle.Items) {
    SeenIds.push_back(Item.Id);
  }

  for (const ContextCandidate& Candidate : Request.Candidates) {
    if (Candidate.Id.Value.empty()) {
      return Failure(ContextSelectionErrorCode::DuplicateItemId);
    }
    if (std::find(SeenIds.begin(), SeenIds.end(), Candidate.Id) !=
        SeenIds.end()) {
      return Failure(ContextSelectionErrorCode::DuplicateItemId);
    }
    SeenIds.push_back(Candidate.Id);

    if (Candidate.Scope != Request.Scope) {
      Bundle.ExcludedItems.push_back(
          {.Id = Candidate.Id,
           .Reason = ContextExclusionReason::ScopeMismatch});
      continue;
    }
    if (Candidate.Source.ExpiresAt.has_value() &&
        *Candidate.Source.ExpiresAt <= CurrentTime) {
      Bundle.ExcludedItems.push_back(
          {.Id = Candidate.Id, .Reason = ContextExclusionReason::Expired});
      continue;
    }
    if (IsReservedKind(Candidate.Kind) ||
        !HasValidPriorityAndTrust(Candidate)) {
      Bundle.ExcludedItems.push_back(
          {.Id = Candidate.Id,
           .Reason = ContextExclusionReason::ReservedProviderRole});
      continue;
    }
    if (!HasSourceReference(Candidate.Source)) {
      Bundle.ExcludedItems.push_back(
          {.Id = Candidate.Id,
           .Reason = ContextExclusionReason::MissingSourceReference});
      continue;
    }
    if (Candidate.Utf8Text.empty()) {
      return Failure(ContextSelectionErrorCode::MissingRequiredContext);
    }
    if (!IsValidUtf8(Candidate.Utf8Text)) {
      return Failure(ContextSelectionErrorCode::InvalidUtf8);
    }
    Eligible.push_back(Candidate);
  }

  std::sort(Eligible.begin(), Eligible.end(),
            [](const ContextCandidate& Left,
               const ContextCandidate& Right) {
              if (Left.Priority != Right.Priority) {
                return Left.Priority < Right.Priority;
              }
              if (Left.Kind != Right.Kind) {
                return KindOrder(Left.Kind) < KindOrder(Right.Kind);
              }
              if (Left.Source.CreatedAt != Right.Source.CreatedAt) {
                return Left.Source.CreatedAt > Right.Source.CreatedAt;
              }
              return Left.Id.Value < Right.Id.Value;
            });

  bool BudgetExhausted = false;
  for (const ContextCandidate& Candidate : Eligible) {
    const auto CandidateBytes =
        static_cast<std::uint64_t>(Candidate.Utf8Text.size());
    if (BudgetExhausted ||
        Candidate.TokenCost >
            Request.Budget.TokenBudget - Bundle.SelectedTokenTotal ||
        CandidateBytes >
            Request.Budget.ByteBudget - Bundle.SelectedByteTotal) {
      BudgetExhausted = true;
      Bundle.TruncatedItemIds.push_back(Candidate.Id);
      continue;
    }

    Bundle.SelectedTokenTotal += Candidate.TokenCost;
    Bundle.SelectedByteTotal += CandidateBytes;
    Bundle.Items.push_back(SelectedContextItem{
        .Id = Candidate.Id,
        .Kind = Candidate.Kind,
        .Priority = Candidate.Priority,
        .ProviderRole = ProviderMessageRole::Assistant,
        .Trust = Candidate.Trust,
        .Scope = Candidate.Scope,
        .Source = Candidate.Source,
        .Utf8Text = Candidate.Utf8Text,
        .TokenCost = Candidate.TokenCost,
        .ByteCost = CandidateBytes,
    });
  }

  return Bundle;
}

} // namespace EmulativeDialog::Context
