#include "Source/Storage/SqliteStore.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using EmulativeDialog::Storage::BackupRecallRecord;
using EmulativeDialog::Storage::ConversationScopeKey;
using EmulativeDialog::Storage::MemoryDeleteState;
using EmulativeDialog::Storage::MemoryProvenance;
using EmulativeDialog::Storage::MemoryRecord;
using EmulativeDialog::Storage::SqliteStore;
using EmulativeDialog::Storage::SqliteStoreError;
using EmulativeDialog::Storage::SqliteStoreErrorCode;
using EmulativeDialog::Storage::SqliteStoreResult;
using EmulativeDialog::Storage::SqliteStoreTransaction;

int Failures = 0;

void Check(bool Condition, std::string_view Name) {
  if (Condition) {
    std::cout << "PASS " << Name << '\n';
  } else {
    ++Failures;
    std::cerr << "FAIL " << Name << '\n';
  }
}

template <typename Value>
const Value* AsValue(const SqliteStoreResult<Value>& Result) {
  return std::get_if<Value>(&Result);
}

template <typename Value>
const SqliteStoreError* AsError(const SqliteStoreResult<Value>& Result) {
  return std::get_if<SqliteStoreError>(&Result);
}

std::int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::filesystem::path TestDataDirectory() {
  const char* Environment =
      std::getenv("EMULATIVEDIALOG_TEST_DATA_DIR");
  if (Environment == nullptr || *Environment == '\0') {
    return {};
  }
  return std::filesystem::path(Environment);
}

std::filesystem::path NewDatabasePath(std::string_view Label) {
  const auto Directory = TestDataDirectory();
  if (Directory.empty()) {
    return {};
  }
  std::error_code Ignored;
  std::filesystem::create_directories(Directory, Ignored);
  auto Path = Directory / (std::string(Label) + ".sqlite3");
  std::filesystem::remove(Path, Ignored);
  return Path;
}

ConversationScopeKey Scope(std::string Channel, std::string Principal) {
  return ConversationScopeKey{
      .ChannelId = std::move(Channel),
      .AccountId = "account-1",
      .ConversationId = "conversation-1",
      .PrincipalId = std::move(Principal),
  };
}

ConversationScopeKey ScopeA() { return Scope("channel-qq", "principal-a"); }

ConversationScopeKey ScopeB() { return Scope("channel-qq", "principal-b"); }

MemoryRecord MakeRecord(std::string MemoryId, const ConversationScopeKey& Key,
                        std::uint64_t Version = 1) {
  const auto Timestamp = Now();
  return MemoryRecord{
      .MemoryId = std::move(MemoryId),
      .MemoryKind = "Preference",
      .Scope = Key,
      .SubjectId = Key.PrincipalId,
      .Content = "bounded memory content",
      .SourceReference = "message-1",
      .CreatedAtUnixSeconds = Timestamp - 60,
      .UpdatedAtUnixSeconds = Timestamp - 60,
      .ExpiresAtUnixSeconds = std::nullopt,
      .Provenance = MemoryProvenance::UserExplicit,
      .ConsentId = "consent-1",
      .DeleteState = MemoryDeleteState::Active,
      .Version = Version,
  };
}

