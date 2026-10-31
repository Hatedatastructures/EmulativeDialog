#include "Source/Memory/SqliteMemoryStorageAdapter.hpp"

#include "Source/Memory/ConversationMemory.hpp"
#include "Source/Storage/SqliteStore.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using EmulativeDialog::Memory::AuthorizedMemoryContext;
using EmulativeDialog::Memory::ConsentGrant;
using EmulativeDialog::Memory::ConversationMemory;
using EmulativeDialog::Memory::ConversationScopeKind;
using EmulativeDialog::Memory::MemoryCategory;
using EmulativeDialog::Memory::MemoryMutationBatch;
using EmulativeDialog::Memory::MemoryProvenance;
using EmulativeDialog::Memory::MemoryStorageError;
using EmulativeDialog::Memory::MemoryStorageErrorCode;
using EmulativeDialog::Memory::MemoryWriteRequest;
using EmulativeDialog::Memory::SqliteMemoryStorageAdapter;
using EmulativeDialog::Memory::StoredMemory;
using EmulativeDialog::Storage::ConversationScopeKey;
using EmulativeDialog::Storage::MemoryDeleteState;
using EmulativeDialog::Storage::MemoryRecord;
using StorageMemoryProvenance =
    EmulativeDialog::Storage::MemoryProvenance;
using EmulativeDialog::Storage::SqliteStore;
using EmulativeDialog::Storage::SqliteStoreError;
using EmulativeDialog::Storage::SqliteStoreResult;
using EmulativeDialog::Storage::SqliteStoreTransaction;

int Failures = 0;
std::atomic<std::uint64_t> TemporaryDirectorySequence{0};

void Check(bool Condition, std::string_view Description) {
  if (!Condition) {
    ++Failures;
    std::fprintf(stderr, "FAIL: %.*s\n",
                 static_cast<int>(Description.size()), Description.data());
  }
}

std::chrono::sys_seconds Now() {
  return std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
}

