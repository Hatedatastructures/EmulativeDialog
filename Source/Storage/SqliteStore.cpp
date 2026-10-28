#include "Source/Storage/SqliteStore.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace EmulativeDialog::Storage {
namespace {

SqliteStoreError Failure(SqliteStoreErrorCode Code, std::string Detail) {
  return SqliteStoreError{.Code = Code, .Detail = std::move(Detail)};
}

SqliteStoreError DatabaseFailure(const char* Context, sqlite3* Database) {
  std::string Detail = Context != nullptr ? Context : "database";
  Detail.append(": ");
  Detail.append(Database != nullptr ? sqlite3_errmsg(Database)
                                    : "database is not open");
  return Failure(SqliteStoreErrorCode::DatabaseFailure, std::move(Detail));
}

bool IsBlank(std::string_view Value) { return Value.empty(); }

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

const char* ProvenanceName(MemoryProvenance Provenance) noexcept {
  switch (Provenance) {
  case MemoryProvenance::UserExplicit:
    return "UserExplicit";
  case MemoryProvenance::UserConfirmed:
    return "UserConfirmed";
  case MemoryProvenance::AuthorizedSummary:
    return "AuthorizedSummary";
  }
  return "UserExplicit";
}

std::optional<MemoryProvenance> ParseProvenance(std::string_view Value) {
  if (Value == "UserExplicit") {
    return MemoryProvenance::UserExplicit;
  }
  if (Value == "UserConfirmed") {
    return MemoryProvenance::UserConfirmed;
  }
  if (Value == "AuthorizedSummary") {
    return MemoryProvenance::AuthorizedSummary;
  }
  return std::nullopt;
}

std::optional<SqliteStoreError> ValidateScope(
    const ConversationScopeKey& Scope) {
  if (!Scope.IsComplete()) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "scope key requires channel, account, conversation, and "
                   "principal identifiers");
  }
  return std::nullopt;
}

std::optional<SqliteStoreError> ValidateRecord(const MemoryRecord& Record) {
  if (const auto ScopeError = ValidateScope(Record.Scope);
      ScopeError.has_value()) {
    return ScopeError;
  }
  if (IsBlank(Record.MemoryId) ||
      Record.MemoryId.size() > MaximumMemoryIdLength) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "memory id must be non-empty and bounded");
  }
  if (IsBlank(Record.MemoryKind) || IsBlank(Record.SubjectId) ||
      IsBlank(Record.ConsentId) || IsBlank(Record.SourceReference)) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "memory kind, subject, consent, and source are required");
  }
  if (IsBlank(Record.Content) ||
      Record.Content.size() > MaximumMemoryContentLength ||
      !IsValidUtf8(Record.Content)) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "memory content must be bounded valid UTF-8");
  }
  if (Record.CreatedAtUnixSeconds <= 0 ||
      Record.UpdatedAtUnixSeconds < Record.CreatedAtUnixSeconds) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "record timestamps are inconsistent");
  }
  if (Record.ExpiresAtUnixSeconds.has_value() &&
      *Record.ExpiresAtUnixSeconds <= Record.CreatedAtUnixSeconds) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "expiry must be later than creation");
  }
  if (Record.Version == 0) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "record version must be positive");
  }
  if (Record.DeleteState != MemoryDeleteState::Active) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "records must be written active; use TombstoneRecord");
  }
  return std::nullopt;
}

