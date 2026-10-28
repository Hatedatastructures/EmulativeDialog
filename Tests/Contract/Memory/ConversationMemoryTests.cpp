#include "Source/Memory/ConversationMemory.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace EmulativeDialog::Memory;

int Failures = 0;

void Check(bool Condition, std::string_view Name) {
  if (Condition) {
    std::cout << "PASS " << Name << '\n';
  } else {
    ++Failures;
    std::cerr << "FAIL " << Name << '\n';
  }
}

std::chrono::sys_seconds Now() {
  return std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}};
}

std::string ScopeKey(const StoredMemory& Record) {
  return Record.ChannelId + "|" + Record.AccountId + "|" +
         Record.ConversationId + "|" +
         std::to_string(static_cast<int>(Record.ScopeKind)) + "|" +
         Record.PrincipalId;
}

class FakeMemoryStorage final : public MemoryStoragePort {
public:
  bool FailApply{false};
  bool FailList{false};
  bool FailBackup{false};
  std::vector<StoredMemory> BackupRecords;
  std::map<std::string, std::vector<StoredMemory>> Rows;

  MemoryStorageResult<std::monostate> ApplyAtomically(
      const MemoryMutationBatch& Batch) override {
    if (FailApply) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected batch failure"};
    }
    auto Staged = Rows;
    auto& ScopeRows = Staged[ScopeKey(Batch.Scope)];
    for (StoredMemory& Record : ScopeRows) {
      if (std::find(Batch.Tombstones.begin(), Batch.Tombstones.end(),
                    Record.MemoryId) != Batch.Tombstones.end()) {
        Record.Tombstoned = true;
        ++Record.Version;
      }
    }
    for (const StoredMemory& Upsert : Batch.Upserts) {
      bool Replaced = false;
      for (StoredMemory& Existing : ScopeRows) {
        if (Existing.MemoryId != Upsert.MemoryId) {
          continue;
        }
        if (Upsert.Version <= Existing.Version) {
          return MemoryStorageError{
              .Code = MemoryStorageErrorCode::VersionConflict,
              .Detail = "injected version conflict"};
        }
        Existing = Upsert;
        Replaced = true;
        break;
      }
      if (!Replaced) {
        ScopeRows.push_back(Upsert);
      }
    }
    Rows = std::move(Staged);
    return std::monostate{};
  }

  MemoryStorageResult<std::vector<StoredMemory>> ListActive(
      const StoredMemory& ScopeTemplate,
      std::chrono::sys_seconds Current) const override {
    if (FailList) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected list failure"};
    }
    std::vector<StoredMemory> Active;
    const auto Scope = Rows.find(ScopeKey(ScopeTemplate));
    if (Scope == Rows.end()) {
      return Active;
    }
    for (const StoredMemory& Record : Scope->second) {
      if (!Record.Tombstoned &&
          (!Record.ExpiresAt.has_value() || *Record.ExpiresAt > Current)) {
        Active.push_back(Record);
      }
    }
    return Active;
  }

  MemoryStorageResult<std::vector<StoredMemory>> RecallBackup(
      const StoredMemory& ScopeTemplate,
      std::chrono::sys_seconds Current) const override {
    if (FailBackup) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected backup failure"};
    }
    std::vector<StoredMemory> Result;
    for (const StoredMemory& Record : BackupRecords) {
      if (ScopeKey(Record) == ScopeKey(ScopeTemplate) &&
          (!Record.ExpiresAt.has_value() || *Record.ExpiresAt > Current)) {
        Result.push_back(Record);
      }
    }
    return Result;
  }
};

AuthorizedMemoryContext Context(
    ConversationScopeKind Kind = ConversationScopeKind::Direct,
    std::string Principal = "principal-01",
    std::string Conversation = "conversation-01",
    std::uint64_t PolicyVersion = 1) {
  AuthorizedMemoryContext Value;
  Value.ChannelId = "qq";
  Value.AccountId = "account-01";
  Value.ConversationId = std::move(Conversation);
  Value.PrincipalId = std::move(Principal);
  Value.ScopeKind = Kind;
  Value.IdentityPolicyVersion = PolicyVersion;
  Value.IdentityVerified = true;
  return Value;
}