class TemporaryDatabase final {
public:
  explicit TemporaryDatabase(std::string_view Label) {
    const char* RootEnvironment =
        std::getenv("EMULATIVEDIALOG_TEST_DATA_DIR");
    if (RootEnvironment == nullptr || *RootEnvironment == '\0') {
      return;
    }

    const std::filesystem::path Root{RootEnvironment};
    std::error_code Error;
    std::filesystem::create_directories(Root, Error);
    if (Error) {
      return;
    }

    const auto Stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    for (std::uint64_t Attempt = 0; Attempt < 32; ++Attempt) {
      const auto Sequence =
          TemporaryDirectorySequence.fetch_add(1, std::memory_order_relaxed);
      const std::string Name =
          "ED0144-" + std::string(Label) + "-" + std::to_string(Stamp) +
          "-" + std::to_string(Sequence);
      std::filesystem::path Candidate = Root / Name;
      Error.clear();
      if (std::filesystem::create_directory(Candidate, Error)) {
        Directory_ = std::move(Candidate);
        Database_ = Directory_ / "memory.sqlite3";
        return;
      }
      if (Error) {
        return;
      }
    }
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  ~TemporaryDatabase() {
    if (Directory_.empty()) {
      return;
    }
    std::error_code Ignored;
    std::filesystem::remove_all(Directory_, Ignored);
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept {
    return Database_;
  }

  [[nodiscard]] bool IsReady() const noexcept {
    return !Directory_.empty() && !Database_.empty();
  }

private:
  std::filesystem::path Directory_;
  std::filesystem::path Database_;
};

std::unique_ptr<SqliteStore> OpenStore(
    const std::filesystem::path& Path) {
  auto Opened = SqliteStore::Open(Path);
  auto* Store =
      std::get_if<std::unique_ptr<SqliteStore>>(&Opened);
  Check(Store != nullptr, "SQLite store opens test database");
  if (Store == nullptr) {
    return {};
  }
  return std::move(*Store);
}

AuthorizedMemoryContext Context(
    ConversationScopeKind Kind = ConversationScopeKind::Direct,
    std::string Conversation = "conversation-01",
    std::string Principal = "principal-01") {
  AuthorizedMemoryContext Result;
  Result.ChannelId = "qq";
  Result.AccountId = "account-01";
  Result.ConversationId = std::move(Conversation);
  Result.PrincipalId = std::move(Principal);
  Result.ScopeKind = Kind;
  Result.IdentityPolicyVersion = 1;
  Result.IdentityVerified = true;
  return Result;
}

StoredMemory ScopeTemplate(const AuthorizedMemoryContext& ContextValue) {
  StoredMemory Result;
  Result.ChannelId = ContextValue.ChannelId;
  Result.AccountId = ContextValue.AccountId;
  Result.ConversationId = ContextValue.ConversationId;
  Result.ScopeKind = ContextValue.ScopeKind;
  Result.PrincipalId = ContextValue.PrincipalId;
  return Result;
}

StoredMemory MakeRecord(
    const StoredMemory& Scope, std::string MemoryId,
    MemoryCategory Category = MemoryCategory::IdentityPreference,
    MemoryProvenance Provenance = MemoryProvenance::UserExplicit,
    std::uint64_t Version = 1,
    std::optional<bool> UserConfirmedOverride = std::nullopt) {
  StoredMemory Result = Scope;
  Result.MemoryId = std::move(MemoryId);
  Result.Category = Category;
  Result.SubjectId = Scope.PrincipalId;
  Result.Content = "synthetic memory content";
  Result.SourceReference = "fixture-message-01";
  Result.ConsentId = "consent-01";
  Result.CreatedAt = Now() - std::chrono::seconds{60};
  Result.ExpiresAt = std::nullopt;
  Result.Provenance = Provenance;
  Result.UserConfirmed = UserConfirmedOverride.value_or(
      Provenance == MemoryProvenance::UserConfirmed);
  Result.Version = Version;
  return Result;
}

ConsentGrant MakeGrant(const AuthorizedMemoryContext& ContextValue,
                       std::string ConsentId = "consent-01") {
  ConsentGrant Grant;
  Grant.ConsentId = std::move(ConsentId);
  Grant.GrantorPrincipalId = ContextValue.PrincipalId;
  Grant.ChannelId = ContextValue.ChannelId;
  Grant.AccountId = ContextValue.AccountId;
  Grant.ConversationId = ContextValue.ConversationId;
  Grant.ScopeKind = ContextValue.ScopeKind;
  Grant.Category = MemoryCategory::IdentityPreference;
  Grant.SourceMessageId = "fixture-consent-message";
  Grant.GrantedAt = Now() - std::chrono::seconds{30};
  Grant.IdentityPolicyVersion = ContextValue.IdentityPolicyVersion;
  return Grant;
}

MemoryWriteRequest MakeWriteRequest(std::string MemoryId,
                                   bool UserConfirmed = false) {
  MemoryWriteRequest Request;
  Request.MemoryId = std::move(MemoryId);
  Request.Category = MemoryCategory::IdentityPreference;
  Request.SubjectId = "principal-01";
  Request.Content = "synthetic durable preference";
  Request.SourceReference = "fixture-message-write";
  Request.ConsentId = "consent-01";
  Request.CreatedAt = Now() - std::chrono::seconds{20};
  Request.Provenance = MemoryProvenance::UserExplicit;
  Request.UserConfirmed = UserConfirmed;
  Request.Version = 1;
  return Request;
}

bool HasStorageError(
    const EmulativeDialog::Memory::MemoryStorageResult<std::monostate>& Result,
    MemoryStorageErrorCode Code) {
  const auto* Error = std::get_if<MemoryStorageError>(&Result);
  return Error != nullptr && Error->Code == Code;
}

bool HasStorageError(
    const EmulativeDialog::Memory::MemoryStorageResult<
        std::vector<StoredMemory>>& Result,
    MemoryStorageErrorCode Code) {
  const auto* Error = std::get_if<MemoryStorageError>(&Result);
  return Error != nullptr && Error->Code == Code;
}

const std::vector<StoredMemory>* StorageValue(
    const EmulativeDialog::Memory::MemoryStorageResult<
        std::vector<StoredMemory>>& Result) {
  return std::get_if<std::vector<StoredMemory>>(&Result);
}

ConversationScopeKey StorageScopeForTest(const StoredMemory& Scope) {
  const std::string_view Prefix =
      Scope.ScopeKind == ConversationScopeKind::Group
          ? "EmulativeDialog.ScopeV1.Group/"
          : "EmulativeDialog.ScopeV1.Direct/";
  return ConversationScopeKey{
      .ChannelId = Scope.ChannelId,
      .AccountId = Scope.AccountId,
      .ConversationId = std::string(Prefix) + Scope.ConversationId,
      .PrincipalId = Scope.PrincipalId,
  };
}

MemoryRecord MakeRawRecord(const StoredMemory& Scope, std::string MemoryId,
                           std::string MemoryKind) {
  const auto Timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             Now().time_since_epoch())
                             .count();
  return MemoryRecord{
      .MemoryId = std::move(MemoryId),
      .MemoryKind = std::move(MemoryKind),
      .Scope = StorageScopeForTest(Scope),
      .SubjectId = Scope.PrincipalId,
      .Content = "synthetic content",
      .SourceReference = "fixture-message",
      .CreatedAtUnixSeconds = Timestamp - 5,
      .UpdatedAtUnixSeconds = Timestamp,
      .ExpiresAtUnixSeconds = std::nullopt,
      .Provenance = StorageMemoryProvenance::UserExplicit,
      .ConsentId = "consent-raw",
      .DeleteState = MemoryDeleteState::Active,
      .Version = 1,
  };
}

void PutRawRecord(SqliteStore& Store, const MemoryRecord& Record) {
  auto Begin = Store.BeginTransaction();
  auto* Transaction =
      std::get_if<std::unique_ptr<SqliteStoreTransaction>>(&Begin);
  Check(Transaction != nullptr, "raw SQLite fixture transaction starts");
  if (Transaction == nullptr) {
    return;
  }
  const auto Put = Store.PutRecord(**Transaction, Record);
  Check(std::holds_alternative<std::monostate>(Put),
        "raw SQLite fixture record is stored");
  if (std::holds_alternative<SqliteStoreError>(Put)) {
    static_cast<void>((*Transaction)->Rollback());
    return;
  }
  Check(std::holds_alternative<std::monostate>((*Transaction)->Commit()),
        "raw SQLite fixture transaction commits");
}

class SplitSqliteBackupStorage final
    : public EmulativeDialog::Memory::MemoryStoragePort {
public:
  SplitSqliteBackupStorage(SqliteMemoryStorageAdapter& Destination,
                           SqliteMemoryStorageAdapter& BackupSource) noexcept
      : Destination_(Destination), BackupSource_(BackupSource) {}

  EmulativeDialog::Memory::MemoryStorageResult<std::monostate>
  ApplyAtomically(const MemoryMutationBatch& Batch) override {
    return Destination_.ApplyAtomically(Batch);
  }

  EmulativeDialog::Memory::MemoryStorageResult<std::vector<StoredMemory>>
  ListActive(const StoredMemory& ScopeTemplate,
             std::chrono::sys_seconds Current) const override {
    return Destination_.ListActive(ScopeTemplate, Current);
  }

  EmulativeDialog::Memory::MemoryStorageResult<std::vector<StoredMemory>>
  RecallBackup(const StoredMemory& ScopeTemplate,
               std::chrono::sys_seconds Current) const override {
    return BackupSource_.RecallBackup(ScopeTemplate, Current);
  }

private:
  SqliteMemoryStorageAdapter& Destination_;
  SqliteMemoryStorageAdapter& BackupSource_;
};

void TestV1MemoryKindCompatibility() {
  TemporaryDatabase Database{"V1Compatibility"};
  if (!Database.IsReady()) {
    Check(false, "test database directory is available");
    return;
  }
  auto Store = OpenStore(Database.Path());
  if (!Store) {
    return;
  }

  const StoredMemory Scope = ScopeTemplate(Context());
  PutRawRecord(*Store, MakeRawRecord(
                           Scope, "legacy-v1",
                           "EDMemoryV1/IdentityPreference/1"));
  SqliteMemoryStorageAdapter Adapter{*Store};
  const auto Active = Adapter.ListActive(Scope, Now());
  const auto* ActiveRecords = StorageValue(Active);
  Check(ActiveRecords != nullptr && ActiveRecords->size() == 1U,
        "legacy V1 row remains readable from active storage");
  if (ActiveRecords != nullptr && ActiveRecords->size() == 1U) {
    Check(!ActiveRecords->front().ProposalId.has_value(),
          "legacy V1 row does not invent a ProposalId");
    Check(ActiveRecords->front().UserConfirmed,
          "legacy V1 confirmation bit remains readable");
  }
  const auto Backup = Adapter.RecallBackup(Scope, Now());
  const auto* BackupRecords = StorageValue(Backup);
  Check(BackupRecords != nullptr && BackupRecords->size() == 1U,
        "legacy V1 row remains readable from backup storage");
  if (BackupRecords != nullptr && BackupRecords->size() == 1U) {
    Check(!BackupRecords->front().ProposalId.has_value(),
          "legacy V1 backup does not infer a ProposalId");
  }
}

void TestProposalIdRoundTripAndBackupRestore() {
  TemporaryDatabase SourceDatabase{"ProposalSource"};
  TemporaryDatabase DestinationDatabase{"ProposalDestination"};
  TemporaryDatabase MismatchDatabase{"ProposalMismatch"};
  if (!SourceDatabase.IsReady() || !DestinationDatabase.IsReady() ||
      !MismatchDatabase.IsReady()) {
    Check(false, "proposal round-trip databases are available");
    return;
  }

  std::string ProposalId{"proposal/:delimiters"};
  ProposalId.push_back('\0');
  ProposalId.push_back(static_cast<char>(0xff));
  ProposalId.push_back(static_cast<char>(0xc3));
  const AuthorizedMemoryContext ContextValue = Context();
  const StoredMemory Scope = ScopeTemplate(ContextValue);

  {
    auto Store = OpenStore(SourceDatabase.Path());
    if (!Store) {
      return;
    }
    SqliteMemoryStorageAdapter Adapter{*Store};
    ConversationMemory Memory{Adapter};
    ConsentGrant Grant = MakeGrant(ContextValue);
    Grant.ProposalId = ProposalId;
    Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(Grant)),
          "proposal-bound consent registers");
    MemoryWriteRequest Request = MakeWriteRequest("proposal-memory", true);
    Request.ProposalId = ProposalId;
    Check(std::holds_alternative<std::monostate>(
              Memory.Write(ContextValue, Request, Now())),
          "proposal-bound write reaches SQLite ApplyAtomically");