const char* SchemaScript = R"sql(
CREATE TABLE SchemaVersion(
  Version INTEGER NOT NULL PRIMARY KEY CHECK(Version > 0)
);
INSERT INTO SchemaVersion(Version) VALUES(1);
CREATE TABLE MemoryRecords(
  ChannelId TEXT NOT NULL CHECK(length(ChannelId) > 0),
  AccountId TEXT NOT NULL CHECK(length(AccountId) > 0),
  ConversationId TEXT NOT NULL CHECK(length(ConversationId) > 0),
  PrincipalId TEXT NOT NULL CHECK(length(PrincipalId) > 0),
  MemoryId TEXT NOT NULL CHECK(length(MemoryId) BETWEEN 1 AND 128),
  MemoryKind TEXT NOT NULL CHECK(length(MemoryKind) > 0),
  SubjectId TEXT NOT NULL CHECK(length(SubjectId) > 0),
  Content TEXT NOT NULL CHECK(length(Content) BETWEEN 1 AND 8192),
  SourceReference TEXT NOT NULL CHECK(length(SourceReference) > 0),
  CreatedAt INTEGER NOT NULL CHECK(CreatedAt > 0),
  UpdatedAt INTEGER NOT NULL CHECK(UpdatedAt >= CreatedAt),
  ExpiresAt INTEGER CHECK(ExpiresAt IS NULL OR ExpiresAt > CreatedAt),
  Provenance TEXT NOT NULL
    CHECK(Provenance IN ('UserExplicit','UserConfirmed','AuthorizedSummary')),
  ConsentId TEXT NOT NULL CHECK(length(ConsentId) > 0),
  DeleteState TEXT NOT NULL CHECK(DeleteState IN ('Active','Tombstoned')),
  Version INTEGER NOT NULL CHECK(Version > 0),
  PRIMARY KEY(ChannelId, AccountId, ConversationId, PrincipalId, MemoryId)
);
CREATE INDEX MemoryRecordsScopeState
  ON MemoryRecords(ChannelId, AccountId, ConversationId, PrincipalId,
                   DeleteState, ExpiresAt);
)sql";

std::string ReadText(sqlite3_stmt* Statement, int Column) {
  const auto* Text =
      reinterpret_cast<const char*>(sqlite3_column_text(Statement, Column));
  return Text != nullptr ? std::string(Text) : std::string();
}

std::int64_t ReadInt64(sqlite3_stmt* Statement, int Column) {
  return sqlite3_column_int64(Statement, Column);
}

std::uint64_t ReadUint64(sqlite3_stmt* Statement, int Column) {
  return static_cast<std::uint64_t>(sqlite3_column_int64(Statement, Column));
}

ConversationScopeKey ReadScope(sqlite3_stmt* Statement, int FirstColumn) {
  ConversationScopeKey Scope;
  Scope.ChannelId = ReadText(Statement, FirstColumn);
  Scope.AccountId = ReadText(Statement, FirstColumn + 1);
  Scope.ConversationId = ReadText(Statement, FirstColumn + 2);
  Scope.PrincipalId = ReadText(Statement, FirstColumn + 3);
  return Scope;
}

std::optional<MemoryProvenance> ReadProvenance(sqlite3_stmt* Statement,
                                               int Column) {
  return ParseProvenance(ReadText(Statement, Column));
}

struct StatementHandle final {
  sqlite3_stmt* Handle{nullptr};
  ~StatementHandle() {
    if (Handle != nullptr) {
      sqlite3_finalize(Handle);
    }
  }
};

constexpr const char* RecordColumns =
    "ChannelId, AccountId, ConversationId, PrincipalId, MemoryId, MemoryKind, "
    "SubjectId, Content, SourceReference, CreatedAt, UpdatedAt, ExpiresAt, "
    "Provenance, ConsentId, Version";

std::optional<SqliteStoreError> MapHydration(
    std::string_view Context, const std::optional<MemoryProvenance>& Provenance) {
  if (!Provenance.has_value()) {
    return Failure(SqliteStoreErrorCode::DatabaseFailure,
                   std::string(Context) + ": stored provenance is invalid");
  }
  return std::nullopt;
}