ConsentGrant Grant(MemoryCategory Category,
                   const AuthorizedMemoryContext& Value,
                   std::string Id = "consent-01") {
  ConsentGrant Grant;
  Grant.ConsentId = std::move(Id);
  Grant.GrantorPrincipalId = Value.PrincipalId;
  Grant.ChannelId = Value.ChannelId;
  Grant.AccountId = Value.AccountId;
  Grant.ConversationId = Value.ConversationId;
  Grant.ScopeKind = Value.ScopeKind;
  Grant.Category = Category;
  Grant.SourceMessageId = "fixture-message-consent";
  Grant.GrantedAt = Now();
  Grant.IdentityPolicyVersion = Value.IdentityPolicyVersion;
  if (Category == MemoryCategory::GroupMemory) {
    Grant.AllowedGroupMembers = {Value.PrincipalId};
  }
  return Grant;
}

MemoryWriteRequest Request(std::string Id, MemoryCategory Category,
                           std::string ConsentId = "consent-01") {
  MemoryWriteRequest Request;
  Request.MemoryId = std::move(Id);
  Request.Category = Category;
  Request.SubjectId = "principal-01";
  Request.Content = "Synthetic memory";
  Request.SourceReference = "fixture-message-01";
  Request.ConsentId = std::move(ConsentId);
  Request.CreatedAt = Now() - std::chrono::seconds{100};
  Request.UserConfirmed = true;
  Request.Version = 1;
  return Request;
}

template <typename Value>
const MemoryError* AsError(const MemoryResult<Value>& Result) {
  return std::get_if<MemoryError>(&Result);
}

std::size_t ActiveCount(const FakeMemoryStorage& Storage,
                        const StoredMemory& Scope) {
  const auto It = Storage.Rows.find(ScopeKey(Scope));
  if (It == Storage.Rows.end()) {
    return 0;
  }
  return static_cast<std::size_t>(
      std::count_if(It->second.begin(), It->second.end(),
                    [](const StoredMemory& Record) {
                      return !Record.Tombstoned;
                    }));
}

StoredMemory ScopeOf(const AuthorizedMemoryContext& Value) {
  StoredMemory Scope;
  Scope.ChannelId = Value.ChannelId;
  Scope.AccountId = Value.AccountId;
  Scope.ConversationId = Value.ConversationId;
  Scope.ScopeKind = Value.ScopeKind;
  Scope.PrincipalId = Value.PrincipalId;
  return Scope;
}

void TestIdentitySessionAndPolicy() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  auto Session = Context(ConversationScopeKind::Session);
  Check(AsError(Memory.Write(
            Session, Request("memory-1", MemoryCategory::IdentityPreference),
            Now()))
            ->Code == MemoryErrorCode::SessionScopeVolatile,
        "SessionScope write rejected");
  Check(AsError(Memory.Recall(Session, Now(), 100))->Code ==
            MemoryErrorCode::SessionScopeVolatile,
        "SessionScope recall rejected");
  auto Unverified = Context();
  Unverified.IdentityVerified = false;
  Check(AsError(Memory.Recall(Unverified, Now(), 100))->Code ==
            MemoryErrorCode::UnauthorizedIdentity,
        "unverified identity rejected");
  auto MismatchedPolicy = Context(ConversationScopeKind::Direct,
                                  "principal-01", "conversation-01", 2);
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Context())));
  Check(AsError(Memory.Write(
            MismatchedPolicy,
            Request("memory-2", MemoryCategory::IdentityPreference), Now()))
            ->Code == MemoryErrorCode::ConsentMismatch,
        "identity policy version change invalidates consent");
}

