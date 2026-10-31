#include "Source/Memory/SqliteMemoryStorageAdapter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
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

bool IsValidDurableContent(std::string_view Content) noexcept {
  std::size_t Position = 0;
  while (Position < Content.size()) {
    const auto Lead = static_cast<unsigned char>(Content[Position]);
    if (Lead == 0U) {
      return false;
    }
    if (Lead <= 0x7fU) {
      ++Position;
      continue;
    }

    std::size_t Length = 0;
    if (Lead >= 0xc2U && Lead <= 0xdfU) {
      Length = 2U;
    } else if (Lead >= 0xe0U && Lead <= 0xefU) {
      Length = 3U;
    } else if (Lead >= 0xf0U && Lead <= 0xf4U) {
      Length = 4U;
    } else {
      return false;
    }
    if (Content.size() - Position < Length) {
      return false;
    }

    const auto Second =
        static_cast<unsigned char>(Content[Position + 1U]);
    if ((Lead == 0xe0U && Second < 0xa0U) ||
        (Lead == 0xedU && Second > 0x9fU) ||
        (Lead == 0xf0U && Second < 0x90U) ||
        (Lead == 0xf4U && Second > 0x8fU) ||
        Second < 0x80U || Second > 0xbfU) {
      return false;
    }
    for (std::size_t Offset = 2U; Offset < Length; ++Offset) {
      const auto Continuation =
          static_cast<unsigned char>(Content[Position + Offset]);
      if (Continuation < 0x80U || Continuation > 0xbfU) {
        return false;
      }
    }
    Position += Length;
  }
  return true;
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
    MemoryCategory Category, bool UserConfirmed,
    const std::optional<std::string>& ProposalId) {
  const auto CategoryName = ToStorageCategoryName(Category);
  if (!CategoryName.has_value() ||
      (ProposalId.has_value() && ProposalId->empty())) {
    return std::nullopt;
  }
  constexpr std::size_t MaximumKindLength =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (!ProposalId.has_value()) {
    std::string Kind{"EDMemoryV1/"};
    Kind.append(*CategoryName);
    Kind.push_back('/');
    Kind.push_back(UserConfirmed ? '1' : '0');
    if (Kind.size() > MaximumKindLength) {
      return std::nullopt;
    }
    return Kind;
  }

  constexpr std::string_view Prefix{"EDMemoryV2/"};
  std::array<char, std::numeric_limits<std::size_t>::digits10 + 1U>
      LengthBuffer{};
  const auto [LengthEnd, LengthError] = std::to_chars(
      LengthBuffer.data(), LengthBuffer.data() + LengthBuffer.size(),
      ProposalId->size());
  if (LengthError != std::errc{}) {
    return std::nullopt;
  }
  const std::size_t DecimalLength =
      static_cast<std::size_t>(LengthEnd - LengthBuffer.data());
  constexpr std::size_t FixedSuffixLength = 5U; // "/<bit>/P" plus ':'
  const std::size_t FixedLength =
      Prefix.size() + CategoryName->size() + FixedSuffixLength +
      DecimalLength;
  if (FixedLength > MaximumKindLength ||
      ProposalId->size() > (MaximumKindLength - FixedLength) / 2U) {
    return std::nullopt;
  }
  const std::size_t EncodedLength =
      FixedLength + ProposalId->size() * 2U;

  std::string Kind;
  Kind.reserve(EncodedLength);
  Kind.append(Prefix);
  Kind.append(*CategoryName);
  Kind.push_back('/');
  Kind.push_back(UserConfirmed ? '1' : '0');
  Kind.append("/P");
  Kind.append(LengthBuffer.data(), DecimalLength);
  Kind.push_back(':');
  constexpr char HexDigits[] = "0123456789abcdef";
  for (const unsigned char Byte : *ProposalId) {
    Kind.push_back(HexDigits[Byte >> 4U]);
    Kind.push_back(HexDigits[Byte & 0x0fU]);
  }
  return Kind;
}

struct DecodedMemoryKind final {
  MemoryCategory Category;
  bool UserConfirmed;
  std::optional<std::string> ProposalId;
};

