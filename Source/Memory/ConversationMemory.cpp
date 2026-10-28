#include "Source/Memory/ConversationMemory.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace EmulativeDialog::Memory {
namespace {

using Context::ContextCandidate;
using Context::ContextConfidence;
using Context::ContextItemId;
using Context::ContextItemKind;
using Context::ContextPriorityTier;
using Context::ContextProvenance;
using Context::ContextSource;
using Context::ContextTrustClass;
using Context::ConversationKind;
using Context::ConversationScopeKey;

constexpr std::string_view SensitiveMarkers[] = {
    "BEGIN PRIVATE KEY", "password=", "access_token=", "client_secret=",
    "Bearer "};

bool ContainsSensitiveMarker(std::string_view Content) {
  for (const auto Marker : SensitiveMarkers) {
    if (Content.find(Marker) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

ContextItemKind ToContextKind(MemoryCategory Category) {
  switch (Category) {
  case MemoryCategory::IdentityPreference:
  case MemoryCategory::ConversationSummary:
    return ContextItemKind::IdentityPreference;
  case MemoryCategory::GroupMemory:
    return ContextItemKind::GroupMemory;
  case MemoryCategory::IdentityLinkMemory:
    return ContextItemKind::IdentityLinkMemory;
  case MemoryCategory::ActiveConversationContext:
    return ContextItemKind::ActiveChatTask;
  }
  return ContextItemKind::IdentityPreference;
}

ContextPriorityTier ToTier(MemoryCategory Category) {
  switch (Category) {
  case MemoryCategory::ActiveConversationContext:
    return ContextPriorityTier::Tier4;
  case MemoryCategory::IdentityPreference:
  case MemoryCategory::ConversationSummary:
    return ContextPriorityTier::Tier5;
  case MemoryCategory::GroupMemory:
    return ContextPriorityTier::Tier6;
  case MemoryCategory::IdentityLinkMemory:
    return ContextPriorityTier::Tier7;
  }
  return ContextPriorityTier::Tier5;
}

ContextProvenance ToContextProvenance(MemoryProvenance Provenance) {
  switch (Provenance) {
  case MemoryProvenance::UserExplicit:
    return ContextProvenance::UserExplicit;
  case MemoryProvenance::UserConfirmed:
    return ContextProvenance::UserConfirmed;
  case MemoryProvenance::AuthorizedSummary:
    return ContextProvenance::AuthorizedSummary;
  }
  return ContextProvenance::UserExplicit;
}

std::uint64_t EstimateTokens(std::string_view Text) {
  return static_cast<std::uint64_t>(Text.size() / 4U) + 1U;
}

} // namespace

bool AuthorizedMemoryContext::IsComplete() const noexcept {
  return !ChannelId.empty() && !AccountId.empty() && !ConversationId.empty() &&
         !PrincipalId.empty() && IdentityPolicyVersion > 0 &&
         IdentityVerified;
}

ConversationMemory::ConversationMemory(MemoryStoragePort& Storage,
                                       std::size_t PerScopeCapacity) noexcept
    : Storage_(Storage), PerScopeCapacity_(PerScopeCapacity == 0U
                                               ? 1U
                                               : PerScopeCapacity) {}

MemoryError ConversationMemory::Error(MemoryErrorCode Code,
                                      std::string Detail) {
  return MemoryError{.Code = Code, .Detail = std::move(Detail)};
}

std::optional<MemoryError> ConversationMemory::Authorize(
    const AuthorizedMemoryContext& Context,
    const MemoryWriteRequest& Request) {
  if (!Context.IsComplete()) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "verified principal, scope, and policy version are required");
  }
  if (Request.MemoryId.empty() || Request.MemoryId.size() > 128U ||
      Request.SubjectId.empty() || Request.Content.empty() ||
      Request.Content.size() > 8192U || Request.SourceReference.empty() ||
      Request.ConsentId.empty()) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "memory id, subject, content, source, and consent are "
                 "required and bounded");
  }
  if (Request.Version == 0U) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "memory version must be positive");
  }
  if (Request.Provenance == MemoryProvenance::UserConfirmed &&
      !Request.UserConfirmed) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "user-confirmed provenance requires explicit confirmation");
  }
  if (ContainsSensitiveMarker(Request.Content)) {
    return Error(MemoryErrorCode::SensitiveContent,
                 "memory content contains a forbidden sensitive marker");
  }
  return std::nullopt;
}

