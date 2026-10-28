#include "Source/Memory/ConversationMemory.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
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
         Record.ConversationId + "|" + Record.PrincipalId;
}

class FakeMemoryStorage final : public MemoryStoragePort {
public:
  bool FailPut{false};
  bool FailList{false};
  bool FailTombstone{false};
  bool FailBackup{false};
  std::vector<StoredMemory> BackupRecords;
  std::map<std::string, std::vector<StoredMemory>> Rows;

  MemoryStorageResult<std::monostate> Put(
      const StoredMemory& Record) override {
    if (FailPut) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected put failure"};
    }
    auto& ScopeRows = Rows[ScopeKey(Record)];
    for (StoredMemory& Existing : ScopeRows) {
      if (Existing.MemoryId != Record.MemoryId) {
        continue;
      }
      if (Record.Version <= Existing.Version) {
        return MemoryStorageError{
            .Code = MemoryStorageErrorCode::VersionConflict,
            .Detail = "injected version conflict"};
      }
      Existing = Record;
      return std::monostate{};
    }
    ScopeRows.push_back(Record);
    return std::monostate{};
  }

  MemoryStorageResult<StoredMemory> GetActive(
      const StoredMemory& ScopeTemplate, std::string_view MemoryId,
      std::chrono::sys_seconds Current) const override {
    const auto Scope = Rows.find(ScopeKey(ScopeTemplate));
    if (Scope == Rows.end()) {
      return Missing();
    }
    for (const StoredMemory& Record : Scope->second) {
      if (Record.MemoryId == MemoryId && !Record.Tombstoned &&
          (!Record.ExpiresAt.has_value() || *Record.ExpiresAt > Current)) {
        return Record;
      }
    }
    return Missing();
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

  MemoryStorageResult<std::monostate> Tombstone(
      const StoredMemory& ScopeTemplate, std::string_view MemoryId,
      std::chrono::sys_seconds) override {
    if (FailTombstone) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected tombstone failure"};
    }
    auto Scope = Rows.find(ScopeKey(ScopeTemplate));
    if (Scope == Rows.end()) {
      return NotFound();
    }
    for (StoredMemory& Record : Scope->second) {
      if (Record.MemoryId == MemoryId && !Record.Tombstoned) {
        Record.Tombstoned = true;
        ++Record.Version;
        return std::monostate{};
      }
    }
    return NotFound();
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
      if (ScopeKey(Record) == ScopeKey(ScopeTemplate) && !Record.Tombstoned &&
          (!Record.ExpiresAt.has_value() || *Record.ExpiresAt > Current)) {
        Result.push_back(Record);
      }
    }
    return Result;
  }

private:
  static MemoryStorageError Missing() {
    return MemoryStorageError{.Code = MemoryStorageErrorCode::NotFound,
                              .Detail = "missing"};
  }
  static MemoryStorageError NotFound() {
    return MemoryStorageError{.Code = MemoryStorageErrorCode::NotFound,
                              .Detail = "not found"};
  }
};

AuthorizedMemoryContext AliveContext() {
  AuthorizedMemoryContext Context;
  Context.ChannelId = "qq";
  Context.AccountId = "account-01";
  Context.ConversationId = "direct-01";
  Context.PrincipalId = "principal-01";
  Context.IdentityPolicyVersion = 1;
  Context.IdentityVerified = true;
  return Context;
}

AuthorizedMemoryContext OtherContext() {
  auto Context = AliveContext();
  Context.PrincipalId = "principal-02";
  return Context;
}

ConsentGrant Consent(MemoryCategory Category,
                     const AuthorizedMemoryContext& Context,
                     std::string Id = "consent-01") {
  ConsentGrant Grant;
  Grant.ConsentId = std::move(Id);
  Grant.GrantorPrincipalId = Context.PrincipalId;
  Grant.ChannelId = Context.ChannelId;
  Grant.AccountId = Context.AccountId;
  Grant.ConversationId = Context.ConversationId;
  Grant.Category = Category;
  Grant.Granted = true;
  return Grant;
}

MemoryWriteRequest WriteRequest(std::string Id, MemoryCategory Category,
                                std::string ConsentId = "consent-01") {
  MemoryWriteRequest Request;
  Request.MemoryId = std::move(Id);
  Request.Category = Category;
  Request.SubjectId = "principal-01";
  Request.Content = "Synthetic preference alpha";
  Request.SourceReference = "fixture-message-01";
  Request.ConsentId = std::move(ConsentId);
  Request.CreatedAt = Now() - std::chrono::seconds{600};
  Request.Provenance = MemoryProvenance::UserExplicit;
  Request.UserConfirmed = true;
  Request.Version = 1;
  return Request;
}