int CreateRawDatabase(const std::filesystem::path& Path, const char* Sql) {
  sqlite3* Database = nullptr;
  if (sqlite3_open_v2(Path.string().c_str(), &Database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    sqlite3_close_v2(Database);
    return 1;
  }
  const int Result = sqlite3_exec(Database, Sql, nullptr, nullptr, nullptr);
  sqlite3_close_v2(Database);
  return Result == SQLITE_OK ? 0 : 1;
}

void TestSchemaAndRoundTrip() {
  const auto Path = NewDatabasePath("SchemaAndRoundTrip");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  Check(Store != nullptr, "schema open creates store");
  if (Store == nullptr) {
    return;
  }
  auto Transaction = (*Store)->BeginTransaction();
  const auto* TransactionValue = AsValue(Transaction);
  Check(TransactionValue != nullptr, "schema begin transaction");
  if (TransactionValue == nullptr) {
    return;
  }
  auto Record = MakeRecord("memory-1", ScopeA());
  Check(AsValue((*Store)->PutRecord(**TransactionValue, Record)) != nullptr,
        "schema record write");
  Check(AsValue((*TransactionValue)->Commit()) != nullptr,
        "schema transaction commit");

  auto Loaded = (*Store)->GetActiveRecord(ScopeA(), "memory-1", Now());
  const auto* LoadedValue = AsValue(Loaded);
  Check(LoadedValue != nullptr, "schema record round trip");
  if (LoadedValue != nullptr) {
    Check((*LoadedValue).Content == Record.Content,
          "schema record content preserved");
    Check((*LoadedValue).ConsentId == "consent-1",
          "schema consent preserved");
  }
  Check(AsValue((*Store)->ListActiveRecords(ScopeA(), Now())) != nullptr,
        "schema list active records");
}

void TestScopeIsolation() {
  const auto Path = NewDatabasePath("ScopeIsolation");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "scope store open");
    return;
  }
  auto Transaction = (*Store)->BeginTransaction();
  auto Commit = (*Store)->PutRecord(**AsValue(Transaction),
                                    MakeRecord("memory-1", ScopeA()));
  Check(AsValue(Commit) != nullptr, "scope write");
  Check(AsValue((**AsValue(Transaction)).Commit()) != nullptr,
        "scope commit");

  auto CrossScope = (*Store)->GetActiveRecord(ScopeB(), "memory-1", Now());
  Check(AsError(CrossScope)->Code == SqliteStoreErrorCode::RecordNotFound,
        "scope foreign key not readable");

  auto TombstoneTransaction = (*Store)->BeginTransaction();
  auto CrossTombstone = (*Store)->TombstoneRecord(
      **AsValue(TombstoneTransaction), ScopeB(), "memory-1", Now());
  Check(AsError(CrossTombstone)->Code ==
            SqliteStoreErrorCode::RecordNotFound,
        "scope foreign tombstone rejected");
  Check(AsValue((**AsValue(TombstoneTransaction)).Rollback()) != nullptr,
        "scope rollback");
  auto StillActive = (*Store)->GetActiveRecord(ScopeA(), "memory-1", Now());
  Check(AsValue(StillActive) != nullptr, "scope isolation preserves record");

  auto IncompleteScope = ScopeA();
  IncompleteScope.PrincipalId.clear();
  auto Invalid = (*Store)->GetActiveRecord(IncompleteScope, "memory-1", Now());
  Check(AsError(Invalid)->Code == SqliteStoreErrorCode::InvalidArgument,
        "scope incomplete rejected");
}

void TestTransactionsRequiredAndNested() {
  const auto Path = NewDatabasePath("TransactionsRequiredAndNested");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "transaction store open");
    return;
  }
  auto Outer = (*Store)->BeginTransaction();
  const auto* OuterValue = AsValue(Outer);
  Check(OuterValue != nullptr, "transaction begin");
  auto Nested = (*Store)->BeginTransaction();
  Check(AsError(Nested)->Code ==
            SqliteStoreErrorCode::NestedTransactionRejected,
        "nested transaction rejected");
  Check(AsValue((**OuterValue).Rollback()) != nullptr, "nested rollback");
  auto Reuse = (*Store)->PutRecord(**OuterValue,
                                   MakeRecord("memory-1", ScopeA()));
  Check(AsError(Reuse)->Code == SqliteStoreErrorCode::TransactionRequired,
        "inactive transaction rejected");
}

void TestRollbackAndDestructorRollback() {
  const auto Path = NewDatabasePath("RollbackAndDestructorRollback");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "rollback store open");
    return;
  }
  {
    auto Transaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction),
                                      MakeRecord("memory-1", ScopeA()))) !=
              nullptr,
          "rollback write");
    Check(AsValue((**AsValue(Transaction)).Rollback()) != nullptr,
          "rollback explicit");
  }
  Check(AsError((*Store)->GetActiveRecord(ScopeA(), "memory-1", Now()))
            ->Code == SqliteStoreErrorCode::RecordNotFound,
        "rollback removes write");

  {
    auto Transaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction),
                                      MakeRecord("memory-2", ScopeA()))) !=
              nullptr,
          "destructor rollback write");
  }
  Check(AsError((*Store)->GetActiveRecord(ScopeA(), "memory-2", Now()))
            ->Code == SqliteStoreErrorCode::RecordNotFound,
        "destructor rollback removes write");
}