    const auto CurrentUnixSeconds = std::chrono::duration_cast<
        std::chrono::seconds>(Now().time_since_epoch()).count();
    const auto Raw = Store->GetActiveRecord(
        StorageScopeForTest(Scope), "proposal-memory", CurrentUnixSeconds);
    const auto* RawRecord = std::get_if<MemoryRecord>(&Raw);
    constexpr std::string_view ExpectedMemoryKind =
        "EDMemoryV2/IdentityPreference/1/P23:"
        "70726f706f73616c2f3a64656c696d697465727300ffc3";
    Check(RawRecord != nullptr &&
              RawRecord->MemoryKind == ExpectedMemoryKind,
          "V2 storage kind uses canonical length and lowercase exact-byte hex");

    const auto Active = Adapter.ListActive(Scope, Now());
    const auto* ActiveRecords = StorageValue(Active);
    Check(ActiveRecords != nullptr && ActiveRecords->size() == 1U,
          "V2 proposal row is listed while the SQLite connection is open");
    if (ActiveRecords != nullptr && ActiveRecords->size() == 1U) {
      Check(ActiveRecords->front().ProposalId == ProposalId,
            "active read preserves delimiters, NUL and non-UTF-8 bytes");
    }
    const auto Backup = Adapter.RecallBackup(Scope, Now());
    const auto* BackupRecords = StorageValue(Backup);
    Check(BackupRecords != nullptr && BackupRecords->size() == 1U,
          "V2 proposal row is available to SQLite backup recall");
    if (BackupRecords != nullptr && BackupRecords->size() == 1U) {
      Check(BackupRecords->front().ProposalId == ProposalId,
            "backup read preserves exact proposal bytes");
    }
  }

  std::unique_ptr<SqliteStore> SourceStore = OpenStore(SourceDatabase.Path());
  std::unique_ptr<SqliteStore> DestinationStore =
      OpenStore(DestinationDatabase.Path());
  std::unique_ptr<SqliteStore> MismatchStore =
      OpenStore(MismatchDatabase.Path());
  if (!SourceStore || !DestinationStore || !MismatchStore) {
    return;
  }
  SqliteMemoryStorageAdapter SourceAdapter{*SourceStore};
  SqliteMemoryStorageAdapter DestinationAdapter{*DestinationStore};
  SqliteMemoryStorageAdapter MismatchAdapter{*MismatchStore};

  ConversationMemory ReopenedMemory{SourceAdapter};
  ConsentGrant ExactGrant = MakeGrant(ContextValue);
  ExactGrant.ProposalId = ProposalId;
  Check(std::holds_alternative<std::monostate>(
            ReopenedMemory.RegisterConsent(ExactGrant)),
        "exact proposal consent is registered after reopening the source DB");
  const auto Recalled = ReopenedMemory.Recall(ContextValue, Now(), 256U);
  const auto* RecalledItems = std::get_if<std::vector<
      EmulativeDialog::Context::ContextCandidate>>(&Recalled);
  Check(RecalledItems != nullptr && RecalledItems->size() == 1U,
        "exact proposal-bound consent survives SQLite-backed recall after reopen");
  ConversationMemory MismatchedMemory{SourceAdapter};
  ConsentGrant WrongRecallGrant = ExactGrant;
  WrongRecallGrant.ProposalId->back() = static_cast<char>(0xc4);
  Check(std::holds_alternative<std::monostate>(
            MismatchedMemory.RegisterConsent(WrongRecallGrant)),
        "different proposal-bound consent registers for negative recall test");
  const auto MismatchedRecall =
      MismatchedMemory.Recall(ContextValue, Now(), 256U);
  const auto* MismatchedItems = std::get_if<std::vector<
      EmulativeDialog::Context::ContextCandidate>>(&MismatchedRecall);
  Check(MismatchedItems != nullptr && MismatchedItems->empty(),
        "mismatched proposal-bound consent fails closed on recall");

  SplitSqliteBackupStorage RestoreStorage{DestinationAdapter, SourceAdapter};
  ConversationMemory Restorer{RestoreStorage};
  Check(std::holds_alternative<std::monostate>(Restorer.RegisterConsent(ExactGrant)),
        "exact proposal consent registers for backup restoration");
  const auto Restore = Restorer.RestoreBackup(ContextValue, Now());
  const auto* RestoredCount = std::get_if<std::size_t>(&Restore);
  Check(RestoredCount != nullptr && *RestoredCount == 1U,
        "matching proposal-bound consent restores SQLite backup row");
  const auto RestoredRows = DestinationAdapter.ListActive(Scope, Now());
  const auto* RestoredRecords = StorageValue(RestoredRows);
  Check(RestoredRecords != nullptr && RestoredRecords->size() == 1U,
        "restored proposal row is present in destination SQLite database");
  if (RestoredRecords != nullptr && RestoredRecords->size() == 1U) {
    Check(RestoredRecords->front().ProposalId == ProposalId,
          "SQLite backup restore preserves exact proposal bytes");
  }
  ConversationMemory RestoredMemory{DestinationAdapter};
  Check(std::holds_alternative<std::monostate>(
            RestoredMemory.RegisterConsent(ExactGrant)),
        "exact proposal consent registers after destination restore");
  const auto RestoredRecall =
      RestoredMemory.Recall(ContextValue, Now(), 256U);
  const auto* RestoredItems = std::get_if<std::vector<
      EmulativeDialog::Context::ContextCandidate>>(&RestoredRecall);
  Check(RestoredItems != nullptr && RestoredItems->size() == 1U,
        "restored proposal remains recallable under exact consent");

  SplitSqliteBackupStorage MismatchRestoreStorage{MismatchAdapter,
                                                   SourceAdapter};
  ConversationMemory MismatchRestorer{MismatchRestoreStorage};
  Check(std::holds_alternative<std::monostate>(
            MismatchRestorer.RegisterConsent(WrongRecallGrant)),
        "different proposal-bound consent registers for negative restore test");
  const auto MismatchRestore =
      MismatchRestorer.RestoreBackup(ContextValue, Now());
  const auto* MismatchCount = std::get_if<std::size_t>(&MismatchRestore);
  Check(MismatchCount != nullptr && *MismatchCount == 0U,
        "mismatched proposal-bound consent fails closed on backup restore");
  const auto MismatchRows = MismatchAdapter.ListActive(Scope, Now());
  Check(StorageValue(MismatchRows) != nullptr &&
            StorageValue(MismatchRows)->empty(),
        "mismatched proposal restore writes no SQLite row");

  MemoryMutationBatch EmptyProposalBatch;
  EmptyProposalBatch.Scope = Scope;
  StoredMemory EmptyProposal = MakeRecord(Scope, "empty-proposal");
  EmptyProposal.ProposalId = std::string{};
  EmptyProposalBatch.Upserts.push_back(std::move(EmptyProposal));
  Check(HasStorageError(DestinationAdapter.ApplyAtomically(EmptyProposalBatch),
                        MemoryStorageErrorCode::InvalidArgument),
        "present but empty ProposalId is rejected before SQLite write");
}