template <typename Value>
const MemoryError* AsMemoryError(const MemoryResult<Value>& Result) {
  return std::get_if<MemoryError>(&Result);
}

void TestIdentityAndArgumentGates() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  auto Incomplete = AliveContext();
  Incomplete.IdentityVerified = false;
  Check(AsMemoryError(Memory.Write(Incomplete, WriteRequest(
                                                 "memory-1",
                                                 MemoryCategory::
                                                     IdentityPreference),
                                     Now()))->Code ==
            MemoryErrorCode::UnauthorizedIdentity,
        "unverified identity denied");
  Check(AsMemoryError(Memory.Recall(Incomplete, Now(), 100))->Code ==
            MemoryErrorCode::UnauthorizedIdentity,
        "unverified recall denied");
  auto Invalid = WriteRequest("", MemoryCategory::IdentityPreference);
  Check(AsMemoryError(Memory.Write(AliveContext(), Invalid, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "empty memory id denied");
  auto Sensitive = WriteRequest("memory-1",
                                MemoryCategory::IdentityPreference);
  Sensitive.Content = "password=synthetic";
  Check(AsMemoryError(Memory.Write(AliveContext(), Sensitive, Now()))->Code ==
            MemoryErrorCode::SensitiveContent,
        "sensitive marker denied");
  auto Unconfirmed = WriteRequest("memory-2",
                                  MemoryCategory::IdentityPreference);
  Unconfirmed.Provenance = MemoryProvenance::UserConfirmed;
  Unconfirmed.UserConfirmed = false;
  Check(AsMemoryError(Memory.Write(AliveContext(), Unconfirmed, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "unconfirmed provenance denied");
}

void TestConsentLifecycle() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Context = AliveContext();
  Check(AsMemoryError(Memory.Write(
                          Context,
                          WriteRequest("memory-1",
                                       MemoryCategory::IdentityPreference),
                          Now()))
            ->Code == MemoryErrorCode::ConsentRequired,
        "write without consent denied");
  Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(
            Consent(MemoryCategory::IdentityPreference, Context))),
        "consent registered");
  Check(AsMemoryError(Memory.RegisterConsent(
                          Consent(MemoryCategory::IdentityPreference,
                                  Context)))
            ->Code == MemoryErrorCode::InvalidArgument,
        "duplicate consent rejected");
  Check(AsMemoryError(Memory.RegisterConsent(ConsentGrant{}))->Code ==
            MemoryErrorCode::InvalidArgument,
        "invalid consent rejected");
  auto WrongCategory = WriteRequest("memory-2",
                                    MemoryCategory::GroupMemory);
  Check(AsMemoryError(Memory.Write(Context, WrongCategory, Now()))->Code ==
            MemoryErrorCode::ConsentMismatch,
        "consent category mismatch denied");
  Check(std::holds_alternative<std::monostate>(Memory.Write(
            Context,
            WriteRequest("memory-1", MemoryCategory::IdentityPreference),
            Now())),
        "authorized write accepted");
  Check(Storage.Rows.size() == 1, "authorized write persisted");
  const auto Recall = Memory.Recall(Context, Now(), 1000);
  Check(std::holds_alternative<std::vector<EmulativeDialog::Context::
                                               ContextCandidate>>(Recall) &&
            std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
                Recall)
                    .size() == 1,
        "authorized recall returns candidate");
  Check(AsMemoryError(Memory.RevokeConsent(OtherContext(), "consent-01",
                                           Now()))
            ->Code == MemoryErrorCode::UnauthorizedIdentity,
        "foreign principal cannot revoke");
  Check(std::holds_alternative<std::monostate>(Memory.RevokeConsent(
            Context, "consent-01", Now() + std::chrono::seconds{1})),
        "grantor revokes consent");
  const auto AfterRevoke =
      Memory.Recall(Context, Now() + std::chrono::seconds{2}, 1000);
  Check(std::holds_alternative<std::vector<EmulativeDialog::Context::
                                               ContextCandidate>>(
            AfterRevoke) &&
            std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
                AfterRevoke)
                .empty(),
        "revoked memory no longer recalled");
  Check(!Memory.IsConsentActive("consent-01"), "revoked consent inactive");
}