SqliteStoreResult<MemoryRecord> HydrateMemoryRecord(sqlite3_stmt* Statement) {
  MemoryRecord Record;
  Record.Scope = ReadScope(Statement, 0);
  Record.MemoryId = ReadText(Statement, 4);
  Record.MemoryKind = ReadText(Statement, 5);
  Record.SubjectId = ReadText(Statement, 6);
  Record.Content = ReadText(Statement, 7);
  Record.SourceReference = ReadText(Statement, 8);
  Record.CreatedAtUnixSeconds = ReadInt64(Statement, 9);
  Record.UpdatedAtUnixSeconds = ReadInt64(Statement, 10);
  if (sqlite3_column_type(Statement, 11) != SQLITE_NULL) {
    Record.ExpiresAtUnixSeconds = ReadInt64(Statement, 11);
  }
  const auto Provenance = ReadProvenance(Statement, 12);
  if (const auto Error = MapHydration("memory record", Provenance);
      Error.has_value()) {
    return *Error;
  }
  Record.Provenance = *Provenance;
  Record.ConsentId = ReadText(Statement, 13);
  Record.Version = ReadUint64(Statement, 14);
  Record.DeleteState = MemoryDeleteState::Active;
  return Record;
}

SqliteStoreResult<BackupRecallRecord> HydrateBackupRecord(
    sqlite3_stmt* Statement) {
  BackupRecallRecord Record;
  Record.Scope = ReadScope(Statement, 0);
  Record.MemoryId = ReadText(Statement, 4);
  Record.MemoryKind = ReadText(Statement, 5);
  Record.SubjectId = ReadText(Statement, 6);
  Record.Content = ReadText(Statement, 7);
  Record.SourceReference = ReadText(Statement, 8);
  Record.CreatedAtUnixSeconds = ReadInt64(Statement, 9);
  const auto Provenance = ReadProvenance(Statement, 12);
  if (const auto Error = MapHydration("backup record", Provenance);
      Error.has_value()) {
    return *Error;
  }
  Record.Provenance = *Provenance;
  Record.ConsentId = ReadText(Statement, 13);
  Record.Version = ReadUint64(Statement, 14);
  return Record;
}

void BindText(sqlite3_stmt* Statement, int Index, std::string_view Value) {
  sqlite3_bind_text(Statement, Index, Value.data(),
                    static_cast<int>(Value.size()), SQLITE_TRANSIENT);
}

void BindScope(sqlite3_stmt* Statement, int FirstIndex,
               const ConversationScopeKey& Scope) {
  BindText(Statement, FirstIndex, Scope.ChannelId);
  BindText(Statement, FirstIndex + 1, Scope.AccountId);
  BindText(Statement, FirstIndex + 2, Scope.ConversationId);
  BindText(Statement, FirstIndex + 3, Scope.PrincipalId);
}

} // namespace

bool ConversationScopeKey::IsComplete() const noexcept {
  return !ChannelId.empty() && !AccountId.empty() && !ConversationId.empty() &&
         !PrincipalId.empty();
}

SqliteStoreTransaction::SqliteStoreTransaction(SqliteStore& Owner) noexcept
    : Owner_(&Owner) {}

SqliteStoreTransaction::~SqliteStoreTransaction() {
  if (Active_ && Owner_ != nullptr) {
    static_cast<void>(Owner_->ExecuteStatement("ROLLBACK"));
    Owner_->FinishTransaction(false);
    Active_ = false;
  }
}

SqliteStoreResult<std::monostate> SqliteStoreTransaction::Commit() {
  if (!Active_ || Owner_ == nullptr) {
    return Failure(SqliteStoreErrorCode::TransactionRequired,
                   "transaction is not active");
  }
  auto Result = Owner_->ExecuteStatement("COMMIT");
  if (std::holds_alternative<SqliteStoreError>(Result)) {
    static_cast<void>(Owner_->ExecuteStatement("ROLLBACK"));
    Active_ = false;
    Owner_->FinishTransaction(false);
    return std::get<SqliteStoreError>(std::move(Result));
  }
  Active_ = false;
  Owner_->FinishTransaction(true);
  return std::monostate{};
}