void TestConsentValidationAndProposalBinding() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context();
  auto Invalid = Grant(MemoryCategory::IdentityPreference, Value);
  Invalid.SourceMessageId.clear();
  Check(AsError(Memory.RegisterConsent(Invalid))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires source message");
  auto ProposalGrant = Grant(MemoryCategory::IdentityPreference, Value);
  ProposalGrant.ProposalId = "proposal-a";
  Check(std::holds_alternative<std::monostate>(
            Memory.RegisterConsent(ProposalGrant)),
        "proposal-bound consent registered");
  auto WrongProposal =
      Request("memory-1", MemoryCategory::IdentityPreference);
  WrongProposal.ProposalId = "proposal-b";
  Check(AsError(Memory.Write(Value, WrongProposal, Now()))->Code ==
            MemoryErrorCode::ConsentMismatch,
        "consent A cannot authorize proposal B");
  auto MissingProposal =
      Request("memory-1", MemoryCategory::IdentityPreference);
  Check(AsError(Memory.Write(Value, MissingProposal, Now()))->Code ==
            MemoryErrorCode::ConsentMismatch,
        "proposal-bound consent requires matching proposal");
  auto CorrectProposal =
      Request("memory-1", MemoryCategory::IdentityPreference);
  CorrectProposal.ProposalId = "proposal-a";
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Value, CorrectProposal, Now())),
        "matching proposal writes");
  Check(AsError(Memory.RegisterConsent(ProposalGrant))->Code ==
            MemoryErrorCode::InvalidArgument,
        "duplicate consent rejected");
}

void TestScopeKindAndGroupMembership() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Direct = Context(ConversationScopeKind::Direct);
  const auto Group = Context(ConversationScopeKind::Group);
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Direct, "consent-direct")));
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::GroupMemory, Group, "consent-group")));
  static_cast<void>(Memory.Write(
      Direct,
      Request("memory-direct", MemoryCategory::IdentityPreference,
              "consent-direct"),
      Now()));
  static_cast<void>(Memory.Write(
      Group,
      Request("memory-group", MemoryCategory::GroupMemory, "consent-group"),
      Now()));
  const auto DirectRecall = Memory.Recall(Direct, Now(), 1000);
  const auto GroupRecall = Memory.Recall(Group, Now(), 1000);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            DirectRecall)
            .size() == 1,
        "direct scope reads only direct records");
  const auto& GroupCandidates =
      std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
          GroupRecall);
  Check(GroupCandidates.size() == 1 &&
            GroupCandidates.front().Scope.Kind ==
                EmulativeDialog::Context::ConversationKind::Group,
        "group recall keeps group scope kind");
  auto ExcludedMember =
      Context(ConversationScopeKind::Group, "principal-02");
  ExcludedMember.ConversationId = Group.ConversationId;
  auto ExcludedConsent = Grant(MemoryCategory::GroupMemory, Group,
                               "consent-group-excluded");
  ExcludedConsent.GrantorPrincipalId = "principal-02";
  ExcludedConsent.AllowedGroupMembers = {"principal-01"};
  Check(AsError(Memory.RegisterConsent(ExcludedConsent)) == nullptr,
        "group consent registration accepts member list");
  Check(AsError(Memory.Write(
            ExcludedMember,
            Request("memory-excluded", MemoryCategory::GroupMemory,
                    "consent-group-excluded"),
            Now()))
            ->Code == MemoryErrorCode::ConsentMismatch,
        "group consent denies unlisted member");
}

void TestRevocationAtomicityAndScope() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context();
  auto OtherConversation =
      Context(ConversationScopeKind::Direct, "principal-01",
              "conversation-02");
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-1", MemoryCategory::IdentityPreference), Now()));
  Check(AsError(Memory.RevokeConsent(OtherConversation, "consent-01", Now()))
            ->Code == MemoryErrorCode::UnauthorizedIdentity,
        "revoke requires exact conversation scope");
  Storage.FailApply = true;
  Check(AsError(Memory.RevokeConsent(Value, "consent-01", Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "revoke batch failure reported");
  Check(Memory.IsConsentActive("consent-01"),
        "consent remains active after failed tombstone batch");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 1,
        "record remains active after failed revoke");
  Storage.FailApply = false;
  Check(std::holds_alternative<std::monostate>(
            Memory.RevokeConsent(Value, "consent-01", Now())),
        "exact scope revoke succeeds");
  Check(!Memory.IsConsentActive("consent-01"), "consent revoked after batch");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 0,
        "affected records tombstoned");
}