void TestBackupRestorePreservesExpiryAcrossRestart() {
  TemporaryDatabase SourceDatabase{"ExpirySource"};
  TemporaryDatabase DestinationDatabase{"ExpiryDestination"};
  if (!SourceDatabase.IsReady() || !DestinationDatabase.IsReady()) {
    Check(false, "expiry backup databases are available");
    return;
  }

  const AuthorizedMemoryContext ContextValue = Context();
  const StoredMemory Scope = ScopeTemplate(ContextValue);
  const std::chrono::sys_seconds WrittenAt = Now();
  const std::chrono::sys_seconds BeforeExpiry =
      WrittenAt + std::chrono::hours{1};
  const std::chrono::sys_seconds ExpiresAt =
      WrittenAt + std::chrono::hours{24};

  {
    auto Store = OpenStore(SourceDatabase.Path());
    if (!Store) {
      return;
    }
    SqliteMemoryStorageAdapter Adapter{*Store};
    ConversationMemory Memory{Adapter};
    ConsentGrant Grant = MakeGrant(ContextValue);
    Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(Grant)),
          "expiry consent registers for SQLite backup source");

    MemoryWriteRequest Request = MakeWriteRequest("expiring-memory", true);
    Request.CreatedAt = WrittenAt;
    Request.ExpiresAt = ExpiresAt;
    Check(std::holds_alternative<std::monostate>(
              Memory.Write(ContextValue, Request, WrittenAt)),
          "future-expiring memory is written to the real SQLite source");

    const auto Backup = Adapter.RecallBackup(Scope, BeforeExpiry);
    const auto* BackupRecords = StorageValue(Backup);
    Check(BackupRecords != nullptr && BackupRecords->size() == 1U,
          "future-expiring memory is available to backup recall");
    if (BackupRecords != nullptr && BackupRecords->size() == 1U) {
      Check(BackupRecords->front().ExpiresAt == ExpiresAt,
            "backup recall preserves the exact future expiry timestamp");
    }
  }

  std::unique_ptr<SqliteStore> SourceStore =
      OpenStore(SourceDatabase.Path());
  std::unique_ptr<SqliteStore> DestinationStore =
      OpenStore(DestinationDatabase.Path());
  if (!SourceStore || !DestinationStore) {
    return;
  }
  SqliteMemoryStorageAdapter SourceAdapter{*SourceStore};
  SqliteMemoryStorageAdapter DestinationAdapter{*DestinationStore};
  const auto ReopenedBackup = SourceAdapter.RecallBackup(Scope, BeforeExpiry);
  const auto* ReopenedRecords = StorageValue(ReopenedBackup);
  Check(ReopenedRecords != nullptr && ReopenedRecords->size() == 1U,
        "backup recall remains available after closing and reopening SQLite");
  if (ReopenedRecords != nullptr && ReopenedRecords->size() == 1U) {
    Check(ReopenedRecords->front().ExpiresAt == ExpiresAt,
          "reopened SQLite backup retains the original expiry timestamp");
  }

  SplitSqliteBackupStorage RestoreStorage{DestinationAdapter, SourceAdapter};
  ConversationMemory Restorer{RestoreStorage};
  ConsentGrant Grant = MakeGrant(ContextValue);
  Check(std::holds_alternative<std::monostate>(Restorer.RegisterConsent(Grant)),
        "authorized restore consent registers for the exact memory scope");
  const auto Restore = Restorer.RestoreBackup(ContextValue, BeforeExpiry);
  const auto* RestoredCount = std::get_if<std::size_t>(&Restore);
  Check(RestoredCount != nullptr && *RestoredCount == 1U,
        "authorized ConversationMemory restore copies the future-expiring row");

  const auto ActiveBeforeExpiry =
      DestinationAdapter.ListActive(Scope, BeforeExpiry);
  const auto* ActiveRecords = StorageValue(ActiveBeforeExpiry);
  Check(ActiveRecords != nullptr && ActiveRecords->size() == 1U,
        "restored SQLite row is active before its original expiry");
  if (ActiveRecords != nullptr && ActiveRecords->size() == 1U) {
    Check(ActiveRecords->front().ExpiresAt == ExpiresAt,
          "restore preserves exact expiry in the destination SQLite record");
  }

  const auto ActiveSourceBackup = SourceAdapter.RecallBackup(Scope, ExpiresAt);
  const auto* ActiveSourceBackupRecords = StorageValue(ActiveSourceBackup);
  Check(ActiveSourceBackupRecords != nullptr &&
            ActiveSourceBackupRecords->empty(),
        "source backup recall hides the record at its original expiry");
  const auto ActiveDestinationBackup =
      DestinationAdapter.RecallBackup(Scope, ExpiresAt);
  const auto* ActiveDestinationBackupRecords =
      StorageValue(ActiveDestinationBackup);
  Check(ActiveDestinationBackupRecords != nullptr &&
            ActiveDestinationBackupRecords->empty(),
        "destination backup recall hides the record at its original expiry");

  const auto ActiveAtExpiry = DestinationAdapter.ListActive(Scope, ExpiresAt);
  const auto* ActiveAtExpiryRecords = StorageValue(ActiveAtExpiry);
  Check(ActiveAtExpiryRecords != nullptr && ActiveAtExpiryRecords->empty(),
        "ListActive hides the restored record at its original expiry");
  const auto RecallAtExpiry = Restorer.Recall(ContextValue, ExpiresAt, 256U);
  const auto* RecalledAtExpiry = std::get_if<std::vector<
      EmulativeDialog::Context::ContextCandidate>>(&RecallAtExpiry);
  Check(RecalledAtExpiry != nullptr && RecalledAtExpiry->empty(),
        "authorized ConversationMemory recall hides the expired row");
  const auto RestoreAtExpiry = Restorer.RestoreBackup(ContextValue, ExpiresAt);
  const auto* ExpiredRestoredCount = std::get_if<std::size_t>(&RestoreAtExpiry);
  Check(ExpiredRestoredCount != nullptr && *ExpiredRestoredCount == 0U,
        "backup restore cannot resurrect a row at or after expiry");
}

