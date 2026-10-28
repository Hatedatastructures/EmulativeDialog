#include "Source/Memory/ConversationMemory.hpp"

#include <algorithm>
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
    "BEGIN PRIVATE KEY", "BEGIN RSA PRIVATE KEY", "password=",
    "access_token=", "client_secret=", "api_key=", "apikey=",
    "aws_secret_access_key=", "payment_token=", "credit_card=", "card_number=",
    "cvv=", "pan=", "Bearer "};

ContextItemKind ToContextKind(MemoryCategory Category) {
  switch (Category) {
  case MemoryCategory::ConversationSummary:
    return ContextItemKind::ConversationHistory;
  case MemoryCategory::IdentityPreference:
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
  case MemoryCategory::ConversationSummary:
    return ContextPriorityTier::Tier3;
  case MemoryCategory::ActiveConversationContext:
    return ContextPriorityTier::Tier4;
  case MemoryCategory::IdentityPreference:
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

ConversationKind ToContextConversationKind(ConversationScopeKind Kind) {
  return Kind == ConversationScopeKind::Group ? ConversationKind::Group
                                              : ConversationKind::Direct;
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
    : Storage_(Storage),
      PerScopeCapacity_(PerScopeCapacity == 0U ? 1U : PerScopeCapacity) {}

MemoryError ConversationMemory::Error(MemoryErrorCode Code,
                                      std::string Detail) {
  return MemoryError{.Code = Code, .Detail = std::move(Detail)};
}

bool ConversationMemory::ContainsSensitiveContent(std::string_view Text) {
  for (const auto Marker : SensitiveMarkers) {
    if (Text.find(Marker) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

std::optional<MemoryError> ConversationMemory::Authorize(
    const AuthorizedMemoryContext& Context,
    const MemoryWriteRequest& Request) {
  if (!Context.IsComplete()) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "verified principal, scope, and policy version are required");
  }
  if (Context.ScopeKind == ConversationScopeKind::Session) {
    return Error(MemoryErrorCode::SessionScopeVolatile,
                 "SessionScope is volatile and cannot be persisted");
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
  if (ContainsSensitiveContent(Request.Content)) {
    return Error(MemoryErrorCode::SensitiveContent,
                 "memory content contains a forbidden sensitive marker");
  }
  return std::nullopt;
}

bool ConversationMemory::SameScope(const ConsentGrant& Grant,
                                   const AuthorizedMemoryContext& Context) {
  return Grant.ChannelId == Context.ChannelId &&
         Grant.AccountId == Context.AccountId &&
         Grant.ConversationId == Context.ConversationId &&
         Grant.ScopeKind == Context.ScopeKind &&
         Grant.GrantorPrincipalId == Context.PrincipalId;
}

StoredMemory ConversationMemory::ScopeTemplate(
    const AuthorizedMemoryContext& Context) const {
  StoredMemory Template;
  Template.ChannelId = Context.ChannelId;
  Template.AccountId = Context.AccountId;
  Template.ConversationId = Context.ConversationId;
  Template.ScopeKind = Context.ScopeKind;
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

const ConversationMemory::ActiveConsent* ConversationMemory::FindExactConsent(
    const AuthorizedMemoryContext& Context,
    std::string_view ConsentId) const {
  for (const ActiveConsent& Consent : Consents_) {
    if (Consent.Grant.ConsentId == ConsentId &&
        SameScope(Consent.Grant, Context)) {
      return &Consent;
    }
  }
  return nullptr;
}

bool ConversationMemory::ConsentMatches(
    const ActiveConsent& Consent, const AuthorizedMemoryContext& Context,
    MemoryCategory Category,
    const std::optional<std::string>& ProposalId) const {
  if (!Consent.Active || !Consent.Grant.Granted ||
      !SameScope(Consent.Grant, Context) ||
      Consent.Grant.Category != Category ||
      Consent.Grant.IdentityPolicyVersion != Context.IdentityPolicyVersion) {
    return false;
  }
  if (Consent.Grant.ProposalId.has_value() &&
      (!ProposalId.has_value() || *ProposalId != *Consent.Grant.ProposalId)) {
    return false;
  }
  if (Category == MemoryCategory::GroupMemory) {
    const auto& Members = Consent.Grant.AllowedGroupMembers;
    if (Members.empty() ||
        std::find(Members.begin(), Members.end(), Context.PrincipalId) ==
            Members.end()) {
      return false;
    }
  }
  return true;
}

MemoryResult<std::monostate> ConversationMemory::RegisterConsent(
    const ConsentGrant& Grant) {
  if (Grant.ConsentId.empty() || Grant.GrantorPrincipalId.empty() ||
      Grant.ChannelId.empty() || Grant.AccountId.empty() ||
      Grant.ConversationId.empty() || Grant.SourceMessageId.empty() ||
      Grant.IdentityPolicyVersion == 0U) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "consent requires id, verified grantor, exact scope, source "
                 "message, and policy version");
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
  const auto* Consent = FindExactConsent(Context, ConsentId);
  if (Consent == nullptr) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "revocation requires the exact grant scope and principal");
  }
  const auto Records = ListActive(Context, Now);
  if (const auto* RecordsError = std::get_if<MemoryError>(&Records)) {
    return *RecordsError;
  }
  MemoryMutationBatch Batch;
  Batch.Scope = ScopeTemplate(Context);
  for (const StoredMemory& Record :
       std::get<std::vector<StoredMemory>>(Records)) {
    if (Record.ConsentId == ConsentId) {
      Batch.Tombstones.push_back(Record.MemoryId);
    }
  }
  if (!Batch.Tombstones.empty()) {
    const auto Applied = Storage_.ApplyAtomically(Batch);
    if (const auto* StorageError =
            std::get_if<MemoryStorageError>(&Applied)) {
      return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
    }
  }
  for (ActiveConsent& Entry : Consents_) {
    if (Entry.Grant.ConsentId == ConsentId) {
      Entry.Active = false;
      break;
    }
  }
  return std::monostate{};
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
  if (!ConsentMatches(*Consent, Context, Request.Category,
                      Request.ProposalId)) {
    return Error(MemoryErrorCode::ConsentMismatch,
                 "consent does not cover this principal, scope, category, "
                 "proposal, or policy version");
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

  StoredMemory Record = ScopeTemplate(Context);
  Record.MemoryId = Request.MemoryId;
  Record.Category = Request.Category;
  Record.SubjectId = Request.SubjectId;
  Record.Content = Request.Content;
  Record.SourceReference = Request.SourceReference;
  Record.ConsentId = Request.ConsentId;
  Record.ProposalId = Request.ProposalId;
  Record.CreatedAt =
      Request.CreatedAt == std::chrono::sys_seconds{} ? Now
                                                      : Request.CreatedAt;
  if (Record.CreatedAt > Now) {
    Record.CreatedAt = Now;
  }
  Record.ExpiresAt = Request.ExpiresAt;
  Record.Provenance = Request.Provenance;
  Record.UserConfirmed = Request.UserConfirmed;
  Record.Version = Request.Version;

  MemoryMutationBatch Batch;
  Batch.Scope = ScopeTemplate(Context);
  if (Active.size() >= PerScopeCapacity_) {
    std::sort(Active.begin(), Active.end(),
              [](const StoredMemory& Left, const StoredMemory& Right) {
                if (Left.CreatedAt != Right.CreatedAt) {
                  return Left.CreatedAt < Right.CreatedAt;
                }
                return Left.MemoryId < Right.MemoryId;
              });
    Batch.Tombstones.push_back(Active.front().MemoryId);
  }
  Batch.Upserts.push_back(std::move(Record));
  const auto Applied = Storage_.ApplyAtomically(Batch);
  if (const auto* StorageError = std::get_if<MemoryStorageError>(&Applied)) {
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
  if (Context.ScopeKind == ConversationScopeKind::Session) {
    return Error(MemoryErrorCode::SessionScopeVolatile,
                 "SessionScope is volatile and cannot be listed");
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
      std::remove_if(
          Records.begin(), Records.end(),
          [this, &Context](const StoredMemory& Record) {
            if (Record.ScopeKind != Context.ScopeKind) {
              return true;
            }
            const auto* Consent = FindConsent(Record.ConsentId);
            return Consent == nullptr ||
                   !ConsentMatches(*Consent, Context, Record.Category,
                                   Record.ProposalId);
          }),
      Records.end());
  std::sort(Records.begin(), Records.end(),
            [](const StoredMemory& Left, const StoredMemory& Right) {
              if (ToTier(Left.Category) != ToTier(Right.Category)) {
                return ToTier(Left.Category) < ToTier(Right.Category);
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
        .Kind = ToContextConversationKind(Context.ScopeKind),
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
  if (Context.ScopeKind == ConversationScopeKind::Session) {
    return Error(MemoryErrorCode::SessionScopeVolatile,
                 "SessionScope is volatile and cannot be persisted");
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
  MemoryMutationBatch Batch;
  Batch.Scope = ScopeTemplate(Context);
  for (const StoredMemory& Record :
       std::get<std::vector<StoredMemory>>(Listed)) {
    if (Request.MemoryId.has_value() &&
        Record.MemoryId != *Request.MemoryId) {
      continue;
    }
    if (Request.Category.has_value() &&
        Record.Category != *Request.Category) {
      continue;
    }
    Batch.Tombstones.push_back(Record.MemoryId);
  }
  if (Batch.Tombstones.empty()) {
    return Error(MemoryErrorCode::InvalidArgument,
                 "forget target matched no active memory");
  }
  const auto Applied = Storage_.ApplyAtomically(Batch);
  if (const auto* StorageError = std::get_if<MemoryStorageError>(&Applied)) {
    return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
  }
  return std::monostate{};
}

MemoryResult<std::size_t> ConversationMemory::RestoreBackup(
    const AuthorizedMemoryContext& Context, std::chrono::sys_seconds Now) {
  if (!Context.IsComplete()) {
    return Error(MemoryErrorCode::UnauthorizedIdentity,
                 "verified principal and scope are required");
  }
  if (Context.ScopeKind == ConversationScopeKind::Session) {
    return Error(MemoryErrorCode::SessionScopeVolatile,
                 "SessionScope is volatile and cannot be restored");
  }
  const auto Existing = ListActive(Context, Now);
  if (const auto* ExistingError = std::get_if<MemoryError>(&Existing)) {
    return *ExistingError;
  }
  std::size_t ActiveCount =
      std::get<std::vector<StoredMemory>>(Existing).size();
  const auto Backup = Storage_.RecallBackup(ScopeTemplate(Context), Now);
  if (const auto* StorageError = std::get_if<MemoryStorageError>(&Backup)) {
    return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
  }
  std::size_t Restored = 0;
  for (const StoredMemory& Record :
       std::get<std::vector<StoredMemory>>(Backup)) {
    if (Record.Tombstoned || Record.ScopeKind != Context.ScopeKind ||
        Record.ChannelId != Context.ChannelId ||
        Record.AccountId != Context.AccountId ||
        Record.ConversationId != Context.ConversationId ||
        Record.PrincipalId != Context.PrincipalId ||
        ContainsSensitiveContent(Record.Content) ||
        ActiveCount >= PerScopeCapacity_) {
      continue;
    }
    const auto* Consent = FindConsent(Record.ConsentId);
    if (Consent == nullptr ||
        !ConsentMatches(*Consent, Context, Record.Category,
                        Record.ProposalId)) {
      continue;
    }
    MemoryMutationBatch Batch;
    Batch.Scope = ScopeTemplate(Context);
    Batch.Upserts.push_back(Record);
    const auto Applied = Storage_.ApplyAtomically(Batch);
    if (const auto* StorageError =
            std::get_if<MemoryStorageError>(&Applied)) {
      if (StorageError->Code == MemoryStorageErrorCode::VersionConflict) {
        continue;
      }
      return Error(MemoryErrorCode::StorageFailure, StorageError->Detail);
    }
    ++Restored;
    ++ActiveCount;
  }
  return Restored;
}

} // namespace EmulativeDialog::Memory