void TestScopeIsolation() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  auto First = AliveContext();
  auto Second = OtherContext();
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::IdentityPreference, First, "consent-a")));
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::IdentityPreference, Second, "consent-b")));
  static_cast<void>(Memory.Write(
      First, WriteRequest("memory-a", MemoryCategory::IdentityPreference,
                          "consent-a"),
      Now()));
  const auto SecondRecall = Memory.Recall(Second, Now(), 1000);
  Check(std::holds_alternative<std::vector<EmulativeDialog::Context::
                                               ContextCandidate>>(
            SecondRecall) &&
            std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
                SecondRecall)
                .empty(),
        "cross principal recall isolated");
  ForgetRequest CrossForget;
  CrossForget.MemoryId = "memory-a";
  Check(AsMemoryError(Memory.Forget(Second, CrossForget, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "cross principal forget does not mutate");
  const auto FirstRecall = Memory.Recall(First, Now(), 1000);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            FirstRecall)
            .size() == 1,
        "original scope unchanged");
}

void TestForgetAndExpiry() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Context = AliveContext();
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::IdentityPreference, Context)));
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::GroupMemory, Context, "consent-group")));
  static_cast<void>(Memory.Write(
      Context, WriteRequest("memory-1",
                            MemoryCategory::IdentityPreference),
      Now()));
  static_cast<void>(Memory.Write(
      Context, WriteRequest("memory-2", MemoryCategory::GroupMemory,
                            "consent-group"),
      Now()));
  ForgetRequest NoTarget;
  Check(AsMemoryError(Memory.Forget(Context, NoTarget, Now()))->Code ==
            MemoryErrorCode::ForgetScopeRequired,
        "broad forget rejected");
  ForgetRequest MissingTarget;
  MissingTarget.MemoryId = "memory-missing";
  Check(AsMemoryError(Memory.Forget(Context, MissingTarget, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "unmatched forget rejected");
  ForgetRequest Precise;
  Precise.MemoryId = "memory-1";
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Context, Precise, Now())),
        "precise forget tombstoned");
  ForgetRequest Category;
  Category.Category = MemoryCategory::GroupMemory;
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Context, Category, Now())),
        "category forget tombstoned");
  const auto Recall = Memory.Recall(Context, Now(), 1000);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Recall)
            .empty(),
        "forgotten records not recalled");
  auto Expiring = WriteRequest("memory-3",
                               MemoryCategory::IdentityPreference);
  Expiring.ExpiresAt = Now() - std::chrono::seconds{1};
  Check(AsMemoryError(Memory.Write(Context, Expiring, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "expired write rejected");
  auto FutureExpiry = WriteRequest("memory-4",
                                   MemoryCategory::IdentityPreference);
  FutureExpiry.ExpiresAt = Now() + std::chrono::seconds{10};
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Context, FutureExpiry, Now())),
        "expiring write accepted");
  const auto Later = Memory.Recall(
      Context, Now() + std::chrono::seconds{11}, 1000);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Later)
            .empty(),
        "expired memory filtered");
}

void TestCapacityAndPriority() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage, 2);
  const auto Context = AliveContext();
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::IdentityPreference, Context)));
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::ActiveConversationContext, Context,
              "consent-task")));
  for (int Index = 0; Index < 3; ++Index) {
    auto Request = WriteRequest(
        "memory-" + std::to_string(Index),
        MemoryCategory::IdentityPreference);
    Request.CreatedAt = Now() - std::chrono::seconds{100 - Index};
    static_cast<void>(Memory.Write(Context, Request, Now()));
  }
  const auto Records = Storage.Rows.begin()->second;
  std::size_t Active = 0;
  for (const auto& Record : Records) {
    Active += Record.Tombstoned ? 0U : 1U;
  }
  Check(Active == 2, "capacity evicts oldest deterministically");
  auto Task = WriteRequest("memory-task",
                           MemoryCategory::ActiveConversationContext,
                           "consent-task");
  Task.Content = "Active task context";
  static_cast<void>(Memory.Write(Context, Task, Now()));
  const auto Recall = Memory.Recall(Context, Now(), 1000);
  const auto& Candidates =
      std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
          Recall);
  Check(!Candidates.empty() &&
            Candidates.front().Kind ==
                EmulativeDialog::Context::ContextItemKind::ActiveChatTask,
        "higher priority task recalled first");
  const auto SmallBudget = Memory.Recall(Context, Now(), 1);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            SmallBudget)
                .size() <= 1,
        "recall respects token budget");
}