SqliteStoreResult<std::monostate> SqliteStoreTransaction::Rollback() {
  if (!Active_ || Owner_ == nullptr) {
    return Failure(SqliteStoreErrorCode::TransactionRequired,
                   "transaction is not active");
  }
  auto Result = Owner_->ExecuteStatement("ROLLBACK");
  Active_ = false;
  Owner_->FinishTransaction(false);
  return Result;
}

bool SqliteStoreTransaction::IsActive() const noexcept { return Active_; }

SqliteStore::~SqliteStore() {
  if (Database_ != nullptr) {
    sqlite3_close_v2(Database_);
    Database_ = nullptr;
  }
}

SqliteStoreResult<std::unique_ptr<SqliteStore>> SqliteStore::Open(
    const std::filesystem::path& DatabasePath) {
  if (DatabasePath.empty()) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "database path is required");
  }
  const auto Parent = DatabasePath.parent_path();
  if (!Parent.empty()) {
    std::error_code DirectoryError;
    std::filesystem::create_directories(Parent, DirectoryError);
    if (DirectoryError) {
      return Failure(SqliteStoreErrorCode::DatabaseFailure,
                     "database directory could not be created");
    }
  }

  std::unique_ptr<SqliteStore> Store(new SqliteStore());
  const int OpenResult =
      sqlite3_open_v2(DatabasePath.string().c_str(), &Store->Database_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                      nullptr);
  if (OpenResult != SQLITE_OK) {
    const auto Error =
        DatabaseFailure("database open failed", Store->Database_);
    if (Store->Database_ != nullptr) {
      sqlite3_close_v2(Store->Database_);
      Store->Database_ = nullptr;
    }
    return Error;
  }
  sqlite3_busy_timeout(Store->Database_, 5000);

  StatementHandle CountStatement;
  if (sqlite3_prepare_v2(
          Store->Database_,
          "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
          "name NOT LIKE 'sqlite_%'",
          -1, &CountStatement.Handle, nullptr) != SQLITE_OK) {
    return DatabaseFailure("schema inspection failed", Store->Database_);
  }
  const bool ExistingSchema =
      sqlite3_step(CountStatement.Handle) == SQLITE_ROW &&
      sqlite3_column_int64(CountStatement.Handle, 0) > 0;

  if (!ExistingSchema) {
    if (const auto Error = Store->ExecuteStatement(SchemaScript);
        std::holds_alternative<SqliteStoreError>(Error)) {
      return std::get<SqliteStoreError>(std::move(Error));
    }
  } else {
    StatementHandle VersionStatement;
    if (sqlite3_prepare_v2(Store->Database_,
                           "SELECT Version FROM SchemaVersion", -1,
                           &VersionStatement.Handle, nullptr) != SQLITE_OK) {
      return Failure(SqliteStoreErrorCode::SchemaVersionMismatch,
                     "existing database has no recognized schema version");
    }
    if (sqlite3_step(VersionStatement.Handle) != SQLITE_ROW ||
        sqlite3_column_int64(VersionStatement.Handle, 0) !=
            SqliteMemorySchemaVersion ||
        sqlite3_step(VersionStatement.Handle) != SQLITE_DONE) {
      return Failure(SqliteStoreErrorCode::SchemaVersionMismatch,
                     "existing database schema version is not supported");
    }
    StatementHandle TableStatement;
    if (sqlite3_prepare_v2(
            Store->Database_,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
            "name='MemoryRecords'",
            -1, &TableStatement.Handle, nullptr) != SQLITE_OK ||
        sqlite3_step(TableStatement.Handle) != SQLITE_ROW ||
        sqlite3_column_int64(TableStatement.Handle, 0) != 1) {
      return Failure(SqliteStoreErrorCode::SchemaVersionMismatch,
                     "existing database is missing required tables");
    }
  }

  return Store;
}

