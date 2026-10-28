#include "Source/Core/InstructionPolicy.hpp"

#include <cstddef>

namespace EmulativeDialog::Core {

InstructionPolicy InstructionPolicy::V1() noexcept {
  // Private constructor is accessible here.
  return InstructionPolicy{V1Version, V1AiDisclosure};
}

std::optional<InstructionRoleExpectation> InstructionPolicy::ExpectationFor(
    Context::ContextItemKind Kind,
    Context::ContextTrustClass Trust) const noexcept {
  using Context::ContextItemKind;
  using Context::ContextTrustClass;

  switch (Kind) {
  case ContextItemKind::SystemInstructions:
    if (Trust == ContextTrustClass::TrustedInstruction) {
      return InstructionRoleExpectation{
          .Role = Context::ProviderMessageRole::System,
          .Rank = InstructionRank::System,
      };
    }
    return std::nullopt;
  case ContextItemKind::DeveloperInstructions:
    if (Trust == ContextTrustClass::TrustedInstruction) {
      return InstructionRoleExpectation{
          .Role = Context::ProviderMessageRole::Developer,
          .Rank = InstructionRank::Developer,
      };
    }
    return std::nullopt;
  case ContextItemKind::CurrentUserMessage:
    if (Trust == ContextTrustClass::TrustedCurrentUser) {
      return InstructionRoleExpectation{
          .Role = Context::ProviderMessageRole::User,
          .Rank = InstructionRank::CurrentUser,
      };
    }
    return std::nullopt;
  case ContextItemKind::SessionContext:
    if (Trust == ContextTrustClass::SessionContext) {
      return InstructionRoleExpectation{
          .Role = Context::ProviderMessageRole::Assistant,
          .Rank = InstructionRank::LowTrust,
      };
    }
    return std::nullopt;
  case ContextItemKind::ConversationHistory:
    if (Trust == ContextTrustClass::LowTrustHistory) {
      return InstructionRoleExpectation{
          .Role = Context::ProviderMessageRole::Assistant,
          .Rank = InstructionRank::LowTrust,
      };
    }
    return std::nullopt;
  case ContextItemKind::ActiveChatTask:
  case ContextItemKind::IdentityPreference:
  case ContextItemKind::GroupMemory:
  case ContextItemKind::IdentityLinkMemory:
    if (Trust == ContextTrustClass::AuthorizedMemory) {
      return InstructionRoleExpectation{
          .Role = Context::ProviderMessageRole::Assistant,
          .Rank = InstructionRank::LowTrust,
      };
    }
    return std::nullopt;
  case ContextItemKind::IdentityPolicy:
    // Core policy is carried out-of-band; a bundle item must never inject it.
    return std::nullopt;
  }
  return std::nullopt;
}

bool InstructionPolicy::IsProtectedKind(
    Context::ContextItemKind Kind) const noexcept {
  return Kind == Context::ContextItemKind::SystemInstructions ||
         Kind == Context::ContextItemKind::DeveloperInstructions ||
         Kind == Context::ContextItemKind::IdentityPolicy;
}

bool InstructionPolicy::IsCanonicalUtf8(std::string_view Text) noexcept {
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

} // namespace EmulativeDialog::Core