void TestRestoreBackup() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Context = AliveContext();
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::IdentityPreference, Context)));
  auto Record = WriteRequest("memory-restore",
                             MemoryCategory::IdentityPreference);
  StoredMemory Stored;
  Stored.MemoryId = Record.MemoryId;
  Stored.Category = Record.Category;
  Stored.ChannelId = Context.ChannelId;
  Stored.AccountId = Context.AccountId;
  Stored.ConversationId = Context.ConversationId;
  Stored.PrincipalId = Context.PrincipalId;
  Stored.SubjectId = Record.SubjectId;
  Stored.Content = Record.Content;
  Stored.SourceReference = Record.SourceReference;
  Stored.ConsentId = Record.ConsentId;
  Stored.CreatedAt = Record.CreatedAt;
  Stored.Provenance = Record.Provenance;
  Stored.Version = 1;
  Storage.BackupRecords.push_back(Stored);
  const auto Restored = Memory.RestoreBackup(Context, Now());
  Check(std::holds_alternative<std::size_t>(Restored) &&
            std::get<std::size_t>(Restored) == 1,
        "authorized backup restored");
  static_cast<void>(Memory.RevokeConsent(Context, "consent-01", Now()));
  const auto Denied = Memory.RestoreBackup(Context, Now());
  Check(std::holds_alternative<std::size_t>(Denied) &&
            std::get<std::size_t>(Denied) == 0,
        "revoked consent not restored");
}

void TestStorageFailureMapping() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Context = AliveContext();
  static_cast<void>(Memory.RegisterConsent(
      Consent(MemoryCategory::IdentityPreference, Context)));
  Storage.FailList = true;
  Check(AsMemoryError(Memory.Recall(Context, Now(), 10))->Code ==
            MemoryErrorCode::StorageFailure,
        "list failure mapped");
  Storage.FailList = false;
  Storage.FailPut = true;
  Check(AsMemoryError(Memory.Write(
                          Context,
                          WriteRequest("memory-1",
                                       MemoryCategory::IdentityPreference),
                          Now()))
            ->Code == MemoryErrorCode::StorageFailure,
        "put failure mapped");
  Storage.FailPut = false;
  Check(std::holds_alternative<std::monostate>(Memory.Write(
            Context,
            WriteRequest("memory-live",
                         MemoryCategory::IdentityPreference),
            Now())),
        "record for tombstone failure");
  Storage.FailTombstone = true;
  ForgetRequest Request;
  Request.MemoryId = "memory-live";
  Check(AsMemoryError(Memory.Forget(Context, Request, Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "tombstone failure mapped");
  Storage.FailBackup = true;
  Check(std::get<MemoryError>(Memory.RestoreBackup(Context, Now())).Code ==
            MemoryErrorCode::StorageFailure,
        "backup failure mapped");
}

void TestFixtureCaseCoverage() {
  std::ifstream Fixture(
      "Tests/Contract/Memory/Fixtures/ScopeConsentCases.json",
      std::ios::binary);
  std::ostringstream Buffer;
  Buffer << Fixture.rdbuf();
  const std::string Content = Buffer.str();
  for (const char* CaseId :
       {"SC-001", "SC-002", "SC-006", "SC-008", "SC-009", "SC-010",
        "SC-011"}) {
    Check(Content.find(CaseId) != std::string::npos,
          std::string("fixture case present ") + CaseId);
  }
  std::ifstream Forget(
      "Tests/Contract/Memory/Fixtures/ForgetRecoveryCases.json",
      std::ios::binary);
  std::ostringstream ForgetBuffer;
  ForgetBuffer << Forget.rdbuf();
  const std::string ForgetContent = ForgetBuffer.str();
  for (const char* CaseId : {"FR-001", "FR-004", "FR-008"}) {
    Check(ForgetContent.find(CaseId) != std::string::npos,
          std::string("forget fixture case present ") + CaseId);
  }
}

} // namespace

int main() {
  TestIdentityAndArgumentGates();
  TestConsentLifecycle();
  TestScopeIsolation();
  TestForgetAndExpiry();
  TestCapacityAndPriority();
  TestRestoreBackup();
  TestStorageFailureMapping();
  TestFixtureCaseCoverage();
  if (Failures != 0) {
    std::cerr << Failures << " checks failed\n";
    return 1;
  }
  std::cout << "All ConversationMemory checks passed\n";
  return 0;
}