void TestVersionConflict() {
  const auto Path = NewDatabasePath("VersionConflict");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "version store open");
    return;
  }
  {
    auto Transaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->PutRecord(
              **AsValue(Transaction),
              MakeRecord("memory-1", ScopeA(), 2))) != nullptr,
          "version write newer");
    Check(AsValue((**AsValue(Transaction)).Commit()) != nullptr,
          "version commit");
  }
  {
    auto Transaction = (*Store)->BeginTransaction();
    auto Stale = (*Store)->PutRecord(
        **AsValue(Transaction), MakeRecord("memory-1", ScopeA(), 1));
    Check(AsError(Stale)->Code == SqliteStoreErrorCode::VersionConflict,
          "stale version rejected");
    Check(AsValue((**AsValue(Transaction)).Rollback()) != nullptr,
          "version rollback");
  }
  auto Current = (*Store)->GetActiveRecord(ScopeA(), "memory-1", Now());
  Check(AsValue(Current) != nullptr && (*AsValue(Current)).Version == 2,
        "version preserved");
}

void TestTombstoneAndBackupRecall() {
  const auto Path = NewDatabasePath("TombstoneAndBackupRecall");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "tombstone store open");
    return;
  }
  {
    auto Transaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction),
                                      MakeRecord("memory-1", ScopeA()))) !=
              nullptr,
          "tombstone write");
    Check(AsValue((**AsValue(Transaction)).Commit()) != nullptr,
          "tombstone write commit");
  }
  Check(AsValue((*Store)->RecallBackupForScope(ScopeA(), Now())) != nullptr &&
            (*AsValue((*Store)->RecallBackupForScope(ScopeA(), Now()))).size() ==
                1,
        "tombstone backup before delete");
  {
    auto Transaction = (*Store)->BeginTransaction();
    auto Tombstoned = (*Store)->TombstoneRecord(
        **AsValue(Transaction), ScopeA(), "memory-1", Now());
    Check(AsValue(Tombstoned) != nullptr, "tombstone apply");
    Check(AsValue((**AsValue(Transaction)).Commit()) != nullptr,
          "tombstone commit");
  }
  Check(AsError((*Store)->GetActiveRecord(ScopeA(), "memory-1", Now()))->Code ==
            SqliteStoreErrorCode::RecordNotFound,
        "tombstone hidden from lookup");
  auto Listed = (*Store)->ListActiveRecords(ScopeA(), Now());
  Check(AsValue(Listed) != nullptr && (*AsValue(Listed)).empty(),
        "tombstone hidden from list");
  auto Backup = (*Store)->RecallBackupForScope(ScopeA(), Now());
  Check(AsValue(Backup) != nullptr && (*AsValue(Backup)).empty(),
        "tombstone hidden from backup");

  auto Transaction = (*Store)->BeginTransaction();
  auto Again = (*Store)->TombstoneRecord(**AsValue(Transaction), ScopeA(),
                                         "memory-1", Now());
  Check(AsError(Again)->Code == SqliteStoreErrorCode::RecordNotFound,
        "tombstone repeated rejected");
  Check(AsValue((**AsValue(Transaction)).Rollback()) != nullptr,
        "tombstone repeated rollback");
}

void TestExpiryFiltering() {
  const auto Path = NewDatabasePath("ExpiryFiltering");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "expiry store open");
    return;
  }
  const auto Timestamp = Now();
  auto Record = MakeRecord("memory-1", ScopeA());
  Record.CreatedAtUnixSeconds = Timestamp - 100;
  Record.UpdatedAtUnixSeconds = Timestamp - 100;
  Record.ExpiresAtUnixSeconds = Timestamp - 50;
  {
    auto Transaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction), Record)) !=
              nullptr,
          "expiry write");
    Check(AsValue((**AsValue(Transaction)).Commit()) != nullptr,
          "expiry commit");
  }
  Check(AsError((*Store)->GetActiveRecord(ScopeA(), "memory-1", Timestamp))
            ->Code == SqliteStoreErrorCode::RecordNotFound,
        "expired record hidden");
  auto Backup = (*Store)->RecallBackupForScope(ScopeA(), Timestamp);
  Check(AsValue(Backup) != nullptr && (*AsValue(Backup)).empty(),
        "expired record hidden from backup");
  auto BeforeExpiry =
      (*Store)->GetActiveRecord(ScopeA(), "memory-1", Timestamp - 60);
  Check(AsValue(BeforeExpiry) != nullptr, "record visible before expiry");
}