StoredMemory ConversationMemory::ScopeTemplate(
    const AuthorizedMemoryContext& Context) const {
  StoredMemory Template;
  Template.ChannelId = Context.ChannelId;
  Template.AccountId = Context.AccountId;
  Template.ConversationId = Context.ConversationId;
  Template.PrincipalId = Context.PrincipalId;
  return Template;
}

const ConversationMemory::ActiveConsent* ConversationMemory::FindConsent(
    std::string_view ConsentId) const {
  for (const ActiveConsent& Consent : Consents_) {
    if (Consent.Grant.ConsentId == ConsentId) {
      return &Consent;
    }
  }
  return nullptr;
}

bool ConversationMemory::ConsentMatches(
    const ActiveConsent& Consent, const AuthorizedMemoryContext& Context,
    MemoryCategory Category) const {
  return Consent.Active && Consent.Grant.Granted &&
         Consent.Grant.GrantorPrincipalId == Context.PrincipalId &&
         Consent.Grant.ChannelId == Context.ChannelId &&
         Consent.Grant.AccountId == Context.AccountId &&
         Consent.Grant.ConversationId == Context.ConversationId &&
         Consent.Grant.Category == Category;
}

MemoryResult<std::monostate> ConversationMemory::RegisterConsent(
    const ConsentGrant& Grant) {
  if (Grant.ConsentId.empty() || Grant.GrantorPrincipalId.empty() ||
      Grant.ChannelId.empty() || Grant.AccountId.empty() ||
      Grant.ConversationId.empty()) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "consent requires id, verified grantor, and exact scope");
  }
  if (FindConsent(Grant.ConsentId) != nullptr) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "consent id is already registered");
  }
  Consents_.push_back(ActiveConsent{.Grant = Grant, .Active = Grant.Granted});
  return std::monostate{};
}

bool ConversationMemory::IsConsentActive(std::string_view ConsentId) const {
  const auto* Consent = FindConsent(ConsentId);
  return Consent != nullptr && Consent->Active && Consent->Grant.Granted;
}

MemoryResult<std::monostate> ConversationMemory::RevokeConsent(
    const AuthorizedMemoryContext& Context, std::string_view ConsentId,
    std::chrono::sys_seconds Now) {
  if (!Context.IsComplete()) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "verified principal and scope are required");
  }
  for (ActiveConsent& Consent : Consents_) {
    if (Consent.Grant.ConsentId != ConsentId) {
      continue;
    }
    if (Consent.Grant.GrantorPrincipalId != Context.PrincipalId) {
      return Error(MemoryErrorCode::UnauthorizedIdentity,
                   "only the granting principal may revoke consent");
    }
    Consent.Active = false;
    const auto Records = ListActive(Context, Now);
    if (const auto* RecordsError = std::get_if<MemoryError>(&Records)) {
      return *RecordsError;
    }
    for (const StoredMemory& Record : std::get<std::vector<StoredMemory>>(
             Records)) {
      if (Record.ConsentId != ConsentId) {
        continue;
      }
      StoredMemory Template = ScopeTemplate(Context);
      Template.ChannelId = Record.ChannelId;
      Template.AccountId = Record.AccountId;
      Template.ConversationId = Record.ConversationId;
      Template.PrincipalId = Record.PrincipalId;
      const auto Tombstone =
          Storage_.Tombstone(Template, Record.MemoryId, Now);
      if (const auto* StorageError =
              std::get_if<MemoryStorageError>(&Tombstone)) {
        return Error(MemoryErrorCode::StorageFailure,
                     StorageError->Detail);
      }
    }
    return std::monostate{};
  }
  return Error(MemoryErrorCode::InvalidArgument, "consent is not registered");
}