std::optional<DecodedMemoryKind> FromStorageMemoryKind(
    std::string_view Kind) noexcept {
  constexpr std::size_t MaximumKindLength =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (Kind.size() > MaximumKindLength) {
    return std::nullopt;
  }

  constexpr std::string_view V1Prefix{"EDMemoryV1/"};
  constexpr std::string_view V2Prefix{"EDMemoryV2/"};
  const bool IsV1 = Kind.starts_with(V1Prefix);
  const bool IsV2 = Kind.starts_with(V2Prefix);
  if (!IsV1 && !IsV2) {
    return std::nullopt;
  }

  const std::string_view Prefix = IsV1 ? V1Prefix : V2Prefix;
  const std::string_view Encoded = Kind.substr(Prefix.size());
  const std::size_t CategorySeparator = Encoded.find('/');
  if (CategorySeparator == std::string_view::npos ||
      CategorySeparator == 0U || CategorySeparator + 2U > Encoded.size()) {
    return std::nullopt;
  }

  const auto Category =
      FromStorageCategoryName(Encoded.substr(0, CategorySeparator));
  if (!Category.has_value()) {
    return std::nullopt;
  }

  const std::size_t ConfirmationPosition = CategorySeparator + 1U;
  if (ConfirmationPosition >= Encoded.size()) {
    return std::nullopt;
  }
  const char Confirmation = Encoded[ConfirmationPosition];
  if (Confirmation != '0' && Confirmation != '1') {
    return std::nullopt;
  }

  std::optional<std::string> ProposalId;
  if (IsV1) {
    if (ConfirmationPosition + 1U != Encoded.size()) {
      return std::nullopt;
    }
  } else {
    if (ConfirmationPosition + 1U >= Encoded.size() ||
        Encoded[ConfirmationPosition + 1U] != '/') {
      return std::nullopt;
    }
    const std::string_view Payload =
        Encoded.substr(ConfirmationPosition + 2U);
    if (Payload.size() < 3U || Payload[0] != 'P') {
      return std::nullopt;
    }
    const std::size_t Colon = Payload.find(':', 1U);
    if (Colon == std::string_view::npos || Colon == 1U) {
      return std::nullopt;
    }
    const std::string_view DecimalLength = Payload.substr(1U, Colon - 1U);
    if (DecimalLength.size() > 1U && DecimalLength.front() == '0') {
      return std::nullopt;
    }
    std::size_t DecodedLength = 0U;
    constexpr std::size_t MaximumSize =
        std::numeric_limits<std::size_t>::max();
    for (const char DigitCharacter : DecimalLength) {
      if (DigitCharacter < '0' || DigitCharacter > '9') {
        return std::nullopt;
      }
      const std::size_t Digit =
          static_cast<std::size_t>(DigitCharacter - '0');
      if (DecodedLength > (MaximumSize - Digit) / 10U) {
        return std::nullopt;
      }
      DecodedLength = DecodedLength * 10U + Digit;
    }
    if (DecodedLength == 0U) {
      return std::nullopt;
    }

    const std::string_view Hex = Payload.substr(Colon + 1U);
    if ((Hex.size() % 2U) != 0U || DecodedLength != Hex.size() / 2U) {
      return std::nullopt;
    }

    const auto DecodeNibble = [](char Character) noexcept
        -> std::optional<unsigned char> {
      if (Character >= '0' && Character <= '9') {
        return static_cast<unsigned char>(Character - '0');
      }
      if (Character >= 'a' && Character <= 'f') {
        return static_cast<unsigned char>(Character - 'a' + 10);
      }
      return std::nullopt;
    };
    for (const char Character : Hex) {
      if (!DecodeNibble(Character).has_value()) {
        return std::nullopt;
      }
    }
    try {
      std::string Decoded;
      Decoded.reserve(DecodedLength);
      for (std::size_t Index = 0; Index < Hex.size(); Index += 2U) {
        const auto High = DecodeNibble(Hex[Index]);
        const auto Low = DecodeNibble(Hex[Index + 1U]);
        Decoded.push_back(static_cast<char>((*High << 4U) | *Low));
      }
      ProposalId = std::move(Decoded);
    } catch (const std::bad_alloc&) {
      return std::nullopt;
    }
  }

  return DecodedMemoryKind{.Category = *Category,
                           .UserConfirmed = Confirmation == '1',
                           .ProposalId = std::move(ProposalId)};
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
      ToStorageMemoryKind(Record.Category, Record.UserConfirmed,
                          Record.ProposalId);
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
  Result.ProposalId = Kind->ProposalId;
  Result.Version = Record.Version;
  Result.Tombstoned =
      Record.DeleteState == Storage::MemoryDeleteState::Tombstoned;
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
    if (!IsValidDurableContent(Record.Content)) {
      return InvalidArgument();
    }
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

  auto Recalled = Store_.ListActiveRecords(*StorageScope, UnixSeconds(Now));
  if (const auto* Error = std::get_if<Storage::SqliteStoreError>(&Recalled)) {
    return MapStorageError(*Error);
  }

  std::vector<StoredMemory> Result;
  const auto& Records = std::get<std::vector<Storage::MemoryRecord>>(Recalled);
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

} // namespace EmulativeDialog::Memory