void TestBackupScopeLimited() {
  const auto Path = NewDatabasePath("BackupScopeLimited");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "backup scope store open");
    return;
  }
  {
    auto Transaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction),
                                      MakeRecord("memory-a", ScopeA()))) !=
              nullptr,
          "backup scope write a");
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction),
                                      MakeRecord("memory-b", ScopeB()))) !=
              nullptr,
          "backup scope write b");
    Check(AsValue((**AsValue(Transaction)).Commit()) != nullptr,
          "backup scope commit");
  }
  auto Backup = (*Store)->RecallBackupForScope(ScopeA(), Now());
  const auto* Records = AsValue(Backup);
  Check(Records != nullptr && Records->size() == 1,
        "backup recall limited to scope");
  if (Records != nullptr && Records->size() == 1) {
    const BackupRecallRecord& Record = Records->front();
    Check(Record.MemoryId == "memory-a", "backup recall record identity");
    Check(Record.Scope == ScopeA(), "backup recall scope");
    Check(Record.ConsentId == "consent-1", "backup recall consent");
  }
}

void TestRestartRecovery() {
  const auto Path = NewDatabasePath("RestartRecovery");
  {
    auto Opened = SqliteStore::Open(Path);
    const auto* Store = AsValue(Opened);
    if (Store == nullptr) {
      Check(false, "restart store open");
      return;
    }
    auto Transaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction),
                                      MakeRecord("memory-1", ScopeA()))) !=
              nullptr,
          "restart write a");
    Check(AsValue((*Store)->PutRecord(**AsValue(Transaction),
                                      MakeRecord("memory-2", ScopeA()))) !=
              nullptr,
          "restart write b");
    Check(AsValue((**AsValue(Transaction)).Commit()) != nullptr,
          "restart commit");
    auto TombstoneTransaction = (*Store)->BeginTransaction();
    Check(AsValue((*Store)->TombstoneRecord(
              **AsValue(TombstoneTransaction), ScopeA(), "memory-2", Now())) !=
              nullptr,
          "restart tombstone");
    Check(AsValue((**AsValue(TombstoneTransaction)).Commit()) != nullptr,
          "restart tombstone commit");
  }
  auto Reopened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Reopened);
  Check(Store != nullptr, "restart reopen existing schema");
  if (Store == nullptr) {
    return;
  }
  Check(AsValue((*Store)->GetActiveRecord(ScopeA(), "memory-1", Now())) !=
            nullptr,
        "restart active record recovered");
  Check(AsError((*Store)->GetActiveRecord(ScopeA(), "memory-2", Now()))->Code ==
            SqliteStoreErrorCode::RecordNotFound,
        "restart tombstone remains effective");
}

void TestSchemaVersionMismatchFailClosed() {
  const auto Path = NewDatabasePath("SchemaVersionMismatchFailClosed");
  Check(CreateRawDatabase(
            Path,
            "CREATE TABLE SchemaVersion(Version INTEGER NOT NULL);"
            "INSERT INTO SchemaVersion(Version) VALUES(2);"
            "CREATE TABLE MemoryRecords(Dummy INTEGER);") == 0,
        "schema fixture created");
  auto Opened = SqliteStore::Open(Path);
  Check(AsError(Opened) != nullptr &&
            AsError(Opened)->Code ==
                SqliteStoreErrorCode::SchemaVersionMismatch,
        "schema version mismatch fail closed");
  sqlite3* Database = nullptr;
  Check(sqlite3_open_v2(Path.string().c_str(), &Database,
                        SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
        "mismatched database still readable");
  sqlite3_close_v2(Database);
}

void TestForeignSchemaRejected() {
  const auto Path = NewDatabasePath("ForeignSchemaRejected");
  Check(CreateRawDatabase(Path,
                          "CREATE TABLE ForeignTable(Id INTEGER PRIMARY KEY);") ==
            0,
        "foreign fixture created");
  auto Opened = SqliteStore::Open(Path);
  Check(AsError(Opened) != nullptr &&
            AsError(Opened)->Code ==
                SqliteStoreErrorCode::SchemaVersionMismatch,
        "foreign schema rejected");
}

void TestInvalidArguments() {
  const auto Path = NewDatabasePath("InvalidArguments");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "invalid argument store open");
    return;
  }
  Check(AsError(SqliteStore::Open({}))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "empty database path rejected");
  auto Transaction = (*Store)->BeginTransaction();
  auto& TransactionValue = **AsValue(Transaction);

  auto MissingConsent = MakeRecord("memory-1", ScopeA());
  MissingConsent.ConsentId.clear();
  Check(AsError((*Store)->PutRecord(TransactionValue, MissingConsent))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "missing consent rejected");

  auto EmptyContent = MakeRecord("memory-1", ScopeA());
  EmptyContent.Content.clear();
  Check(AsError((*Store)->PutRecord(TransactionValue, EmptyContent))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "empty content rejected");

  auto InvalidUtf8 = MakeRecord("memory-1", ScopeA());
  InvalidUtf8.Content = std::string("\xC3\x28", 2);
  Check(AsError((*Store)->PutRecord(TransactionValue, InvalidUtf8))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "invalid utf8 rejected");

  auto BadTimestamps = MakeRecord("memory-1", ScopeA());
  BadTimestamps.UpdatedAtUnixSeconds = BadTimestamps.CreatedAtUnixSeconds - 1;
  Check(AsError((*Store)->PutRecord(TransactionValue, BadTimestamps))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "backwards timestamps rejected");

  auto BadExpiry = MakeRecord("memory-1", ScopeA());
  BadExpiry.ExpiresAtUnixSeconds = BadExpiry.CreatedAtUnixSeconds;
  Check(AsError((*Store)->PutRecord(TransactionValue, BadExpiry))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "expiry before creation rejected");

  auto ZeroVersion = MakeRecord("memory-1", ScopeA(), 0);
  Check(AsError((*Store)->PutRecord(TransactionValue, ZeroVersion))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "zero version rejected");

  auto PreTombstoned = MakeRecord("memory-1", ScopeA());
  PreTombstoned.DeleteState = MemoryDeleteState::Tombstoned;
  Check(AsError((*Store)->PutRecord(TransactionValue, PreTombstoned))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "pre-tombstoned write rejected");

  auto EmptyScope = MakeRecord("memory-1", ScopeA());
  EmptyScope.Scope.ChannelId.clear();
  Check(AsError((*Store)->PutRecord(TransactionValue, EmptyScope))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "empty scope write rejected");

  Check(AsError((*Store)->PutRecord(
                    TransactionValue,
                    MakeRecord(std::string(200, 'x'), ScopeA())))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "overlong memory id rejected");
  Check(AsValue(TransactionValue.Rollback()) != nullptr,
        "invalid argument rollback");
}