MemoryResult<std::monostate> ConversationMemory::Write(
    const AuthorizedMemoryContext& Context,
    const MemoryWriteRequest& Request, std::chrono::sys_seconds Now) {
  if (auto Authorization = Authorize(Context, Request);
      Authorization.has_value()) {
    return *Authorization;
  }
  const auto* Consent = FindConsent(Request.ConsentId);
  if (Consent == nullptr) {
    return Error(MemoryErrorCode::ConsentRequired,
                 "durable memory requires a registered Core consent");
  }
  if (!ConsentMatches(*Consent, Context, Request.Category)) {
    return Error(MemoryErrorCode::ConsentMismatch,
                 "consent does not cover this principal, scope, or category");
  }
  if (Request.ExpiresAt.has_value() && *Request.ExpiresAt <= Now) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "new memory is already expired");
  }

  const auto Existing = ListActive(Context, Now);
  if (const auto* ExistingError = std::get_if<MemoryError>(&Existing)) {
    return *ExistingError;
  }
  auto Active = std::get<std::vector<StoredMemory>>(Existing);
  if (Active.size() >= PerScopeCapacity_) {
    std::sort(Active.begin(), Active.end(),
              [](const StoredMemory& Left, const StoredMemory& Right) {
                if (Left.CreatedAt != Right.CreatedAt) {
                  return Left.CreatedAt < Right.CreatedAt;
                }
                return Left.MemoryId < Right.MemoryId;
              });
    const StoredMemory& Victim = Active.front();
    StoredMemory Template = ScopeTemplate(Context);
    const auto Tombstone =
        Storage_.Tombstone(Template, Victim.MemoryId, Now);
    if (const auto* StorageError =
            std::get_if<MemoryStorageError>(&Tombstone)) {
      return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
    }
  }

  StoredMemory Record = ScopeTemplate(Context);
  Record.MemoryId = Request.MemoryId;
  Record.Category = Request.Category;
  Record.SubjectId = Request.SubjectId;
  Record.Content = Request.Content;
  Record.SourceReference = Request.SourceReference;
  Record.ConsentId = Request.ConsentId;
  Record.CreatedAt = Request.CreatedAt == std::chrono::sys_seconds{}
                         ? Now
                         : Request.CreatedAt;
  Record.ExpiresAt = Request.ExpiresAt;
  Record.Provenance = Request.Provenance;
  Record.UserConfirmed = Request.UserConfirmed;
  Record.Version = Request.Version;
  if (Record.CreatedAt > Now) {
    Record.CreatedAt = Now;
  }
  const auto Put = Storage_.Put(Record);
  if (const auto* StorageError = std::get_if<MemoryStorageError>(&Put)) {
    return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
  }
  return std::monostate{};
}

MemoryResult<std::vector<StoredMemory>> ConversationMemory::ListActive(
    const AuthorizedMemoryContext& Context,
    std::chrono::sys_seconds Now) const {
  if (!Context.IsComplete()) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "verified principal and scope are required");
  }
  const auto Listed = Storage_.ListActive(ScopeTemplate(Context), Now);
  if (const auto* StorageError = std::get_if<MemoryStorageError>(&Listed)) {
    return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
  }
  return std::get<std::vector<StoredMemory>>(Listed);
}

MemoryResult<std::vector<ContextCandidate>> ConversationMemory::Recall(
    const AuthorizedMemoryContext& Context, std::chrono::sys_seconds Now,
    std::uint64_t TokenBudget) const {
  const auto Listed = ListActive(Context, Now);
  if (const auto* ListedError = std::get_if<MemoryError>(&Listed)) {
    return *ListedError;
  }
  auto Records = std::get<std::vector<StoredMemory>>(Listed);
  Records.erase(
      std::remove_if(Records.begin(), Records.end(),
                     [this](const StoredMemory& Record) {
                       const auto* Consent = FindConsent(Record.ConsentId);
                       return Consent == nullptr || !Consent->Active ||
                              !Consent->Grant.Granted;
                     }),
      Records.end());
  std::sort(Records.begin(), Records.end(),
            [](const StoredMemory& Left, const StoredMemory& Right) {
              const auto LeftTier = ToTier(Left.Category);
              const auto RightTier = ToTier(Right.Category);
              if (LeftTier != RightTier) {
                return LeftTier < RightTier;
              }
              if (Left.CreatedAt != Right.CreatedAt) {
                return Left.CreatedAt > Right.CreatedAt;
              }
              return Left.MemoryId < Right.MemoryId;
            });
  std::vector<ContextCandidate> Candidates;
  std::uint64_t UsedTokens = 0;
  for (const StoredMemory& Record : Records) {
    const auto Cost = EstimateTokens(Record.Content);
    if (UsedTokens > TokenBudget || Cost > TokenBudget - UsedTokens) {
      break;
    }
    ContextCandidate Candidate;
    Candidate.Id = ContextItemId{.Value = Record.MemoryId};
    Candidate.Kind = ToContextKind(Record.Category);
    Candidate.Priority = ToTier(Record.Category);
    Candidate.Trust = ContextTrustClass::AuthorizedMemory;
    Candidate.Scope = ConversationScopeKey{
        .ChannelId = Record.ChannelId,
        .AccountId = Record.AccountId,
        .ConversationId = Record.ConversationId,
        .Kind = Record.Category == MemoryCategory::GroupMemory
                    ? ConversationKind::Group
                    : ConversationKind::Direct,
        .PrincipalId = Record.PrincipalId,
    };
    Candidate.Source = ContextSource{
        .SourceMessageId = Record.SourceReference,
        .SourceOperationId = {},
        .CreatedAt = Record.CreatedAt,
        .ExpiresAt = Record.ExpiresAt,
        .UserConfirmed = Record.UserConfirmed,
        .Confidence = Record.UserConfirmed ? ContextConfidence::High
                                           : ContextConfidence::Medium,
        .Provenance = ToContextProvenance(Record.Provenance),
    };
    Candidate.Utf8Text = Record.Content;
    Candidate.TokenCost = Cost;
    Candidates.push_back(std::move(Candidate));
    UsedTokens += Cost;
  }
  return Candidates;
}