void TestForgetAndExpiry() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context();
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::GroupMemory, Value, "consent-group")));
  static_cast<void>(Memory.Write(
      Value, Request("memory-1", MemoryCategory::IdentityPreference), Now()));
  static_cast<void>(Memory.Write(
      Value, Request("memory-2", MemoryCategory::GroupMemory, "consent-group"),
      Now()));
  ForgetRequest None;
  Check(AsError(Memory.Forget(Value, None, Now()))->Code ==
            MemoryErrorCode::ForgetScopeRequired,
        "broad forget rejected");
  ForgetRequest Missing;
  Missing.MemoryId = "memory-missing";
  Check(AsError(Memory.Forget(Value, Missing, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "unmatched forget rejected");
  ForgetRequest Category;
  Category.Category = MemoryCategory::GroupMemory;
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Value, Category, Now())),
        "category forget succeeds");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 1,
        "category forget tombstones matching record");
  auto Expired = Request("memory-3", MemoryCategory::IdentityPreference);
  Expired.ExpiresAt = Now() - std::chrono::seconds{1};
  Check(AsError(Memory.Write(Value, Expired, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "already-expired write rejected");
}

void TestCapacityAndWriteAtomicity() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage, 2);
  const auto Value = Context();
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  for (int Index = 0; Index < 2; ++Index) {
    auto Write =
        Request("memory-" + std::to_string(Index),
                MemoryCategory::IdentityPreference);
    Write.CreatedAt = Now() - std::chrono::seconds{100 - Index};
    static_cast<void>(Memory.Write(Value, Write, Now()));
  }
  Storage.FailApply = true;
  auto Third = Request("memory-2", MemoryCategory::IdentityPreference);
  Third.CreatedAt = Now();
  Check(AsError(Memory.Write(Value, Third, Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "capacity write batch failure reported");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 2,
        "failed write preserves eviction candidate");
  Storage.FailApply = false;
  static_cast<void>(Memory.Write(Value, Third, Now()));
  Check(ActiveCount(Storage, ScopeOf(Value)) == 2,
        "successful capacity write keeps bounded count");
  auto Conflict = Request("memory-0", MemoryCategory::IdentityPreference);
  Conflict.Version = 1;
  Check(AsError(Memory.Write(Value, Conflict, Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "version conflict maps to storage failure");
}

void TestRecallPriorityAndSummaryMapping() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context(ConversationScopeKind::Group);
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::ConversationSummary, Value, "consent-summary")));
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::ActiveConversationContext, Value,
            "consent-task")));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-summary", MemoryCategory::ConversationSummary,
              "consent-summary"),
      Now()));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-task", MemoryCategory::ActiveConversationContext,
              "consent-task"),
      Now()));
  const auto Recall = Memory.Recall(Value, Now(), 1000);
  const auto& Candidates =
      std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
          Recall);
  Check(Candidates.size() == 2 &&
            Candidates.front().Kind ==
                EmulativeDialog::Context::ContextItemKind::
                    ConversationHistory,
        "tier 3 summary recalls before tier 4 active task");
  const auto Summary = std::find_if(
      Candidates.begin(), Candidates.end(),
      [](const auto& Candidate) {
        return Candidate.Id.Value == "memory-summary";
      });
  Check(Summary != Candidates.end() &&
            Summary->Kind ==
                EmulativeDialog::Context::ContextItemKind::
                    ConversationHistory &&
            Summary->Priority ==
                EmulativeDialog::Context::ContextPriorityTier::Tier3,
        "summary maps to conversation history tier 3");
}

void TestRestoreBackupBoundaries() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage, 2);
  const auto Value = Context();
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  StoredMemory Base = ScopeOf(Value);
  Base.MemoryId = "memory-restore";
  Base.Category = MemoryCategory::IdentityPreference;
  Base.SubjectId = "principal-01";
  Base.Content = "Synthetic memory";
  Base.SourceReference = "fixture-message-01";
  Base.ConsentId = "consent-01";
  Base.CreatedAt = Now() - std::chrono::seconds{100};
  Base.Version = 1;
  Storage.BackupRecords = {Base};
  const auto Restored = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Restored) &&
            std::get<std::size_t>(Restored) == 1,
        "eligible backup restored");
  Storage.BackupRecords = {Base};
  const auto Conflict = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Conflict) &&
            std::get<std::size_t>(Conflict) == 0,
        "version conflict backup skipped");
  auto Tombstoned = Base;
  Tombstoned.MemoryId = "memory-tombstoned";
  Tombstoned.Tombstoned = true;
  Storage.BackupRecords = {Tombstoned};
  const auto NoResurrection = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(NoResurrection) &&
            std::get<std::size_t>(NoResurrection) == 0,
        "tombstoned backup never resurrects");
  auto Sensitive = Base;
  Sensitive.MemoryId = "memory-sensitive";
  Sensitive.Content = "api_key=synthetic";
  Storage.BackupRecords = {Sensitive};
  const auto Denied = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Denied) &&
            std::get<std::size_t>(Denied) == 0,
        "sensitive backup denied");
  Storage.BackupRecords = {Base};
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-capacity", MemoryCategory::IdentityPreference), Now()));
  const auto Full = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Full) &&
            std::get<std::size_t>(Full) == 0,
        "backup restore respects capacity");
}

