#include "Source/Memory/SqliteMemoryStorageAdapter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace EmulativeDialog::Memory {
namespace {

// SqliteStore schema V1 has no ScopeKind column. Keep Direct and Group in
// distinct primary-key namespaces by encoding the kind into ConversationId.
constexpr std::string_view DirectScopePrefix =
    "EmulativeDialog.ScopeV1.Direct/";
constexpr std::string_view GroupScopePrefix =
    "EmulativeDialog.ScopeV1.Group/";

MemoryStorageError MakeError(MemoryStorageErrorCode Code,
                             std::string Detail) {
  return MemoryStorageError{.Code = Code, .Detail = std::move(Detail)};
}

MemoryStorageError InvalidArgument() {
  return MakeError(MemoryStorageErrorCode::InvalidArgument,
                   "invalid memory storage input");
}

std::optional<Storage::ConversationScopeKey> ToStorageScope(
    const StoredMemory& Scope) {
  std::string_view Prefix;
  switch (Scope.ScopeKind) {
  case ConversationScopeKind::Direct:
    Prefix = DirectScopePrefix;
    break;
  case ConversationScopeKind::Group:
    Prefix = GroupScopePrefix;
    break;
  case ConversationScopeKind::Session:
    return std::nullopt;
  default:
    return std::nullopt;
  }

  Storage::ConversationScopeKey Key{
      .ChannelId = Scope.ChannelId,
      .AccountId = Scope.AccountId,
      .ConversationId = std::string(Prefix) + Scope.ConversationId,
      .PrincipalId = Scope.PrincipalId,
  };
  if (!Key.IsComplete()) {
    return std::nullopt;
  }
  return Key;
}

bool SameScope(const StoredMemory& Left, const StoredMemory& Right) {
  return Left.ChannelId == Right.ChannelId &&
         Left.AccountId == Right.AccountId &&
         Left.ConversationId == Right.ConversationId &&
         Left.ScopeKind == Right.ScopeKind &&
         Left.PrincipalId == Right.PrincipalId;
}

std::optional<std::string_view> ToStorageCategoryName(
    MemoryCategory Category) noexcept {
  switch (Category) {
  case MemoryCategory::IdentityPreference:
    return "IdentityPreference";
  case MemoryCategory::ConversationSummary:
    return "ConversationSummary";
  case MemoryCategory::GroupMemory:
    return "GroupMemory";
  case MemoryCategory::IdentityLinkMemory:
    return "IdentityLinkMemory";
  case MemoryCategory::ActiveConversationContext:
    return "ActiveConversationContext";
  }
  return std::nullopt;
}

std::optional<MemoryCategory> FromStorageCategoryName(
    std::string_view Kind) noexcept {
  if (Kind == "IdentityPreference") {
    return MemoryCategory::IdentityPreference;
  }
  if (Kind == "ConversationSummary") {
    return MemoryCategory::ConversationSummary;
  }
  if (Kind == "GroupMemory") {
    return MemoryCategory::GroupMemory;
  }
  if (Kind == "IdentityLinkMemory") {
    return MemoryCategory::IdentityLinkMemory;
  }
  if (Kind == "ActiveConversationContext") {
    return MemoryCategory::ActiveConversationContext;
  }
  return std::nullopt;
}

std::optional<std::string> ToStorageMemoryKind(
    MemoryCategory Category, bool UserConfirmed) {
  const auto CategoryName = ToStorageCategoryName(Category);
  if (!CategoryName.has_value()) {
    return std::nullopt;
  }
  std::string Kind{"EDMemoryV1/"};
  Kind.append(*CategoryName);
  Kind.push_back('/');
  Kind.push_back(UserConfirmed ? '1' : '0');
  return Kind;
}

struct DecodedMemoryKind final {
  MemoryCategory Category;
  bool UserConfirmed;
};

std::optional<DecodedMemoryKind> FromStorageMemoryKind(
    std::string_view Kind) noexcept {
  constexpr std::string_view Prefix{"EDMemoryV1/"};
  if (!Kind.starts_with(Prefix)) {
    return std::nullopt;
  }

  const std::string_view Encoded = Kind.substr(Prefix.size());
  const std::size_t Separator = Encoded.rfind('/');
  if (Separator == std::string_view::npos ||
      Separator + 2U != Encoded.size()) {
    return std::nullopt;
  }
  const std::string_view Confirmation = Encoded.substr(Separator + 1U);
  if (Confirmation != "0" && Confirmation != "1") {
    return std::nullopt;
  }

  const auto Category = FromStorageCategoryName(Encoded.substr(0, Separator));
  if (!Category.has_value()) {
    return std::nullopt;
  }
  return DecodedMemoryKind{.Category = *Category,
                           .UserConfirmed = Confirmation == "1"};
}

std::optional<Storage::MemoryProvenance> ToStorageProvenance(
    MemoryProvenance Provenance) noexcept {
  switch (Provenance) {
  case MemoryProvenance::UserExplicit:
    return Storage::MemoryProvenance::UserExplicit;
  case MemoryProvenance::UserConfirmed:
    return Storage::MemoryProvenance::UserConfirmed;
  case MemoryProvenance::AuthorizedSummary:
    return Storage::MemoryProvenance::AuthorizedSummary;
  }
  return std::nullopt;
}

std::optional<MemoryProvenance> FromStorageProvenance(
    Storage::MemoryProvenance Provenance) noexcept {
  switch (Provenance) {
  case Storage::MemoryProvenance::UserExplicit:
    return MemoryProvenance::UserExplicit;
  case Storage::MemoryProvenance::UserConfirmed:
    return MemoryProvenance::UserConfirmed;
  case Storage::MemoryProvenance::AuthorizedSummary:
    return MemoryProvenance::AuthorizedSummary;
  }
  return std::nullopt;
}

std::int64_t UnixSeconds(std::chrono::sys_seconds Value) noexcept {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          Value.time_since_epoch())
          .count());
}