bool SqliteStore::IsTransactionUsable(
    const SqliteStoreTransaction& Transaction) const noexcept {
  return Transaction.Owner_ == this && Transaction.Active_ &&
         TransactionActive_;
}

SqliteStoreResult<std::monostate> SqliteStore::ExecuteStatement(
    const char* Statement) {
  char* ErrorMessage = nullptr;
  const int Result =
      sqlite3_exec(Database_, Statement, nullptr, nullptr, &ErrorMessage);
  if (Result != SQLITE_OK) {
    std::string Detail = "statement failed";
    if (ErrorMessage != nullptr) {
      Detail.append(": ");
      Detail.append(ErrorMessage);
      sqlite3_free(ErrorMessage);
    }
    return Failure(SqliteStoreErrorCode::DatabaseFailure, std::move(Detail));
  }
  return std::monostate{};
}

void SqliteStore::FinishTransaction(bool /*Committed*/) noexcept {
  TransactionActive_ = false;
}

SqliteStoreResult<std::unique_ptr<SqliteStoreTransaction>>
SqliteStore::BeginTransaction() {
  if (TransactionActive_) {
    return Failure(SqliteStoreErrorCode::NestedTransactionRejected,
                   "a transaction is already active on this store");
  }
  if (const auto Error = ExecuteStatement("BEGIN IMMEDIATE");
      std::holds_alternative<SqliteStoreError>(Error)) {
    return std::get<SqliteStoreError>(std::move(Error));
  }
  TransactionActive_ = true;
  return std::unique_ptr<SqliteStoreTransaction>(
      new SqliteStoreTransaction(*this));
}

SqliteStoreResult<std::monostate> SqliteStore::PutRecord(
    SqliteStoreTransaction& Transaction, const MemoryRecord& Record) {
  if (!IsTransactionUsable(Transaction)) {
    return Failure(SqliteStoreErrorCode::TransactionRequired,
                   "an active transaction on this store is required");
  }
  if (const auto Validation = ValidateRecord(Record);
      Validation.has_value()) {
    return *Validation;
  }

  StatementHandle Statement;
  const std::string Sql =
      "INSERT INTO MemoryRecords(" + std::string(RecordColumns) +
      ", DeleteState) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'Active') "
      "ON CONFLICT(ChannelId, AccountId, ConversationId, PrincipalId, "
      "MemoryId) DO UPDATE SET "
      "MemoryKind=excluded.MemoryKind, SubjectId=excluded.SubjectId, "
      "Content=excluded.Content, SourceReference=excluded.SourceReference, "
      "UpdatedAt=excluded.UpdatedAt, ExpiresAt=excluded.ExpiresAt, "
      "Provenance=excluded.Provenance, ConsentId=excluded.ConsentId, "
      "Version=excluded.Version "
      "WHERE excluded.Version > MemoryRecords.Version";
  if (sqlite3_prepare_v2(Database_, Sql.c_str(), -1, &Statement.Handle,
                         nullptr) != SQLITE_OK) {
    return DatabaseFailure("record upsert prepare failed", Database_);
  }

  BindScope(Statement.Handle, 1, Record.Scope);
  BindText(Statement.Handle, 5, Record.MemoryId);
  BindText(Statement.Handle, 6, Record.MemoryKind);
  BindText(Statement.Handle, 7, Record.SubjectId);
  BindText(Statement.Handle, 8, Record.Content);
  BindText(Statement.Handle, 9, Record.SourceReference);
  sqlite3_bind_int64(Statement.Handle, 10, Record.CreatedAtUnixSeconds);
  sqlite3_bind_int64(Statement.Handle, 11, Record.UpdatedAtUnixSeconds);
  if (Record.ExpiresAtUnixSeconds.has_value()) {
    sqlite3_bind_int64(Statement.Handle, 12, *Record.ExpiresAtUnixSeconds);
  } else {
    sqlite3_bind_null(Statement.Handle, 12);
  }
  BindText(Statement.Handle, 13, ProvenanceName(Record.Provenance));
  BindText(Statement.Handle, 14, Record.ConsentId);
  sqlite3_bind_int64(Statement.Handle, 15,
                     static_cast<sqlite3_int64>(Record.Version));

  if (sqlite3_step(Statement.Handle) != SQLITE_DONE) {
    return DatabaseFailure("record upsert failed", Database_);
  }
  if (sqlite3_changes(Database_) == 0) {
    return Failure(SqliteStoreErrorCode::VersionConflict,
                   "record version is not newer than the stored version");
  }
  return std::monostate{};
}