void TestCategoryAndProvenanceRoundTrip() {
  TemporaryDatabase Database{"CategoryProvenance"};
  if (!Database.IsReady()) {
    Check(false, "test database directory is available");
    return;
  }
  const StoredMemory Scope = ScopeTemplate(Context());

  const MemoryCategory Categories[] = {
      MemoryCategory::IdentityPreference,
      MemoryCategory::ConversationSummary,
      MemoryCategory::GroupMemory,
      MemoryCategory::IdentityLinkMemory,
      MemoryCategory::ActiveConversationContext,
  };
  const MemoryProvenance Provenances[] = {
      MemoryProvenance::UserExplicit,
      MemoryProvenance::UserConfirmed,
      MemoryProvenance::AuthorizedSummary,
  };
  struct ConfirmationCase final {
    MemoryProvenance Provenance;
    bool UserConfirmed;
  };
  const ConfirmationCase ConfirmationCases[] = {
      {MemoryProvenance::UserExplicit, false},
      {MemoryProvenance::UserExplicit, true},
      {MemoryProvenance::UserConfirmed, false},
      {MemoryProvenance::UserConfirmed, true},
      {MemoryProvenance::AuthorizedSummary, false},
      {MemoryProvenance::AuthorizedSummary, true},
  };

  std::vector<StoredMemory> Expected;
  {
    auto Store = OpenStore(Database.Path());
    if (!Store) {
      return;
    }
    SqliteMemoryStorageAdapter Adapter{*Store};
    MemoryMutationBatch Batch;
    Batch.Scope = Scope;
    for (std::size_t CategoryIndex = 0;
         CategoryIndex < std::size(Categories); ++CategoryIndex) {
      for (std::size_t ProvenanceIndex = 0;
           ProvenanceIndex < std::size(Provenances); ++ProvenanceIndex) {
        const std::string Id =
            "category-" + std::to_string(CategoryIndex) + "-provenance-" +
            std::to_string(ProvenanceIndex);
        StoredMemory Record = MakeRecord(
            Scope, Id, Categories[CategoryIndex],
            Provenances[ProvenanceIndex]);
        Expected.push_back(Record);
        Batch.Upserts.push_back(std::move(Record));
      }
    }
    for (std::size_t Index = 0; Index < std::size(ConfirmationCases); ++Index) {
      const ConfirmationCase& Case = ConfirmationCases[Index];
      StoredMemory Record = MakeRecord(
          Scope, "confirmation-" + std::to_string(Index),
          MemoryCategory::IdentityPreference, Case.Provenance, 1,
          Case.UserConfirmed);
      Expected.push_back(Record);
      Batch.Upserts.push_back(std::move(Record));
    }
    const auto Applied = Adapter.ApplyAtomically(Batch);
    Check(std::holds_alternative<std::monostate>(Applied),
          "all category, provenance, and confirmation variants persist");
  }

  auto ReopenedStore = OpenStore(Database.Path());
  if (!ReopenedStore) {
    return;
  }
  SqliteMemoryStorageAdapter ReopenedAdapter{*ReopenedStore};
  const auto Listed = ReopenedAdapter.ListActive(Scope, Now());
  const auto* Records = StorageValue(Listed);
  Check(Records != nullptr && Records->size() == Expected.size(),
        "all metadata variants survive a real SQLite reopen");
  if (Records == nullptr) {
    return;
  }
  for (const StoredMemory& ExpectedRecord : Expected) {
    const auto Actual = std::find_if(
        Records->begin(), Records->end(), [&ExpectedRecord](const auto& Value) {
          return Value.MemoryId == ExpectedRecord.MemoryId;
        });
    Check(Actual != Records->end(), "record exists after SQLite reopen");
    if (Actual == Records->end()) {
      continue;
    }
    Check(Actual->Category == ExpectedRecord.Category,
          "category survives SQLite reopen");
    Check(Actual->Provenance == ExpectedRecord.Provenance,
          "provenance survives SQLite reopen");
    Check(Actual->UserConfirmed == ExpectedRecord.UserConfirmed,
          "UserConfirmed bit survives SQLite reopen independently");
  }

  const auto Backup = ReopenedAdapter.RecallBackup(Scope, Now());
  const auto* BackupRecords = StorageValue(Backup);
  Check(BackupRecords != nullptr && BackupRecords->size() == Expected.size(),
        "backup recall preserves active records and encoded metadata");
  if (BackupRecords != nullptr) {
    for (const ConfirmationCase& Case : ConfirmationCases) {
      const std::size_t Index = static_cast<std::size_t>(
          &Case - std::begin(ConfirmationCases));
      const std::string Id = "confirmation-" + std::to_string(Index);
      const auto Actual = std::find_if(
          BackupRecords->begin(), BackupRecords->end(), [&Id](const auto& Value) {
            return Value.MemoryId == Id;
          });
      Check(Actual != BackupRecords->end() &&
                Actual->Provenance == Case.Provenance &&
                Actual->UserConfirmed == Case.UserConfirmed,
            "backup recall preserves each independent confirmation value");
    }
  }
}

