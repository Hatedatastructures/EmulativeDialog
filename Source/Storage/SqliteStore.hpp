#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct sqlite3;

namespace EmulativeDialog::Storage {

// Bump only with a reviewed migration; SqliteStore fails closed on any other
// version instead of recreating or clearing an existing database.
inline constexpr int SqliteMemorySchemaVersion = 1;
inline constexpr std::size_t MaximumMemoryIdLength = 128;
inline constexpr std::size_t MaximumMemoryContentLength = 8192;

struct ConversationScopeKey final {
  std::string ChannelId;
  std::string AccountId;
  std::string ConversationId;
  std::string PrincipalId;

  [[nodiscard]] bool IsComplete() const noexcept;
  [[nodiscard]] bool operator==(const ConversationScopeKey&) const = default;
};

enum class MemoryDeleteState {
  Active,
  Tombstoned,
};

enum class MemoryProvenance {
  UserExplicit,
  UserConfirmed,
  AuthorizedSummary,
};

struct MemoryRecord final {
  std::string MemoryId;
  std::string MemoryKind;
  ConversationScopeKey Scope;
  std::string SubjectId;
  std::string Content;
  std::string SourceReference;
  std::int64_t CreatedAtUnixSeconds{0};
  std::int64_t UpdatedAtUnixSeconds{0};
  std::optional<std::int64_t> ExpiresAtUnixSeconds;
  MemoryProvenance Provenance{MemoryProvenance::UserExplicit};
  std::string ConsentId;
  MemoryDeleteState DeleteState{MemoryDeleteState::Active};
  std::uint64_t Version{1};
};

// Limited projection for backup recall. Internal storage details stay private
// and tombstoned or expired rows are never projected.
struct BackupRecallRecord final {
  std::string MemoryId;
  std::string MemoryKind;
  ConversationScopeKey Scope;
  std::string SubjectId;
  std::string Content;
  std::string SourceReference;
  std::int64_t CreatedAtUnixSeconds{0};
  MemoryProvenance Provenance{MemoryProvenance::UserExplicit};
  std::string ConsentId;
  std::uint64_t Version{1};
};

enum class SqliteStoreErrorCode {
  InvalidArgument,
  RecordNotFound,
  VersionConflict,
  SchemaVersionMismatch,
  TransactionRequired,
  NestedTransactionRejected,
  DatabaseFailure,
};

struct SqliteStoreError final {
  SqliteStoreErrorCode Code{SqliteStoreErrorCode::DatabaseFailure};
  std::string Detail;
};

template <typename Value>
using SqliteStoreResult = std::variant<Value, SqliteStoreError>;

class SqliteStore;

class SqliteStoreTransaction final {
public:
  SqliteStoreTransaction(const SqliteStoreTransaction&) = delete;
  SqliteStoreTransaction& operator=(const SqliteStoreTransaction&) = delete;
  SqliteStoreTransaction(SqliteStoreTransaction&&) = delete;
  SqliteStoreTransaction& operator=(SqliteStoreTransaction&&) = delete;
  ~SqliteStoreTransaction();

  [[nodiscard]] SqliteStoreResult<std::monostate> Commit();
  [[nodiscard]] SqliteStoreResult<std::monostate> Rollback();
  [[nodiscard]] bool IsActive() const noexcept;

private:
  friend class SqliteStore;
  explicit SqliteStoreTransaction(SqliteStore& Owner) noexcept;

  SqliteStore* Owner_{nullptr};
  bool Active_{true};
};

class SqliteStore final {
public:
  static SqliteStoreResult<std::unique_ptr<SqliteStore>> Open(
      const std::filesystem::path& DatabasePath);

  SqliteStore(const SqliteStore&) = delete;
  SqliteStore& operator=(const SqliteStore&) = delete;
  SqliteStore(SqliteStore&&) = delete;
  SqliteStore& operator=(SqliteStore&&) = delete;
  ~SqliteStore();

  [[nodiscard]] SqliteStoreResult<std::unique_ptr<SqliteStoreTransaction>>
  BeginTransaction();

  [[nodiscard]] SqliteStoreResult<std::monostate> PutRecord(
      SqliteStoreTransaction& Transaction, const MemoryRecord& Record);

  [[nodiscard]] SqliteStoreResult<MemoryRecord> GetActiveRecord(
      const ConversationScopeKey& Scope, std::string_view MemoryId,
      std::int64_t NowUnixSeconds) const;

  [[nodiscard]] SqliteStoreResult<std::vector<MemoryRecord>>
  ListActiveRecords(const ConversationScopeKey& Scope,
                    std::int64_t NowUnixSeconds) const;

  [[nodiscard]] SqliteStoreResult<std::monostate> TombstoneRecord(
      SqliteStoreTransaction& Transaction, const ConversationScopeKey& Scope,
      std::string_view MemoryId, std::int64_t NowUnixSeconds);

  [[nodiscard]] SqliteStoreResult<std::vector<BackupRecallRecord>>
  RecallBackupForScope(const ConversationScopeKey& Scope,
                       std::int64_t NowUnixSeconds) const;

private:
  friend class SqliteStoreTransaction;
  SqliteStore() = default;

  [[nodiscard]] bool IsTransactionUsable(
      const SqliteStoreTransaction& Transaction) const noexcept;
  [[nodiscard]] SqliteStoreResult<std::monostate> ExecuteStatement(
      const char* Statement);
  void FinishTransaction(bool Committed) noexcept;

  sqlite3* Database_{nullptr};
  bool TransactionActive_{false};
};

} // namespace EmulativeDialog::Storage