void TestRestartBoundaryAndStorageFailures() {
  FakeMemoryStorage Storage;
  const auto Value = Context();
  {
    ConversationMemory Memory(Storage);
    static_cast<void>(Memory.RegisterConsent(
        Grant(MemoryCategory::IdentityPreference, Value)));
    static_cast<void>(Memory.Write(
        Value,
        Request("memory-durable", MemoryCategory::IdentityPreference), Now()));
  }
  ConversationMemory Restarted(Storage);
  static_cast<void>(Restarted.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  const auto Recall = Restarted.Recall(Value, Now(), 1000);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Recall)
            .size() == 1,
        "durable scope recovers after restart");
  Storage.FailList = true;
  Check(AsError(Restarted.Recall(Value, Now(), 1000))->Code ==
            MemoryErrorCode::StorageFailure,
        "list failure mapped");
  Storage.FailList = false;
  Storage.FailBackup = true;
  Check(AsError(Restarted.RestoreBackup(Value, Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "backup failure mapped");
}

void TestAdditionalReviewScenarios() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage, 3);
  const auto Value = Context();
  auto OtherPrincipal =
      Context(ConversationScopeKind::Direct, "principal-02");
  OtherPrincipal.ConversationId = Value.ConversationId;
  Check(AsError(Memory.RevokeConsent(OtherPrincipal, "consent-01", Now()))
            ->Code == MemoryErrorCode::UnauthorizedIdentity,
        "revoke denies foreign principal on same conversation");
  auto EmptyGrantor = Grant(MemoryCategory::IdentityPreference, Value);
  EmptyGrantor.GrantorPrincipalId.clear();
  Check(AsError(Memory.RegisterConsent(EmptyGrantor))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires grantor principal");
  auto EmptyPolicy = Grant(MemoryCategory::IdentityPreference, Value);
  EmptyPolicy.IdentityPolicyVersion = 0;
  Check(AsError(Memory.RegisterConsent(EmptyPolicy))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires identity policy version");
  auto EmptyConversation = Grant(MemoryCategory::IdentityPreference, Value);
  EmptyConversation.ConversationId.clear();
  Check(AsError(Memory.RegisterConsent(EmptyConversation))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires exact conversation scope");
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  auto CardSensitive =
      Request("memory-sensitive", MemoryCategory::IdentityPreference);
  CardSensitive.Content = "card_number=synthetic";
  Check(AsError(Memory.Write(Value, CardSensitive, Now()))->Code ==
            MemoryErrorCode::SensitiveContent,
        "payment marker rejected");
  auto CvvSensitive =
      Request("memory-sensitive", MemoryCategory::IdentityPreference);
  CvvSensitive.Content = "cvv=synthetic";
  Check(AsError(Memory.Write(Value, CvvSensitive, Now()))->Code ==
            MemoryErrorCode::SensitiveContent,
        "cvv marker rejected");
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-a", MemoryCategory::IdentityPreference), Now()));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-b", MemoryCategory::IdentityPreference), Now()));
  ForgetRequest Precise;
  Precise.MemoryId = "memory-a";
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Value, Precise, Now())),
        "precise forget succeeds");
  ForgetRequest WholeScope;
  WholeScope.EntireAuthorizedScope = true;
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Value, WholeScope, Now())),
        "entire authorized scope forget succeeds");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 0,
        "scope forget tombstones all matching records");
  auto Session = Context(ConversationScopeKind::Session);
  Check(AsError(Memory.Forget(Session, Precise, Now()))->Code ==
            MemoryErrorCode::SessionScopeVolatile,
        "SessionScope forget rejected");
  Check(AsError(Memory.RevokeConsent(Value, "missing-consent", Now()))->Code ==
            MemoryErrorCode::UnauthorizedIdentity,
        "unknown consent revoke denied");
  auto FutureExpiry =
      Request("memory-expiry", MemoryCategory::IdentityPreference);
  FutureExpiry.ExpiresAt = Now() + std::chrono::seconds{5};
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Value, FutureExpiry, Now())),
        "future expiry accepted");
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Memory.Recall(Value, Now() + std::chrono::seconds{6}, 1000))
            .empty(),
        "expired recall filtered");
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Memory.Recall(Value, Now(), 1))
            .empty(),
        "tiny token budget yields no candidates");
}