void TestScopeKindIsolation() {
  TemporaryDatabase Database{"ScopeKindIsolation"};
  if (!Database.IsReady()) {
    Check(false, "test database directory is available");
    return;
  }
  auto Store = OpenStore(Database.Path());
  if (!Store) {
    return;
  }
  SqliteMemoryStorageAdapter Adapter{*Store};
  const StoredMemory Direct =
      ScopeTemplate(Context(ConversationScopeKind::Direct));
  const StoredMemory Group =
      ScopeTemplate(Context(ConversationScopeKind::Group));
  Check(Direct.ConversationId == Group.ConversationId,
        "isolation uses the same source conversation identifier");

  MemoryMutationBatch DirectBatch;
  DirectBatch.Scope = Direct;
  DirectBatch.Upserts.push_back(MakeRecord(Direct, "shared-id"));
  MemoryMutationBatch GroupBatch;
  GroupBatch.Scope = Group;
  GroupBatch.Upserts.push_back(MakeRecord(
      Group, "shared-id", MemoryCategory::GroupMemory));

  Check(std::holds_alternative<std::monostate>(
            Adapter.ApplyAtomically(DirectBatch)),
        "direct-scope record is stored");
  Check(std::holds_alternative<std::monostate>(
            Adapter.ApplyAtomically(GroupBatch)),
        "group-scope record with the same ID is stored independently");

  const auto DirectList = Adapter.ListActive(Direct, Now());
  const auto GroupList = Adapter.ListActive(Group, Now());
  Check(StorageValue(DirectList) != nullptr &&
            StorageValue(DirectList)->size() == 1 &&
            StorageValue(DirectList)->front().ScopeKind ==
                ConversationScopeKind::Direct,
        "direct scope returns only its own typed record");
  Check(StorageValue(GroupList) != nullptr &&
            StorageValue(GroupList)->size() == 1 &&
            StorageValue(GroupList)->front().ScopeKind ==
                ConversationScopeKind::Group,
        "group scope returns only its own typed record");

  MemoryMutationBatch DeleteDirect;
  DeleteDirect.Scope = Direct;
  DeleteDirect.Tombstones.push_back("shared-id");
  Check(std::holds_alternative<std::monostate>(
            Adapter.ApplyAtomically(DeleteDirect)),
        "direct-scope tombstone commits");
  const auto DirectAfterDelete = Adapter.ListActive(Direct, Now());
  const auto GroupAfterDelete = Adapter.ListActive(Group, Now());
  Check(StorageValue(DirectAfterDelete) != nullptr &&
            StorageValue(DirectAfterDelete)->empty(),
        "direct tombstone hides its record");
  Check(StorageValue(GroupAfterDelete) != nullptr &&
            StorageValue(GroupAfterDelete)->size() == 1,
        "direct tombstone does not affect the group record");
}

void TestTransactionalRollbackAndVersionConflict() {
  TemporaryDatabase Database{"AtomicRollback"};
  if (!Database.IsReady()) {
    Check(false, "test database directory is available");
    return;
  }
  auto Store = OpenStore(Database.Path());
  if (!Store) {
    return;
  }
  SqliteMemoryStorageAdapter Adapter{*Store};
  const StoredMemory Scope = ScopeTemplate(Context());

  MemoryMutationBatch Seed;
  Seed.Scope = Scope;
  Seed.Upserts.push_back(MakeRecord(Scope, "existing"));
  Check(std::holds_alternative<std::monostate>(Adapter.ApplyAtomically(Seed)),
        "initial record commits");

  MemoryMutationBatch FailingBatch;
  FailingBatch.Scope = Scope;
  FailingBatch.Upserts.push_back(MakeRecord(Scope, "must-rollback"));
  FailingBatch.Tombstones.push_back("missing-record");
  const auto Failed = Adapter.ApplyAtomically(FailingBatch);
  Check(HasStorageError(Failed, MemoryStorageErrorCode::NotFound),
        "missing tombstone maps to NotFound");
  const auto AfterRollback = Adapter.ListActive(Scope, Now());
  const auto* Records = StorageValue(AfterRollback);
  Check(Records != nullptr && Records->size() == 1 &&
            Records->front().MemoryId == "existing",
        "failed tombstone rolls back earlier upsert in the batch");

  MemoryMutationBatch Conflict;
  Conflict.Scope = Scope;
  Conflict.Upserts.push_back(MakeRecord(
      Scope, "existing", MemoryCategory::IdentityPreference,
      MemoryProvenance::UserExplicit, 1));
  Check(HasStorageError(Adapter.ApplyAtomically(Conflict),
                        MemoryStorageErrorCode::VersionConflict),
        "non-increasing version maps to VersionConflict");

  MemoryMutationBatch Invalid;
  Invalid.Scope = Scope;
  StoredMemory Foreign = MakeRecord(Scope, "foreign");
  Foreign.PrincipalId = "principal-foreign";
  Invalid.Upserts.push_back(std::move(Foreign));
  Check(HasStorageError(Adapter.ApplyAtomically(Invalid),
                        MemoryStorageErrorCode::InvalidArgument),
        "cross-scope upsert is rejected before starting a transaction");
}

void TestNestedTransactionFailsClosed() {
  TemporaryDatabase Database{"NestedTransaction"};
  if (!Database.IsReady()) {
    Check(false, "test database directory is available");
    return;
  }
  auto Store = OpenStore(Database.Path());
  if (!Store) {
    return;
  }
  SqliteMemoryStorageAdapter Adapter{*Store};
  auto Outer = Store->BeginTransaction();
  auto* OuterTransaction =
      std::get_if<std::unique_ptr<SqliteStoreTransaction>>(&Outer);
  Check(OuterTransaction != nullptr,
        "outer SQLite transaction starts before adapter request");
  if (OuterTransaction == nullptr) {
    return;
  }

  MemoryMutationBatch Batch;
  Batch.Scope = ScopeTemplate(Context());
  Batch.Upserts.push_back(MakeRecord(Batch.Scope, "nested"));
  Check(HasStorageError(Adapter.ApplyAtomically(Batch),
                        MemoryStorageErrorCode::Failure),
        "nested transaction rejection maps to storage failure");
  const auto RollbackResult = (*OuterTransaction)->Rollback();
  Check(std::holds_alternative<std::monostate>(RollbackResult),
        "outer test transaction is rolled back");
}