void TestMissingAndInvalidQueryArguments() {
  const auto Path = NewDatabasePath("MissingAndInvalidQueryArguments");
  auto Opened = SqliteStore::Open(Path);
  const auto* Store = AsValue(Opened);
  if (Store == nullptr) {
    Check(false, "query argument store open");
    return;
  }
  Check(AsError((*Store)->GetActiveRecord(ScopeA(), "missing", Now()))->Code ==
            SqliteStoreErrorCode::RecordNotFound,
        "missing record rejected");
  Check(AsError((*Store)->GetActiveRecord(ScopeA(), std::string(200, 'x'),
                                          Now()))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "overlong query id rejected");
  Check(AsError((*Store)->ListActiveRecords({}, Now()))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "incomplete list scope rejected");
  Check(AsError((*Store)->RecallBackupForScope({}, Now()))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "incomplete backup scope rejected");
  auto Transaction = (*Store)->BeginTransaction();
  Check(AsError((*Store)->TombstoneRecord(**AsValue(Transaction), ScopeA(),
                                          "missing", Now()))->Code ==
            SqliteStoreErrorCode::RecordNotFound,
        "missing tombstone rejected");
  Check(AsError((*Store)->TombstoneRecord(**AsValue(Transaction), ScopeA(), "",
                                          Now()))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "empty tombstone id rejected");
  Check(AsError((*Store)->TombstoneRecord(
                    **AsValue(Transaction), ScopeA(), "missing", 0))->Code ==
            SqliteStoreErrorCode::InvalidArgument,
        "invalid tombstone time rejected");
  Check(AsValue((**AsValue(Transaction)).Rollback()) != nullptr,
        "query argument rollback");
}

} // namespace

int main() {
  if (TestDataDirectory().empty()) {
    std::cerr << "EMULATIVEDIALOG_TEST_DATA_DIR must be set\n";
    return 2;
  }
  TestSchemaAndRoundTrip();
  TestScopeIsolation();
  TestTransactionsRequiredAndNested();
  TestRollbackAndDestructorRollback();
  TestVersionConflict();
  TestTombstoneAndBackupRecall();
  TestExpiryFiltering();
  TestBackupScopeLimited();
  TestRestartRecovery();
  TestSchemaVersionMismatchFailClosed();
  TestForeignSchemaRejected();
  TestInvalidArguments();
  TestMissingAndInvalidQueryArguments();
  if (Failures != 0) {
    std::cerr << Failures << " checks failed\n";
    return 1;
  }
  std::cout << "All SqliteStore checks passed\n";
  return 0;
}