MemoryResult<std::monostate> ConversationMemory::Forget(
    const AuthorizedMemoryContext& Context, const ForgetRequest& Request,
    std::chrono::sys_seconds Now) {
  if (!Context.IsComplete()) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "verified principal and scope are required");
  }
  const bool HasTarget = Request.MemoryId.has_value() ||
                         Request.Category.has_value() ||
                         Request.EntireAuthorizedScope;
  if (!HasTarget) {
    return Error(MemoryErrorCode::ForgetScopeRequired,
                 "forget requires a record, category, or exact authorized "
                 "scope");
  }
  const auto Listed = ListActive(Context, Now);
  if (const auto* ListedError = std::get_if<MemoryError>(&Listed)) {
    return *ListedError;
  }
  const auto Records = std::get<std::vector<StoredMemory>>(Listed);
  std::size_t Tombstoned = 0;
  for (const StoredMemory& Record : Records) {
    if (Request.MemoryId.has_value() &&
        Record.MemoryId != *Request.MemoryId) {
      continue;
    }
    if (Request.Category.has_value() &&
        Record.Category != *Request.Category) {
      continue;
    }
    const auto Result =
        Storage_.Tombstone(ScopeTemplate(Context), Record.MemoryId, Now);
    if (const auto* StorageError =
            std::get_if<MemoryStorageError>(&Result)) {
      return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
    }
    ++Tombstoned;
  }
  if (Tombstoned == 0U) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "forget target matched no active memory");
  }
  return std::monostate{};
}

MemoryResult<std::size_t> ConversationMemory::RestoreBackup(
    const AuthorizedMemoryContext& Context, std::chrono::sys_seconds Now) {
  if (!Context.IsComplete()) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "verified principal and scope are required");
  }
  const auto Backup =
      Storage_.RecallBackup(ScopeTemplate(Context), Now);
  if (const auto* StorageError = std::get_if<MemoryStorageError>(&Backup)) {
    return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
  }
  std::size_t Restored = 0;
  for (const StoredMemory& Record :
       std::get<std::vector<StoredMemory>>(Backup)) {
    if (Record.ChannelId != Context.ChannelId ||
        Record.AccountId != Context.AccountId ||
        Record.ConversationId != Context.ConversationId ||
        Record.PrincipalId != Context.PrincipalId) {
      continue;
    }
    const auto* Consent = FindConsent(Record.ConsentId);
    if (Consent == nullptr || !Consent->Active || !Consent->Grant.Granted) {
      continue;
    }
    if (!ConsentMatches(*Consent, Context, Record.Category)) {
      continue;
    }
    const auto Put = Storage_.Put(Record);
    if (const auto* StorageError = std::get_if<MemoryStorageError>(&Put)) {
      if (StorageError->Code == MemoryStorageErrorCode::VersionConflict) {
        continue;
      }
      return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
    }
    ++Restored;
  }
  return Restored;
}

} // namespace EmulativeDialog::Memory