void TestInvalidInputsAndNoOp() {
  TemporaryDatabase Database{"InvalidInputs"};
  if (!Database.IsReady()) {
    Check(false, "test database directory is available");
    return;
  }
  auto Store = OpenStore(Database.Path());
  if (!Store) {
    return;
  }
  SqliteMemoryStorageAdapter Adapter{*Store};

  MemoryMutationBatch Empty;
  Empty.Scope = ScopeTemplate(Context());
  Check(std::holds_alternative<std::monostate>(Adapter.ApplyAtomically(Empty)),
        "empty valid batch is a no-op");

  StoredMemory SessionScope =
      ScopeTemplate(Context(ConversationScopeKind::Session));
  MemoryMutationBatch SessionBatch;
  SessionBatch.Scope = SessionScope;
  SessionBatch.Upserts.push_back(MakeRecord(SessionScope, "volatile"));
  Check(HasStorageError(Adapter.ApplyAtomically(SessionBatch),
                        MemoryStorageErrorCode::InvalidArgument),
        "SessionScope cannot be persisted");
  Check(HasStorageError(Adapter.ListActive(SessionScope, Now()),
                        MemoryStorageErrorCode::InvalidArgument),
        "SessionScope cannot be listed from durable memory");
  Check(HasStorageError(Adapter.RecallBackup(SessionScope, Now()),
                        MemoryStorageErrorCode::InvalidArgument),
        "SessionScope cannot use durable backup recall");

  MemoryMutationBatch EmbeddedNulContent;
  EmbeddedNulContent.Scope = ScopeTemplate(Context());
  StoredMemory TruncatedContent =
      MakeRecord(EmbeddedNulContent.Scope, "embedded-nul-content");
  TruncatedContent.Content = "visible-prefix";
  TruncatedContent.Content.push_back('\0');
  TruncatedContent.Content += "hidden-suffix";
  EmbeddedNulContent.Upserts.push_back(std::move(TruncatedContent));
  Check(HasStorageError(Adapter.ApplyAtomically(EmbeddedNulContent),
                        MemoryStorageErrorCode::InvalidArgument),
        "embedded-NUL content is rejected before SQLite persistence");
  const auto AfterEmbeddedNulRejection =
      Adapter.ListActive(EmbeddedNulContent.Scope, Now());
  const auto* PersistedAfterRejection =
      StorageValue(AfterEmbeddedNulRejection);
  Check(PersistedAfterRejection != nullptr &&
            std::none_of(PersistedAfterRejection->begin(),
                         PersistedAfterRejection->end(),
                         [](const StoredMemory& Record) {
                           return Record.MemoryId == "embedded-nul-content";
                         }),
        "embedded-NUL content rejection leaves no SQLite record");

  MemoryMutationBatch DuplicateUpsert;
  DuplicateUpsert.Scope = ScopeTemplate(Context());
  DuplicateUpsert.Upserts.push_back(
      MakeRecord(DuplicateUpsert.Scope, "duplicate"));
  DuplicateUpsert.Upserts.push_back(
      MakeRecord(DuplicateUpsert.Scope, "duplicate",
                 MemoryCategory::GroupMemory,
                 MemoryProvenance::UserExplicit, 2));
  Check(HasStorageError(Adapter.ApplyAtomically(DuplicateUpsert),
                        MemoryStorageErrorCode::InvalidArgument),
        "duplicate upsert IDs are rejected");

  MemoryMutationBatch DuplicateTombstone;
  DuplicateTombstone.Scope = ScopeTemplate(Context());
  DuplicateTombstone.Tombstones = {"same", "same"};
  Check(HasStorageError(Adapter.ApplyAtomically(DuplicateTombstone),
                        MemoryStorageErrorCode::InvalidArgument),
        "duplicate tombstone IDs are rejected before a transaction");

  MemoryMutationBatch ConflictingId;
  ConflictingId.Scope = ScopeTemplate(Context());
  ConflictingId.Upserts.push_back(
      MakeRecord(ConflictingId.Scope, "same"));
  ConflictingId.Tombstones.push_back("same");
  Check(HasStorageError(Adapter.ApplyAtomically(ConflictingId),
                        MemoryStorageErrorCode::InvalidArgument),
        "same ID cannot be upserted and tombstoned in one batch");

  MemoryMutationBatch InvalidEnum;
  InvalidEnum.Scope = ScopeTemplate(Context());
  InvalidEnum.Upserts.push_back(MakeRecord(
      InvalidEnum.Scope, "invalid-category",
      static_cast<MemoryCategory>(255)));
  Check(HasStorageError(Adapter.ApplyAtomically(InvalidEnum),
                        MemoryStorageErrorCode::InvalidArgument),
        "unknown category enum is rejected");

  MemoryMutationBatch Expired;
  Expired.Scope = ScopeTemplate(Context());
  StoredMemory ExpiredRecord = MakeRecord(Expired.Scope, "expired");
  ExpiredRecord.ExpiresAt = ExpiredRecord.CreatedAt;
  Expired.Upserts.push_back(std::move(ExpiredRecord));
  Check(HasStorageError(Adapter.ApplyAtomically(Expired),
                        MemoryStorageErrorCode::InvalidArgument),
        "expiry must be later than creation");

  MemoryMutationBatch InvalidVersion;
  InvalidVersion.Scope = ScopeTemplate(Context());
  InvalidVersion.Upserts.push_back(MakeRecord(
      InvalidVersion.Scope, "invalid-version",
      MemoryCategory::IdentityPreference, MemoryProvenance::UserExplicit,
      static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max()) +
          1U));
  Check(HasStorageError(Adapter.ApplyAtomically(InvalidVersion),
                        MemoryStorageErrorCode::InvalidArgument),
        "SQLite signed version bound is enforced");

  MemoryMutationBatch InvalidProvenance;
  InvalidProvenance.Scope = ScopeTemplate(Context());
  InvalidProvenance.Upserts.push_back(MakeRecord(
      InvalidProvenance.Scope, "invalid-provenance",
      MemoryCategory::IdentityPreference,
      static_cast<MemoryProvenance>(255)));
  Check(HasStorageError(Adapter.ApplyAtomically(InvalidProvenance),
                        MemoryStorageErrorCode::InvalidArgument),
        "unknown provenance enum is rejected");

  MemoryMutationBatch InvalidScope;
  InvalidScope.Scope = ScopeTemplate(Context());
  InvalidScope.Scope.AccountId.clear();
  Check(HasStorageError(Adapter.ApplyAtomically(InvalidScope),
                        MemoryStorageErrorCode::InvalidArgument),
        "incomplete scope is rejected");
}