std::int64_t CurrentUnixSeconds() noexcept {
  return UnixSeconds(std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now()));
}

MemoryStorageError MapStorageError(const Storage::SqliteStoreError& Error) {
  switch (Error.Code) {
  case Storage::SqliteStoreErrorCode::RecordNotFound:
    return MakeError(MemoryStorageErrorCode::NotFound,
                     "memory record was not found");
  case Storage::SqliteStoreErrorCode::VersionConflict:
    return MakeError(MemoryStorageErrorCode::VersionConflict,
                     "memory record version conflict");
  case Storage::SqliteStoreErrorCode::InvalidArgument:
    return InvalidArgument();
  case Storage::SqliteStoreErrorCode::SchemaVersionMismatch:
  case Storage::SqliteStoreErrorCode::TransactionRequired:
  case Storage::SqliteStoreErrorCode::NestedTransactionRejected:
  case Storage::SqliteStoreErrorCode::DatabaseFailure:
    return MakeError(MemoryStorageErrorCode::Failure,
                     "SQLite memory storage operation failed");
  }
  return MakeError(MemoryStorageErrorCode::Failure,
                   "SQLite memory storage operation failed");
}

MemoryStorageResult<Storage::MemoryRecord> ToStorageRecord(
    const StoredMemory& Record, const StoredMemory& ScopeTemplate,
    const Storage::ConversationScopeKey& StorageScope) {
  if (!SameScope(Record, ScopeTemplate) || Record.Tombstoned ||
      Record.MemoryId.empty() ||
      Record.MemoryId.size() > Storage::MaximumMemoryIdLength ||
      Record.SubjectId.empty() || Record.Content.empty() ||
      Record.Content.size() > Storage::MaximumMemoryContentLength ||
      Record.SourceReference.empty() || Record.ConsentId.empty() ||
      Record.Version == 0 ||
      Record.Version > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
    return InvalidArgument();
  }

  const auto Kind =
      ToStorageMemoryKind(Record.Category, Record.UserConfirmed);
  const auto Provenance = ToStorageProvenance(Record.Provenance);
  if (!Kind.has_value() || !Provenance.has_value()) {
    return InvalidArgument();
  }

  const std::int64_t CreatedAt = UnixSeconds(Record.CreatedAt);
  const std::int64_t CurrentTime = CurrentUnixSeconds();
  const std::int64_t UpdatedAt = std::max(CreatedAt, CurrentTime);
  if (CreatedAt <= 0 ||
      (Record.ExpiresAt.has_value() &&
       UnixSeconds(*Record.ExpiresAt) <= CreatedAt)) {
    return InvalidArgument();
  }

  Storage::MemoryRecord Result;
  Result.MemoryId = Record.MemoryId;
  Result.MemoryKind = *Kind;
  Result.Scope = StorageScope;
  Result.SubjectId = Record.SubjectId;
  Result.Content = Record.Content;
  Result.SourceReference = Record.SourceReference;
  Result.CreatedAtUnixSeconds = CreatedAt;
  Result.UpdatedAtUnixSeconds = UpdatedAt;
  if (Record.ExpiresAt.has_value()) {
    Result.ExpiresAtUnixSeconds = UnixSeconds(*Record.ExpiresAt);
  }
  Result.Provenance = *Provenance;
  Result.ConsentId = Record.ConsentId;
  Result.DeleteState = Storage::MemoryDeleteState::Active;
  Result.Version = Record.Version;
  return Result;
}

