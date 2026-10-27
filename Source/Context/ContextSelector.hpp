#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace EmulativeDialog::Context {

struct ContextItemId final {
  std::string Value;

  friend bool operator==(const ContextItemId&, const ContextItemId&) = default;
};

enum class ConversationKind : std::uint8_t {
  Direct,
  Group,
};

struct ConversationScopeKey final {
  std::string ChannelId;
  std::string AccountId;
  std::string ConversationId;
  ConversationKind Kind{ConversationKind::Direct};
  std::string PrincipalId;

  friend bool operator==(const ConversationScopeKey&,
                         const ConversationScopeKey&) = default;
};

enum class ContextPriorityTier : std::uint8_t {
  Tier1 = 1,
  Tier2 = 2,
  Tier3 = 3,
  Tier4 = 4,
  Tier5 = 5,
  Tier6 = 6,
  Tier7 = 7,
};

enum class ContextItemKind : std::uint8_t {
  SystemInstructions,
  DeveloperInstructions,
  IdentityPolicy,
  CurrentUserMessage,
  SessionContext,
  ConversationHistory,
  ActiveChatTask,
  IdentityPreference,
  GroupMemory,
  IdentityLinkMemory,
};

enum class ProviderMessageRole : std::uint8_t {
  System,
  Developer,
  User,
  Assistant,
};

enum class ContextTrustClass : std::uint8_t {
  TrustedInstruction,
  TrustedCurrentUser,
  SessionContext,
  LowTrustHistory,
  AuthorizedMemory,
};

enum class ContextProvenance : std::uint8_t {
  TrustedPolicy,
  UserExplicit,
  UserConfirmed,
  AuthorizedSummary,
  SessionOnlyDerived,
};

enum class ContextConfidence : std::uint8_t {
  Low,
  Medium,
  High,
};

struct ContextSource final {
  std::string SourceMessageId;
  std::string SourceOperationId;
  std::chrono::sys_seconds CreatedAt{};
  std::optional<std::chrono::sys_seconds> ExpiresAt;
  bool UserConfirmed{false};
  ContextConfidence Confidence{ContextConfidence::Low};
  ContextProvenance Provenance{ContextProvenance::TrustedPolicy};
};

struct TrustedContextText final {
  std::string Utf8Text;
  std::uint64_t TokenCost{};
  ContextSource Source;
};

struct ContextCandidate final {
  ContextItemId Id;
  ContextItemKind Kind{ContextItemKind::ConversationHistory};
  ContextPriorityTier Priority{ContextPriorityTier::Tier3};
  ContextTrustClass Trust{ContextTrustClass::LowTrustHistory};
  ConversationScopeKey Scope;
  ContextSource Source;
  std::string Utf8Text;
  std::uint64_t TokenCost{};
};

struct ContextSelectionBudget final {
  std::uint64_t TokenBudget{};
  std::uint64_t ByteBudget{};
};

struct ContextSelectionRequest final {
  ConversationScopeKey Scope;
  std::uint64_t IdentityPolicyVersion{};
  TrustedContextText SystemInstructions;
  TrustedContextText DeveloperInstructions;
  TrustedContextText CurrentUserMessage;
  ContextSelectionBudget Budget;
  std::vector<ContextCandidate> Candidates;
};

struct SelectedContextItem final {
  ContextItemId Id;
  ContextItemKind Kind{ContextItemKind::ConversationHistory};
  ContextPriorityTier Priority{ContextPriorityTier::Tier3};
  ProviderMessageRole ProviderRole{ProviderMessageRole::Assistant};
  ContextTrustClass Trust{ContextTrustClass::LowTrustHistory};
  ConversationScopeKey Scope;
  ContextSource Source;
  std::string Utf8Text;
  std::uint64_t TokenCost{};
  std::uint64_t ByteCost{};
};

enum class ContextExclusionReason : std::uint8_t {
  ScopeMismatch,
  Expired,
  ReservedProviderRole,
  InvalidPriorityOrTrust,
  MissingSourceReference,
};

struct ExcludedContextItem final {
  ContextItemId Id;
  ContextExclusionReason Reason{
      ContextExclusionReason::InvalidPriorityOrTrust};
};

struct ContextBundle final {
  std::vector<SelectedContextItem> Items;
  std::vector<ContextItemId> TruncatedItemIds;
  std::vector<ExcludedContextItem> ExcludedItems;
  std::uint64_t IdentityPolicyVersion{};
  std::uint64_t SelectedTokenTotal{};
  std::uint64_t SelectedByteTotal{};
  bool SystemInstructionsPreserved{false};
  bool DeveloperInstructionsPreserved{false};
  bool CurrentUserMessagePreserved{false};
};

enum class ContextSelectionErrorCode : std::uint8_t {
  InvalidBudget,
  InvalidScope,
  MissingRequiredContext,
  InvalidUtf8,
  DuplicateItemId,
  BudgetExhausted,
};

struct ContextSelectionFailure final {
  ContextSelectionErrorCode Code{ContextSelectionErrorCode::InvalidBudget};
};

using ContextSelectionResult = std::variant<ContextBundle,
                                             ContextSelectionFailure>;

class ContextSelector final {
public:
  [[nodiscard]] ContextSelectionResult Select(
      const ContextSelectionRequest& Request,
      std::chrono::sys_seconds CurrentTime) const;
};

} // namespace EmulativeDialog::Context