void TestConversationMemoryConsentTombstoneRestart() {
  TemporaryDatabase Database{"ConsentTombstoneRestart"};
  if (!Database.IsReady()) {
    Check(false, "test database directory is available");
    return;
  }

  const AuthorizedMemoryContext ContextValue = Context();
  {
    auto Store = OpenStore(Database.Path());
    if (!Store) {
      return;
    }
    SqliteMemoryStorageAdapter Adapter{*Store};
    ConversationMemory Memory{Adapter};
    const ConsentGrant Grant = MakeGrant(ContextValue);
    Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(Grant)),
          "consent registers through the production memory service");
    Check(std::holds_alternative<std::monostate>(Memory.Write(
              ContextValue, MakeWriteRequest("persisted-memory", true), Now())),
          "ConversationMemory writes through SQLite adapter");
    const auto Recalled = Memory.Recall(ContextValue, Now(), 256);
    const auto* ContextItems =
        std::get_if<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            &Recalled);
    Check(ContextItems != nullptr && ContextItems->size() == 1,
          "SQLite-backed conversation memory is recallable");
    Check(ContextItems != nullptr && ContextItems->size() == 1 &&
              ContextItems->front().Source.UserConfirmed,
          "ConversationMemory recall retains explicit confirmation through SQLite");
    Check(std::holds_alternative<std::monostate>(
              Memory.RevokeConsent(ContextValue, Grant.ConsentId, Now())),
          "consent revocation tombstones its persisted records");
  }

  {
    auto Store = OpenStore(Database.Path());
    if (!Store) {
      return;
    }
    SqliteMemoryStorageAdapter Adapter{*Store};
    const StoredMemory Scope = ScopeTemplate(ContextValue);
    const auto Active = Adapter.ListActive(Scope, Now());
    const auto* Records = StorageValue(Active);
    Check(Records != nullptr && Records->empty(),
          "consent tombstone remains effective after reopening the database");
    const auto Backup = Adapter.RecallBackup(Scope, Now());
    const auto* BackupRecords = StorageValue(Backup);
    Check(BackupRecords != nullptr && BackupRecords->empty(),
          "backup recall never returns tombstoned memory");

    ConversationMemory Recovered{Adapter};
    const ConsentGrant Grant = MakeGrant(ContextValue);
    Check(std::holds_alternative<std::monostate>(
              Recovered.RegisterConsent(Grant)),
          "recovery requires fresh in-memory consent registration");
    const auto Restore = Recovered.RestoreBackup(ContextValue, Now());
    const auto* Restored = std::get_if<std::size_t>(&Restore);
    Check(Restored != nullptr && *Restored == 0,
          "recovery does not resurrect revoked tombstones");
    const auto AfterRestore = Adapter.ListActive(Scope, Now());
    Check(StorageValue(AfterRestore) != nullptr &&
              StorageValue(AfterRestore)->empty(),
          "revoked memory remains absent after recovery");
  }
}

void TestMalformedStoredKindsFailClosed() {
  const std::string_view Kinds[] = {
      "UnknownCategory",
      "EDMemoryV2/IdentityPreference/1",
      "EDMemoryV1/UnknownCategory/1",
      "EDMemoryV1/IdentityPreference",
      "EDMemoryV1/IdentityPreference/true",
      "EDMemoryV1/IdentityPreference/1/trailing",
      "EDMemoryV2/UnknownCategory/0/P1:00",
      "EDMemoryV2/IdentityPreference/2/P1:00",
      "EDMemoryV2/IdentityPreference/0/P0:",
      "EDMemoryV2/IdentityPreference/0/P01:00",
      "EDMemoryV2/IdentityPreference/0/P1:0",
      "EDMemoryV2/IdentityPreference/0/P2:00",
      "EDMemoryV2/IdentityPreference/0/P1:0g",
      "EDMemoryV2/IdentityPreference/0/P1:AF",
      "EDMemoryV2/IdentityPreference/0/P1:0000",
      "EDMemoryV2/IdentityPreference/0/P1:00x",
      "EDMemoryV2/IdentityPreference/0/P18446744073709551616:",
      "EDMemoryV2/IdentityPreference/0/P2147483647:00",
      "EDMemoryV3/IdentityPreference/0/P1:00",
      "EDMemoryV999/IdentityPreference/0/P1:00",
  };
  for (std::size_t Index = 0; Index < std::size(Kinds); ++Index) {
    TemporaryDatabase Database{"MalformedKind" + std::to_string(Index)};
    if (!Database.IsReady()) {
      Check(false, "test database directory is available");
      continue;
    }
    auto Store = OpenStore(Database.Path());
    if (!Store) {
      continue;
    }

    const StoredMemory Scope = ScopeTemplate(Context());
    const ConversationScopeKey StorageScope{
        .ChannelId = Scope.ChannelId,
        .AccountId = Scope.AccountId,
        .ConversationId =
            "EmulativeDialog.ScopeV1.Direct/" + Scope.ConversationId,
        .PrincipalId = Scope.PrincipalId,
    };
    const std::int64_t Timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(
            Now().time_since_epoch())
            .count();
    MemoryRecord RawRecord{
        .MemoryId = "invalid-kind",
        .MemoryKind = std::string(Kinds[Index]),
        .Scope = StorageScope,
        .SubjectId = Scope.PrincipalId,
        .Content = "synthetic content",
        .SourceReference = "fixture-message",
        .CreatedAtUnixSeconds = Timestamp - 5,
        .UpdatedAtUnixSeconds = Timestamp,
        .ExpiresAtUnixSeconds = std::nullopt,
        .Provenance = StorageMemoryProvenance::UserExplicit,
        .ConsentId = "consent-raw",
        .DeleteState = MemoryDeleteState::Active,
        .Version = 1,
    };
    auto Transaction = Store->BeginTransaction();
    auto* TransactionValue =
        std::get_if<std::unique_ptr<SqliteStoreTransaction>>(&Transaction);
    Check(TransactionValue != nullptr,
          "raw test transaction starts for malformed kind fixture");
    if (TransactionValue == nullptr) {
      continue;
    }
    const auto Put = Store->PutRecord(**TransactionValue, RawRecord);
    Check(std::holds_alternative<std::monostate>(Put),
          "SQLite accepts raw kind text for adapter fail-closed testing");
    const auto Commit = (*TransactionValue)->Commit();
    Check(std::holds_alternative<std::monostate>(Commit),
          "malformed kind fixture commits");

    SqliteMemoryStorageAdapter Adapter{*Store};
    Check(HasStorageError(Adapter.ListActive(Scope, Now()),
                          MemoryStorageErrorCode::Failure),
          "unknown, malformed, or unsupported kind fails closed on active list");
    Check(HasStorageError(Adapter.RecallBackup(Scope, Now()),
                          MemoryStorageErrorCode::Failure),
          "unknown, malformed, or unsupported kind fails closed on backup recall");
  }
}

} // namespace

int main() {
  TestV1MemoryKindCompatibility();
  TestProposalIdRoundTripAndBackupRestore();
  TestBackupRestorePreservesExpiryAcrossRestart();
  TestCategoryAndProvenanceRoundTrip();
  TestScopeKindIsolation();
  TestTransactionalRollbackAndVersionConflict();
  TestNestedTransactionFailsClosed();
  TestInvalidInputsAndNoOp();
  TestConversationMemoryConsentTombstoneRestart();
  TestMalformedStoredKindsFailClosed();

  if (Failures != 0) {
    std::fprintf(stderr, "%d assertion(s) failed\n", Failures);
    return 1;
  }
  std::puts("SqliteMemoryStorageAdapter tests passed");
  return 0;
}