MemoryStorageResult<StoredMemory> FromStorageRecord(
    const Storage::MemoryRecord& Record, const StoredMemory& ScopeTemplate,
    const Storage::ConversationScopeKey& ExpectedStorageScope) {
  if (!(Record.Scope == ExpectedStorageScope) || Record.Version == 0 ||
      Record.Version > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max()) ||
      Record.CreatedAtUnixSeconds <= 0 ||
      Record.UpdatedAtUnixSeconds < Record.CreatedAtUnixSeconds) {
    return MakeError(MemoryStorageErrorCode::Failure,
                     "SQLite memory record is invalid");
  }

  const auto Kind = FromStorageMemoryKind(Record.MemoryKind);
  const auto Provenance = FromStorageProvenance(Record.Provenance);
  if (!Kind.has_value() || !Provenance.has_value()) {
    return MakeError(MemoryStorageErrorCode::Failure,
                     "SQLite memory record contains an unknown kind");
  }

  StoredMemory Result = ScopeTemplate;
  Result.MemoryId = Record.MemoryId;
  Result.Category = Kind->Category;
  Result.SubjectId = Record.SubjectId;
  Result.Content = Record.Content;
  Result.SourceReference = Record.SourceReference;
  Result.ConsentId = Record.ConsentId;
  Result.CreatedAt = std::chrono::sys_seconds{
      std::chrono::seconds{Record.CreatedAtUnixSeconds}};
  if (Record.ExpiresAtUnixSeconds.has_value()) {
    Result.ExpiresAt = std::chrono::sys_seconds{
        std::chrono::seconds{*Record.ExpiresAtUnixSeconds}};
  }
  Result.Provenance = *Provenance;
  Result.UserConfirmed = Kind->UserConfirmed;
  Result.Version = Record.Version;
  Result.Tombstoned =
      Record.DeleteState == Storage::MemoryDeleteState::Tombstoned;
  return Result;
}

Storage::MemoryRecord ToStorageRecord(
    const Storage::BackupRecallRecord& Record) {
  Storage::MemoryRecord Result;
  Result.MemoryId = Record.MemoryId;
  Result.MemoryKind = Record.MemoryKind;
  Result.Scope = Record.Scope;
  Result.SubjectId = Record.SubjectId;
  Result.Content = Record.Content;
  Result.SourceReference = Record.SourceReference;
  Result.CreatedAtUnixSeconds = Record.CreatedAtUnixSeconds;
  Result.UpdatedAtUnixSeconds = Record.CreatedAtUnixSeconds;
  Result.Provenance = Record.Provenance;
  Result.ConsentId = Record.ConsentId;
  Result.DeleteState = Storage::MemoryDeleteState::Active;
  Result.Version = Record.Version;
  return Result;
}

void Rollback(Storage::SqliteStoreTransaction& Transaction) {
  const auto Result = Transaction.Rollback();
  static_cast<void>(Result);
}

} // namespace

SqliteMemoryStorageAdapter::SqliteMemoryStorageAdapter(
    Storage::SqliteStore& Store) noexcept
    : Store_(Store) {}