void TestSensitiveMarkersAreRejectedAcrossPersistedFields() {
  const auto BaseContext = Context();
  auto CheckContextField = [&](std::string_view Name,
                               AuthorizedMemoryContext Candidate) {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    const auto Result = Memory.Write(
        Candidate,
        Request("memory-safe", MemoryCategory::IdentityPreference), Now());
    Check(AsError(Result) != nullptr &&
              AsError(Result)->Code == MemoryErrorCode::SensitiveContent,
          Name);
    Check(Storage.Rows.empty(), "sensitive scope does not touch storage");
  };

  auto SensitiveContext = BaseContext;
  SensitiveContext.ChannelId = "qq ACCOUNT_TOKEN = synthetic";
  CheckContextField("case-insensitive scope marker rejected", SensitiveContext);
  SensitiveContext = BaseContext;
  SensitiveContext.AccountId = "account CLIENT_SECRET=synthetic";
  CheckContextField("account secret marker rejected", SensitiveContext);
  SensitiveContext = BaseContext;
  SensitiveContext.ConversationId = "conversation PRIVATE KEY=synthetic";
  CheckContextField("conversation secret marker rejected", SensitiveContext);
  SensitiveContext = BaseContext;
  SensitiveContext.PrincipalId = "principal AUTHORIZATION=Bearer synthetic";
  CheckContextField("principal credential marker rejected", SensitiveContext);

  auto CheckConsentField = [&](std::string_view Name, ConsentGrant Candidate) {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    const auto Result = Memory.RegisterConsent(Candidate);
    Check(AsError(Result) != nullptr &&
              AsError(Result)->Code == MemoryErrorCode::SensitiveContent,
          Name);
    Check(Memory.IsConsentActive("consent-01") == false,
          "sensitive consent is not registered");
  };

  auto SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ConsentId = "consent API_KEY = synthetic";
  CheckConsentField("consent identifier credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.GrantorPrincipalId = "principal PASSWORD=synthetic";
  CheckConsentField("consent grantor credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ChannelId = "channel AWS_SECRET_ACCESS_KEY=synthetic";
  CheckConsentField("consent channel credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.AccountId = "account payment token = synthetic";
  CheckConsentField("consent account payment marker rejected", SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ConversationId = "conversation card_number=synthetic";
  CheckConsentField("consent conversation payment marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.SourceMessageId = "message Bearer synthetic";
  CheckConsentField("consent source credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ProposalId = "proposal CVV = synthetic";
  CheckConsentField("consent proposal payment marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::GroupMemory,
                         Context(ConversationScopeKind::Group));
  SensitiveGrant.AllowedGroupMembers = {"principal-01", "PAN=synthetic"};
  CheckConsentField("consent member payment marker rejected", SensitiveGrant);

  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(
            Grant(MemoryCategory::IdentityPreference, BaseContext))),
        "safe consent registers before field checks");
  auto CheckRequestField = [&](std::string_view Name,
                               MemoryWriteRequest Candidate) {
    const auto Result = Memory.Write(BaseContext, Candidate, Now());
    Check(AsError(Result) != nullptr &&
              AsError(Result)->Code == MemoryErrorCode::SensitiveContent,
          Name);
    Check(Storage.Rows.empty(), "sensitive record does not touch storage");
  };

  auto SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.MemoryId = "memory password = synthetic";
  CheckRequestField("memory id credential marker rejected", SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.SubjectId = "subject client_secret=synthetic";
  CheckRequestField("subject credential marker rejected", SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.Content = "PRIVATE KEY = synthetic";
  CheckRequestField("content marker is case-insensitive and whitespace-safe",
                    SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.SourceReference = "message credit_card=synthetic";
  CheckRequestField("source reference payment marker rejected",
                    SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.ConsentId = "consent access_token=synthetic";
  CheckRequestField("request consent credential marker rejected",
                    SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.ProposalId = "proposal SSN=synthetic";
  CheckRequestField("request proposal personal-data marker rejected",
                    SensitiveRequest);

  auto CheckBackupField = [&](std::string_view Name,
                              StoredMemory Candidate) {
    FakeMemoryStorage BackupStorage;
    ConversationMemory BackupMemory(BackupStorage);
    Check(std::holds_alternative<std::monostate>(BackupMemory.RegisterConsent(
              Grant(MemoryCategory::IdentityPreference, BaseContext))),
          "safe consent registers before backup field check");
    BackupStorage.BackupRecords = {std::move(Candidate)};
    const auto Result = BackupMemory.RestoreBackup(BaseContext, Now());
    Check(std::holds_alternative<std::size_t>(Result) &&
              std::get<std::size_t>(Result) == 0U,
          Name);
    Check(BackupStorage.Rows.empty(), "sensitive backup does not persist");
  };

  StoredMemory Backup = ScopeOf(BaseContext);
  Backup.MemoryId = "memory-safe";
  Backup.Category = MemoryCategory::IdentityPreference;
  Backup.SubjectId = "principal-01";
  Backup.Content = "Synthetic memory";
  Backup.SourceReference = "fixture-message-01";
  Backup.ConsentId = "consent-01";
  Backup.CreatedAt = Now();
  Backup.Version = 1;
  auto SensitiveBackup = Backup;
  SensitiveBackup.MemoryId = "memory API_KEY = synthetic";
  CheckBackupField("backup memory identifier marker rejected",
                   SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.SubjectId = "subject CLIENT_SECRET=synthetic";
  CheckBackupField("backup subject marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.Content = "PASSWORD = synthetic";
  CheckBackupField("backup content marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.SourceReference = "source Bearer synthetic";
  CheckBackupField("backup source marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.ConsentId = "consent AWS_SECRET_ACCESS_KEY=synthetic";
  CheckBackupField("backup consent marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.ProposalId = "proposal PAYMENT TOKEN = synthetic";
  CheckBackupField("backup proposal marker rejected", SensitiveBackup);

  FakeMemoryStorage SensitiveScopeStorage;
  ConversationMemory SensitiveScopeMemory(SensitiveScopeStorage);
  auto SensitiveScope = BaseContext;
  SensitiveScope.ConversationId = "conversation CARD_NUMBER=synthetic";
  const auto SensitiveScopeResult =
      SensitiveScopeMemory.RestoreBackup(SensitiveScope, Now());
  Check(AsError(SensitiveScopeResult) != nullptr &&
            AsError(SensitiveScopeResult)->Code ==
                MemoryErrorCode::SensitiveContent,
        "restore rejects sensitive scope identifiers");
  Check(SensitiveScopeStorage.Rows.empty(),
        "sensitive restore scope does not touch storage");
}

} // namespace

int main() {
  TestIdentitySessionAndPolicy();
  TestConsentValidationAndProposalBinding();
  TestScopeKindAndGroupMembership();
  TestRevocationAtomicityAndScope();
  TestForgetAndExpiry();
  TestCapacityAndWriteAtomicity();
  TestRecallPriorityAndSummaryMapping();
  TestRestoreBackupBoundaries();
  TestRestartBoundaryAndStorageFailures();
  TestAdditionalReviewScenarios();
  TestSensitiveMarkersAreRejectedAcrossPersistedFields();
  if (Failures != 0) {
    std::cerr << Failures << " checks failed\n";
    return 1;
  }
  std::cout << "All ConversationMemory checks passed\n";
  return 0;
}