SqliteStoreResult<MemoryRecord> SqliteStore::GetActiveRecord(
    const ConversationScopeKey& Scope, std::string_view MemoryId,
    std::int64_t NowUnixSeconds) const {
  if (const auto ScopeError = ValidateScope(Scope);
      ScopeError.has_value()) {
    return *ScopeError;
  }
  if (IsBlank(MemoryId) || MemoryId.size() > MaximumMemoryIdLength) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "memory id must be non-empty and bounded");
  }

  StatementHandle Statement;
  const std::string Sql =
      "SELECT " + std::string(RecordColumns) +
      " FROM MemoryRecords WHERE ChannelId=? AND AccountId=? AND "
      "ConversationId=? AND PrincipalId=? AND MemoryId=? AND "
      "DeleteState='Active' AND (ExpiresAt IS NULL OR ExpiresAt>?)";
  if (sqlite3_prepare_v2(Database_, Sql.c_str(), -1, &Statement.Handle,
                         nullptr) != SQLITE_OK) {
    return DatabaseFailure("record lookup prepare failed", Database_);
  }
  BindScope(Statement.Handle, 1, Scope);
  BindText(Statement.Handle, 5, MemoryId);
  sqlite3_bind_int64(Statement.Handle, 6, NowUnixSeconds);
  const int Step = sqlite3_step(Statement.Handle);
  if (Step == SQLITE_DONE) {
    return Failure(SqliteStoreErrorCode::RecordNotFound,
                   "record is missing, tombstoned, or expired");
  }
  if (Step != SQLITE_ROW) {
    return DatabaseFailure("record lookup failed", Database_);
  }
  return HydrateMemoryRecord(Statement.Handle);
}

SqliteStoreResult<std::vector<MemoryRecord>>
SqliteStore::ListActiveRecords(const ConversationScopeKey& Scope,
                               std::int64_t NowUnixSeconds) const {
  if (const auto ScopeError = ValidateScope(Scope);
      ScopeError.has_value()) {
    return *ScopeError;
  }
  StatementHandle Statement;
  const std::string Sql =
      "SELECT " + std::string(RecordColumns) +
      " FROM MemoryRecords WHERE ChannelId=? AND AccountId=? AND "
      "ConversationId=? AND PrincipalId=? AND DeleteState='Active' AND "
      "(ExpiresAt IS NULL OR ExpiresAt>?) ORDER BY CreatedAt, MemoryId";
  if (sqlite3_prepare_v2(Database_, Sql.c_str(), -1, &Statement.Handle,
                         nullptr) != SQLITE_OK) {
    return DatabaseFailure("record list prepare failed", Database_);
  }
  BindScope(Statement.Handle, 1, Scope);
  sqlite3_bind_int64(Statement.Handle, 5, NowUnixSeconds);

  std::vector<MemoryRecord> Records;
  while (true) {
    const int Step = sqlite3_step(Statement.Handle);
    if (Step == SQLITE_DONE) {
      break;
    }
    if (Step != SQLITE_ROW) {
      return DatabaseFailure("record list failed", Database_);
    }
    auto Record = HydrateMemoryRecord(Statement.Handle);
    if (std::holds_alternative<SqliteStoreError>(Record)) {
      return std::get<SqliteStoreError>(std::move(Record));
    }
    Records.push_back(std::get<MemoryRecord>(std::move(Record)));
  }
  return Records;
}