MemoryStorageResult<std::monostate>
SqliteMemoryStorageAdapter::ApplyAtomically(
    const MemoryMutationBatch& Batch) {
  const auto StorageScope = ToStorageScope(Batch.Scope);
  if (!StorageScope.has_value()) {
    return InvalidArgument();
  }

  std::unordered_set<std::string> UpsertIds;
  std::vector<Storage::MemoryRecord> Upserts;
  Upserts.reserve(Batch.Upserts.size());
  for (const StoredMemory& Record : Batch.Upserts) {
    if (!UpsertIds.insert(Record.MemoryId).second) {
      return InvalidArgument();
    }
    auto Converted = ToStorageRecord(Record, Batch.Scope, *StorageScope);
    if (const auto* Error = std::get_if<MemoryStorageError>(&Converted)) {
      return *Error;
    }
    Upserts.push_back(
        std::get<Storage::MemoryRecord>(std::move(Converted)));
  }

  std::unordered_set<std::string> TombstoneIds;
  for (const std::string& MemoryId : Batch.Tombstones) {
    if (MemoryId.empty() ||
        MemoryId.size() > Storage::MaximumMemoryIdLength ||
        UpsertIds.contains(MemoryId) ||
        !TombstoneIds.insert(MemoryId).second) {
      return InvalidArgument();
    }
  }

  if (Upserts.empty() && TombstoneIds.empty()) {
    return std::monostate{};
  }

  auto BeginResult = Store_.BeginTransaction();
  if (const auto* Error = std::get_if<Storage::SqliteStoreError>(&BeginResult)) {
    return MapStorageError(*Error);
  }
  auto Transaction =
      std::move(std::get<std::unique_ptr<Storage::SqliteStoreTransaction>>(
          BeginResult));

  for (const Storage::MemoryRecord& Record : Upserts) {
    auto Result = Store_.PutRecord(*Transaction, Record);
    if (const auto* Error = std::get_if<Storage::SqliteStoreError>(&Result)) {
      Rollback(*Transaction);
      return MapStorageError(*Error);
    }
  }

  const std::int64_t TombstoneTime = CurrentUnixSeconds();
  for (const std::string& MemoryId : TombstoneIds) {
    auto Result = Store_.TombstoneRecord(
        *Transaction, *StorageScope, MemoryId, TombstoneTime);
    if (const auto* Error = std::get_if<Storage::SqliteStoreError>(&Result)) {
      Rollback(*Transaction);
      return MapStorageError(*Error);
    }
  }

  auto CommitResult = Transaction->Commit();
  if (const auto* Error =
          std::get_if<Storage::SqliteStoreError>(&CommitResult)) {
    Rollback(*Transaction);
    return MapStorageError(*Error);
  }
  return std::monostate{};
}

MemoryStorageResult<std::vector<StoredMemory>>
SqliteMemoryStorageAdapter::ListActive(
    const StoredMemory& ScopeTemplate,
    std::chrono::sys_seconds Now) const {
  const auto StorageScope = ToStorageScope(ScopeTemplate);
  if (!StorageScope.has_value()) {
    return InvalidArgument();
  }

  auto Listed = Store_.ListActiveRecords(*StorageScope, UnixSeconds(Now));
  if (const auto* Error = std::get_if<Storage::SqliteStoreError>(&Listed)) {
    return MapStorageError(*Error);
  }

  std::vector<StoredMemory> Result;
  const auto& Records =
      std::get<std::vector<Storage::MemoryRecord>>(Listed);
  Result.reserve(Records.size());
  for (const Storage::MemoryRecord& Record : Records) {
    auto Converted =
        FromStorageRecord(Record, ScopeTemplate, *StorageScope);
    if (const auto* Error = std::get_if<MemoryStorageError>(&Converted)) {
      return *Error;
    }
    Result.push_back(
        std::get<StoredMemory>(std::move(Converted)));
  }
  return Result;
}

MemoryStorageResult<std::vector<StoredMemory>>
SqliteMemoryStorageAdapter::RecallBackup(
    const StoredMemory& ScopeTemplate,
    std::chrono::sys_seconds Now) const {
  const auto StorageScope = ToStorageScope(ScopeTemplate);
  if (!StorageScope.has_value()) {
    return InvalidArgument();
  }

  auto Recalled = Store_.RecallBackupForScope(
      *StorageScope, UnixSeconds(Now));
  if (const auto* Error = std::get_if<Storage::SqliteStoreError>(&Recalled)) {
    return MapStorageError(*Error);
  }

  std::vector<StoredMemory> Result;
  const auto& Records =
      std::get<std::vector<Storage::BackupRecallRecord>>(Recalled);
  Result.reserve(Records.size());
  for (const Storage::BackupRecallRecord& Backup : Records) {
    auto Converted = FromStorageRecord(ToStorageRecord(Backup),
                                       ScopeTemplate, *StorageScope);
    if (const auto* Error = std::get_if<MemoryStorageError>(&Converted)) {
      return *Error;
    }
    Result.push_back(
        std::get<StoredMemory>(std::move(Converted)));
  }
  return Result;
}

} // namespace EmulativeDialog::Memory