SqliteStoreResult<std::monostate> SqliteStore::TombstoneRecord(
    SqliteStoreTransaction& Transaction, const ConversationScopeKey& Scope,
    std::string_view MemoryId, std::int64_t NowUnixSeconds) {
  if (!IsTransactionUsable(Transaction)) {
    return Failure(SqliteStoreErrorCode::TransactionRequired,
                   "an active transaction on this store is required");
  }
  if (const auto ScopeError = ValidateScope(Scope);
      ScopeError.has_value()) {
    return *ScopeError;
  }
  if (IsBlank(MemoryId) || MemoryId.size() > MaximumMemoryIdLength) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "memory id must be non-empty and bounded");
  }
  if (NowUnixSeconds <= 0) {
    return Failure(SqliteStoreErrorCode::InvalidArgument,
                   "tombstone time must be positive");
  }

  StatementHandle Statement;
  if (sqlite3_prepare_v2(Database_,
                         "UPDATE MemoryRecords SET "
                         "DeleteState='Tombstoned', UpdatedAt=?, "
                         "Version=Version+1 "
                         "WHERE ChannelId=? AND AccountId=? AND "
                         "ConversationId=? AND PrincipalId=? AND MemoryId=? "
                         "AND DeleteState='Active'",
                         -1, &Statement.Handle, nullptr) != SQLITE_OK) {
    return DatabaseFailure("tombstone prepare failed", Database_);
  }
  sqlite3_bind_int64(Statement.Handle, 1, NowUnixSeconds);
  BindScope(Statement.Handle, 2, Scope);
  BindText(Statement.Handle, 6, MemoryId);
  if (sqlite3_step(Statement.Handle) != SQLITE_DONE) {
    return DatabaseFailure("tombstone failed", Database_);
  }
  if (sqlite3_changes(Database_) == 0) {
    return Failure(SqliteStoreErrorCode::RecordNotFound,
                   "record is missing or already tombstoned");
  }
  return std::monostate{};
}

SqliteStoreResult<std::vector<BackupRecallRecord>>
SqliteStore::RecallBackupForScope(const ConversationScopeKey& Scope,
                                  std::int64_t NowUnixSeconds) const {
  if (const auto ScopeError = ValidateScope(Scope);
      ScopeError.has_value()) {
    return *ScopeError;
  }
  StatementHandle Statement;
  const std::string Sql =
      "SELECT " + std::string(RecordColumns) +
      " FROM MemoryRecords WHERE ChannelId=? AND AccountId=? AND "
      "ConversationId=? AND PrincipalId=? AND DeleteState='Active' AND "
      "(ExpiresAt IS NULL OR ExpiresAt>?) ORDER BY CreatedAt, MemoryId";
  if (sqlite3_prepare_v2(Database_, Sql.c_str(), -1, &Statement.Handle,
                         nullptr) != SQLITE_OK) {
    return DatabaseFailure("backup recall prepare failed", Database_);
  }
  BindScope(Statement.Handle, 1, Scope);
  sqlite3_bind_int64(Statement.Handle, 5, NowUnixSeconds);

  std::vector<BackupRecallRecord> Records;
  while (true) {
    const int Step = sqlite3_step(Statement.Handle);
    if (Step == SQLITE_DONE) {
      break;
    }
    if (Step != SQLITE_ROW) {
      return DatabaseFailure("backup recall failed", Database_);
    }
    auto Record = HydrateBackupRecord(Statement.Handle);
    if (std::holds_alternative<SqliteStoreError>(Record)) {
      return std::get<SqliteStoreError>(std::move(Record));
    }
    Records.push_back(std::get<BackupRecallRecord>(std::move(Record)));
  }
  return Records;
}

} // namespace EmulativeDialog::Storage
