#include "Source/Storage/TurnCheckpointStore.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace EmulativeDialog::Storage {
namespace {

constexpr std::string_view RecordPrefix = "EDCP";
constexpr std::string_view RecordVersion = "1";
constexpr std::string_view TurnRecordVersion = "2";
constexpr std::size_t MaximumIdentifierBytes = 128;
constexpr std::size_t MaximumRecordBytes = 2048;

CheckpointStoreError Error(CheckpointStoreErrorCode Code,
                           std::string Description) {
  return CheckpointStoreError{.Code = Code,
                              .SanitizedDescription = std::move(Description)};
}

bool IsContinueCharacter(unsigned char Character) noexcept {
  return (Character >= static_cast<unsigned char>('a') &&
          Character <= static_cast<unsigned char>('z')) ||
         (Character >= static_cast<unsigned char>('A') &&
          Character <= static_cast<unsigned char>('Z')) ||
         (Character >= static_cast<unsigned char>('0') &&
          Character <= static_cast<unsigned char>('9')) ||
         Character == static_cast<unsigned char>('-') ||
         Character == static_cast<unsigned char>('_') ||
         Character == static_cast<unsigned char>('.') ||
         Character == static_cast<unsigned char>(':') ||
         Character == static_cast<unsigned char>('@');
}

bool IsOptionalIdentifier(std::string_view Identifier) noexcept {
  return Identifier.empty() ||
         TurnCheckpointStore::IsValidIdentifier(Identifier);
}

std::optional<TurnPhase> ParseTurnPhase(std::string_view Value) noexcept {
  if (Value == "Created") {
    return TurnPhase::Created;
  }
  if (Value == "ReadyToDispatch") {
    return TurnPhase::ReadyToDispatch;
  }
  if (Value == "ProviderDispatched") {
    return TurnPhase::ProviderDispatched;
  }
  if (Value == "ProviderStreaming") {
    return TurnPhase::ProviderStreaming;
  }
  if (Value == "ProviderCompleted") {
    return TurnPhase::ProviderCompleted;
  }
  if (Value == "ToolDispatched") {
    return TurnPhase::ToolDispatched;
  }
  if (Value == "Sending") {
    return TurnPhase::Sending;
  }
  if (Value == "Completed") {
    return TurnPhase::Completed;
  }
  if (Value == "Cancelled") {
    return TurnPhase::Cancelled;
  }
  if (Value == "Failed") {
    return TurnPhase::Failed;
  }
  if (Value == "Unsupported") {
    return TurnPhase::Unsupported;
  }
  if (Value == "OutcomeUnknown") {
    return TurnPhase::OutcomeUnknown;
  }
  return std::nullopt;
}

std::string_view TurnPhaseName(TurnPhase Phase) noexcept {
  switch (Phase) {
  case TurnPhase::Created:
    return "Created";
  case TurnPhase::ReadyToDispatch:
    return "ReadyToDispatch";
  case TurnPhase::ProviderDispatched:
    return "ProviderDispatched";
  case TurnPhase::ProviderStreaming:
    return "ProviderStreaming";
  case TurnPhase::ProviderCompleted:
    return "ProviderCompleted";
  case TurnPhase::ToolDispatched:
    return "ToolDispatched";
  case TurnPhase::Sending:
    return "Sending";
  case TurnPhase::Completed:
    return "Completed";
  case TurnPhase::Cancelled:
    return "Cancelled";
  case TurnPhase::Failed:
    return "Failed";
  case TurnPhase::Unsupported:
    return "Unsupported";
  case TurnPhase::OutcomeUnknown:
    return "OutcomeUnknown";
  }
  return "OutcomeUnknown";
}

std::optional<ToolExecutionState> ParseToolState(
    std::string_view Value) noexcept {
  if (Value == "NotRequested") {
    return ToolExecutionState::NotRequested;
  }
  if (Value == "Authorized") {
    return ToolExecutionState::Authorized;
  }
  if (Value == "Dispatched") {
    return ToolExecutionState::Dispatched;
  }
  if (Value == "Completed") {
    return ToolExecutionState::Completed;
  }
  if (Value == "Failed") {
    return ToolExecutionState::Failed;
  }
  if (Value == "UnknownOutcome") {
    return ToolExecutionState::UnknownOutcome;
  }
  return std::nullopt;
}

std::string_view ToolStateName(ToolExecutionState State) noexcept {
  switch (State) {
  case ToolExecutionState::NotRequested:
    return "NotRequested";
  case ToolExecutionState::Authorized:
    return "Authorized";
  case ToolExecutionState::Dispatched:
    return "Dispatched";
  case ToolExecutionState::Completed:
    return "Completed";
  case ToolExecutionState::Failed:
    return "Failed";
  case ToolExecutionState::UnknownOutcome:
    return "UnknownOutcome";
  }
  return "UnknownOutcome";
}

std::optional<OperationState> ParseOperationState(
    std::string_view Value) noexcept {
  if (Value == "None") {
    return OperationState::None;
  }
  if (Value == "Prepared") {
    return OperationState::Prepared;
  }
  if (Value == "Authorized") {
    return OperationState::Authorized;
  }
  if (Value == "Dispatched") {
    return OperationState::Dispatched;
  }
  if (Value == "Succeeded") {
    return OperationState::Succeeded;
  }
  if (Value == "Rejected") {
    return OperationState::Rejected;
  }
  if (Value == "Unsupported") {
    return OperationState::Unsupported;
  }
  if (Value == "Failed") {
    return OperationState::Failed;
  }
  if (Value == "UnknownOutcome") {
    return OperationState::UnknownOutcome;
  }
  return std::nullopt;
}

std::string_view OperationStateName(OperationState State) noexcept {
  switch (State) {
  case OperationState::None:
    return "None";
  case OperationState::Prepared:
    return "Prepared";
  case OperationState::Authorized:
    return "Authorized";
  case OperationState::Dispatched:
    return "Dispatched";
  case OperationState::Succeeded:
    return "Succeeded";
  case OperationState::Rejected:
    return "Rejected";
  case OperationState::Unsupported:
    return "Unsupported";
  case OperationState::Failed:
    return "Failed";
  case OperationState::UnknownOutcome:
    return "UnknownOutcome";
  }
  return "UnknownOutcome";
}

std::optional<ScopeState> ParseScopeState(
    std::string_view Value) noexcept {
  if (Value == "Active") {
    return ScopeState::Active;
  }
  if (Value == "Revoked") {
    return ScopeState::Revoked;
  }
  if (Value == "Deleted") {
    return ScopeState::Deleted;
  }
  if (Value == "Tombstoned") {
    return ScopeState::Tombstoned;
  }
  return std::nullopt;
}

std::string_view ScopeStateName(ScopeState State) noexcept {
  switch (State) {
  case ScopeState::Active:
    return "Active";
  case ScopeState::Revoked:
    return "Revoked";
  case ScopeState::Deleted:
    return "Deleted";
  case ScopeState::Tombstoned:
    return "Tombstoned";
  }
  return "Tombstoned";
}

bool IsToolDispatched(ToolExecutionState State) noexcept {
  return State == ToolExecutionState::Dispatched ||
         State == ToolExecutionState::Completed ||
         State == ToolExecutionState::Failed ||
         State == ToolExecutionState::UnknownOutcome;
}

bool RequiresRequestId(TurnPhase Phase) noexcept {
  return Phase == TurnPhase::ProviderDispatched ||
         Phase == TurnPhase::ProviderStreaming ||
         Phase == TurnPhase::ProviderCompleted ||
         Phase == TurnPhase::ToolDispatched ||
         Phase == TurnPhase::Sending ||
         Phase == TurnPhase::Completed;
}

bool CanAdvanceTurn(TurnPhase Previous, TurnPhase Requested) noexcept {
  if (TurnCheckpointStore::IsTerminal(Previous)) {
    return false;
  }
  if (Previous == Requested) {
    return true;
  }
  if (Requested == TurnPhase::Failed ||
      Requested == TurnPhase::Unsupported ||
      Requested == TurnPhase::OutcomeUnknown) {
    return true;
  }
  switch (Previous) {
  case TurnPhase::Created:
    return Requested == TurnPhase::ReadyToDispatch ||
           Requested == TurnPhase::Cancelled;
  case TurnPhase::ReadyToDispatch:
    return Requested == TurnPhase::ProviderDispatched ||
           Requested == TurnPhase::Cancelled;
  case TurnPhase::ProviderDispatched:
    return Requested == TurnPhase::ProviderStreaming ||
           Requested == TurnPhase::ProviderCompleted ||
           Requested == TurnPhase::Cancelled;
  case TurnPhase::ProviderStreaming:
    return Requested == TurnPhase::ProviderCompleted ||
           Requested == TurnPhase::Cancelled;
  case TurnPhase::ProviderCompleted:
    return Requested == TurnPhase::ToolDispatched ||
           Requested == TurnPhase::Sending ||
           Requested == TurnPhase::Completed ||
           Requested == TurnPhase::Cancelled;
  case TurnPhase::ToolDispatched:
    return Requested == TurnPhase::ProviderDispatched ||
           Requested == TurnPhase::ProviderStreaming;
  case TurnPhase::Sending:
    return Requested == TurnPhase::Completed ||
           Requested == TurnPhase::Cancelled;
  case TurnPhase::Completed:
  case TurnPhase::Cancelled:
  case TurnPhase::Failed:
  case TurnPhase::Unsupported:
  case TurnPhase::OutcomeUnknown:
    return false;
  }
  return false;
}

bool CanAdvanceOperation(OperationState Previous,
                         OperationState Requested) noexcept {
  if (Previous == Requested) {
    return true;
  }
  switch (Previous) {
  case OperationState::None:
    return Requested == OperationState::Prepared;
  case OperationState::Prepared:
    return Requested == OperationState::Authorized ||
           Requested == OperationState::Rejected ||
           Requested == OperationState::Unsupported ||
           Requested == OperationState::Failed;
  case OperationState::Authorized:
    return Requested == OperationState::Dispatched ||
           Requested == OperationState::Rejected ||
           Requested == OperationState::Unsupported ||
           Requested == OperationState::Failed;
  case OperationState::Dispatched:
    return Requested == OperationState::Succeeded ||
           Requested == OperationState::Rejected ||
           Requested == OperationState::Unsupported ||
           Requested == OperationState::Failed ||
           Requested == OperationState::UnknownOutcome;
  case OperationState::Succeeded:
  case OperationState::Rejected:
  case OperationState::Unsupported:
  case OperationState::Failed:
  case OperationState::UnknownOutcome:
    return false;
  }
  return false;
}

std::int64_t ToMilliseconds(
    std::chrono::system_clock::time_point Time) noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             Time.time_since_epoch())
      .count();
}

std::optional<std::chrono::system_clock::time_point> FromMilliseconds(
    std::string_view Text) noexcept {
  std::int64_t Milliseconds = 0;
  const auto Parsed = std::from_chars(
      Text.data(), Text.data() + Text.size(), Milliseconds, 10);
  if (Parsed.ec != std::errc{} ||
      Parsed.ptr != Text.data() + Text.size()) {
    return std::nullopt;
  }
  return std::chrono::system_clock::time_point{
      std::chrono::milliseconds{Milliseconds}};
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view Text) noexcept {
  std::uint64_t Value = 0;
  const auto Parsed =
      std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  if (Parsed.ec != std::errc{} ||
      Parsed.ptr != Text.data() + Text.size()) {
    return std::nullopt;
  }
  return Value;
}

std::uint64_t Checksum(std::string_view Payload) noexcept {
  constexpr std::uint64_t Offset = 14695981039346656037ULL;
  constexpr std::uint64_t Prime = 1099511628211ULL;
  std::uint64_t Value = Offset;
  for (const char Character : Payload) {
    Value ^= static_cast<unsigned char>(Character);
    Value *= Prime;
  }
  return Value;
}

std::string ChecksumText(std::uint64_t Value) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Text(16, '0');
  for (std::size_t Index = 0; Index < Text.size(); ++Index) {
    const std::size_t Shift = (Text.size() - 1U - Index) * 4U;
    Text[Index] = Digits[(Value >> Shift) & 0x0FU];
  }
  return Text;
}

std::vector<std::string_view> SplitFields(std::string_view Line) {
  std::vector<std::string_view> Fields;
  std::size_t Start = 0;
  while (true) {
    const std::size_t Delimiter = Line.find('|', Start);
    if (Delimiter == std::string_view::npos) {
      Fields.push_back(Line.substr(Start));
      return Fields;
    }
    Fields.push_back(Line.substr(Start, Delimiter - Start));
    Start = Delimiter + 1U;
  }
}

CheckpointResult<std::monostate> ValidateTurn(
    const TurnCheckpoint& Checkpoint) {
  if (Checkpoint.Version != 1) {
    return Error(CheckpointStoreErrorCode::UnsupportedVersion,
                 "Checkpoint version is not supported.");
  }
  if (!TurnCheckpointStore::IsValidIdentifier(Checkpoint.TurnId) ||
      !TurnCheckpointStore::IsValidIdentifier(Checkpoint.ScopeRef) ||
      !IsOptionalIdentifier(Checkpoint.RequestId) ||
      !IsOptionalIdentifier(Checkpoint.ContextReference) ||
      !IsOptionalIdentifier(Checkpoint.OperationId) ||
      !IsOptionalIdentifier(Checkpoint.IdempotencyKey) ||
      !IsOptionalIdentifier(Checkpoint.ActionId)) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Checkpoint identifiers are invalid.");
  }
  for (const std::string* Value :
       {&Checkpoint.TurnId, &Checkpoint.RequestId, &Checkpoint.ScopeRef,
        &Checkpoint.ContextReference, &Checkpoint.OperationId,
        &Checkpoint.IdempotencyKey, &Checkpoint.ActionId}) {
    if (TurnCheckpointStore::ContainsForbiddenContent(*Value)) {
      return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                   "Checkpoint contains forbidden content.");
    }
  }
  if (Checkpoint.Revision == 0) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Checkpoint revision is invalid.");
  }
  if (static_cast<unsigned>(Checkpoint.Phase) >
          static_cast<unsigned>(TurnPhase::OutcomeUnknown) ||
      static_cast<unsigned>(Checkpoint.ToolState) >
          static_cast<unsigned>(ToolExecutionState::UnknownOutcome) ||
      static_cast<unsigned>(Checkpoint.Operation) >
          static_cast<unsigned>(OperationState::UnknownOutcome)) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Checkpoint state is invalid.");
  }
  if (Checkpoint.Operation != OperationState::None &&
      (Checkpoint.OperationId.empty() ||
       Checkpoint.IdempotencyKey.empty() ||
       Checkpoint.ActionId.empty())) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Operations require stable reconciliation identifiers.");
  }
  if (RequiresRequestId(Checkpoint.Phase) &&
      Checkpoint.RequestId.empty()) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Dispatched checkpoints require a request identifier.");
  }
  if (Checkpoint.Phase >= TurnPhase::ProviderDispatched &&
      Checkpoint.ContextReference.empty()) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Dispatched checkpoints require a recovery context.");
  }
  if (IsToolDispatched(Checkpoint.ToolState) &&
      Checkpoint.Phase < TurnPhase::ProviderCompleted) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Tool state is inconsistent with the turn phase.");
  }
  if (Checkpoint.Phase == TurnPhase::ToolDispatched &&
      !IsToolDispatched(Checkpoint.ToolState)) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Tool dispatch phase requires a dispatched tool state.");
  }
  if (Checkpoint.ToolState == ToolExecutionState::UnknownOutcome &&
      !Checkpoint.UnknownSideEffect) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Unknown tool outcomes must be isolated.");
  }
  if (Checkpoint.Operation == OperationState::UnknownOutcome &&
      !Checkpoint.UnknownSideEffect) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Unknown operation outcomes must be isolated.");
  }
  if (Checkpoint.Phase == TurnPhase::OutcomeUnknown &&
      (!Checkpoint.UnknownSideEffect ||
       Checkpoint.ContextReference.empty())) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Unknown outcomes must preserve safe recovery context.");
  }
  if (Checkpoint.UnknownSideEffect &&
      (Checkpoint.Phase == TurnPhase::Completed ||
       Checkpoint.Phase == TurnPhase::Cancelled ||
       Checkpoint.Phase == TurnPhase::Failed ||
       Checkpoint.Phase == TurnPhase::Unsupported)) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Isolated side effects require an unknown terminal state.");
  }
  return std::monostate{};
}

CheckpointResult<std::monostate> ValidateScope(
    const ScopeCheckpoint& Checkpoint) {
  if (Checkpoint.Version != 1) {
    return Error(CheckpointStoreErrorCode::UnsupportedVersion,
                 "Scope checkpoint version is not supported.");
  }
  if (!TurnCheckpointStore::IsValidIdentifier(Checkpoint.ScopeRef) ||
      Checkpoint.Revision == 0 ||
      static_cast<unsigned>(Checkpoint.State) >
          static_cast<unsigned>(ScopeState::Tombstoned)) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Scope checkpoint is invalid.");
  }
  if (TurnCheckpointStore::ContainsForbiddenContent(Checkpoint.ScopeRef)) {
    return Error(CheckpointStoreErrorCode::InvalidCheckpoint,
                 "Scope checkpoint contains forbidden content.");
  }
  return std::monostate{};
}

bool SameTurnState(const TurnCheckpoint& Left,
                   const TurnCheckpoint& Right) noexcept {
  return Left.TurnId == Right.TurnId &&
         Left.RequestId == Right.RequestId &&
         Left.ScopeRef == Right.ScopeRef &&
         Left.ContextReference == Right.ContextReference &&
         Left.Phase == Right.Phase && Left.ToolState == Right.ToolState &&
         Left.Operation == Right.Operation &&
         Left.OperationId == Right.OperationId &&
         Left.IdempotencyKey == Right.IdempotencyKey &&
         Left.ActionId == Right.ActionId &&
         Left.UnknownSideEffect == Right.UnknownSideEffect &&
         Left.Revision == Right.Revision && Left.Version == Right.Version;
}

bool CanAdvanceScope(ScopeState Previous, ScopeState Requested) noexcept {
  if (Requested == Previous) {
    return true;
  }
  if (Requested == ScopeState::Tombstoned) {
    return Previous == ScopeState::Active ||
           Previous == ScopeState::Revoked ||
           Previous == ScopeState::Deleted;
  }
  if (Requested == ScopeState::Deleted) {
    return Previous == ScopeState::Active ||
           Previous == ScopeState::Revoked;
  }
  if (Requested == ScopeState::Revoked) {
    return Previous == ScopeState::Active;
  }
  return false;
}

std::string EncodeTurn(const TurnCheckpoint& Checkpoint) {
  std::string Payload;
  Payload.reserve(384);
  Payload.append(RecordPrefix)
      .append("|")
      .append(TurnRecordVersion)
      .append("|T|")
      .append(Checkpoint.TurnId)
      .append("|")
      .append(Checkpoint.RequestId)
      .append("|")
      .append(Checkpoint.ScopeRef)
      .append("|")
      .append(Checkpoint.ContextReference)
      .append("|")
      .append(TurnPhaseName(Checkpoint.Phase))
      .append("|")
      .append(ToolStateName(Checkpoint.ToolState))
      .append("|")
      .append(Checkpoint.UnknownSideEffect ? "1" : "0")
      .append("|")
      .append(OperationStateName(Checkpoint.Operation))
      .append("|")
      .append(Checkpoint.OperationId)
      .append("|")
      .append(Checkpoint.IdempotencyKey)
      .append("|")
      .append(Checkpoint.ActionId)
      .append("|")
      .append(std::to_string(ToMilliseconds(Checkpoint.UpdatedAt)))
      .append("|")
      .append(std::to_string(Checkpoint.Revision));
  const std::string RecordChecksum = ChecksumText(Checksum(Payload));
  Payload.append("|").append(RecordChecksum).append("\n");
  return Payload;
}

std::string EncodeScope(const ScopeCheckpoint& Checkpoint) {
  std::string Payload;
  Payload.reserve(192);
  Payload.append(RecordPrefix)
      .append("|")
      .append(RecordVersion)
      .append("|S|")
      .append(Checkpoint.ScopeRef)
      .append("|")
      .append(ScopeStateName(Checkpoint.State))
      .append("|")
      .append(std::to_string(ToMilliseconds(Checkpoint.UpdatedAt)))
      .append("|")
      .append(std::to_string(Checkpoint.Revision));
  const std::string RecordChecksum = ChecksumText(Checksum(Payload));
  Payload.append("|").append(RecordChecksum).append("\n");
  return Payload;
}

enum class LineReadStatus : std::uint8_t {
  Line,
  End,
  TooLarge,
  Incomplete,
  IoFailure
};

LineReadStatus ReadBoundedLine(std::istream& Input, std::string& Line) {
  Line.clear();
  char Character = '\0';
  while (Input.get(Character)) {
    if (Character == '\n') {
      return LineReadStatus::Line;
    }
    if (Line.size() >= MaximumRecordBytes) {
      return LineReadStatus::TooLarge;
    }
    Line.push_back(Character);
  }
  if (!Line.empty()) {
    return LineReadStatus::Incomplete;
  }
  if (Input.bad()) {
    return LineReadStatus::IoFailure;
  }
  return LineReadStatus::End;
}

CheckpointResult<std::monostate> ValidateChecksumAndSplit(
    const std::string& Line, std::vector<std::string_view>& Fields) {
  Fields = SplitFields(Line);
  if (Fields.size() < 2 || Fields.back().size() != 16U) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Checkpoint checksum is malformed.");
  }
  const std::uint64_t Expected = Checksum(
      std::string_view(Line).substr(0, Line.size() - Fields.back().size() - 1U));
  const std::string_view ActualText = Fields.back();
  std::uint64_t Actual = 0;
  const auto Parsed =
      std::from_chars(ActualText.data(),
                      ActualText.data() + ActualText.size(), Actual, 16);
  if (Parsed.ec != std::errc{} ||
      Parsed.ptr != ActualText.data() + ActualText.size() ||
      Actual != Expected) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Checkpoint checksum does not match.");
  }
  return std::monostate{};
}

// A correlation field may be filled once (empty -> value) but must never be
// cleared or replaced on a later revision of the same turn.
bool StableFieldCompatible(const std::string& Previous,
                           const std::string& Requested) noexcept {
  return Previous.empty() || Previous == Requested;
}

CheckpointResult<std::monostate> ParseTurnRecord(
    const std::vector<std::string_view>& Fields,
    CheckpointSnapshot& Snapshot) {
  if (Fields.size() < 3U || Fields[0] != RecordPrefix ||
      Fields[2] != "T") {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Turn checkpoint record is malformed.");
  }
  const bool Legacy = Fields[1] == RecordVersion;
  const bool Current = Fields[1] == TurnRecordVersion;
  if (!Legacy && !Current) {
    return Error(CheckpointStoreErrorCode::UnsupportedVersion,
                 "Turn checkpoint record version is not supported.");
  }
  if ((Legacy && Fields.size() != 12U) ||
      (Current && Fields.size() != 16U)) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Turn checkpoint record is malformed.");
  }
  TurnCheckpoint Checkpoint;
  Checkpoint.TurnId = std::string(Fields[3]);
  Checkpoint.RequestId = std::string(Fields[4]);
  Checkpoint.ScopeRef = std::string(Fields[5]);
  Checkpoint.ContextReference = std::string(Fields[6]);
  const auto Phase = ParseTurnPhase(Fields[7]);
  const auto ToolState = ParseToolState(Fields[8]);
  const auto UpdatedAt =
      FromMilliseconds(Fields[Current ? 14U : 10U]);
  const auto Revision = ParseUnsigned(Fields[Current ? 15U : 11U]);
  std::optional<OperationState> Operation = OperationState::None;
  if (Current) {
    Operation = ParseOperationState(Fields[10]);
    Checkpoint.OperationId = std::string(Fields[11]);
    Checkpoint.IdempotencyKey = std::string(Fields[12]);
    Checkpoint.ActionId = std::string(Fields[13]);
  }
  if (!Phase.has_value() || !ToolState.has_value() ||
      !Operation.has_value() || !UpdatedAt.has_value() ||
      !Revision.has_value() ||
      (Fields[9] != "0" && Fields[9] != "1")) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Turn checkpoint fields are corrupt.");
  }
  Checkpoint.Phase = *Phase;
  Checkpoint.ToolState = *ToolState;
  Checkpoint.Operation = *Operation;
  Checkpoint.UnknownSideEffect = Fields[9] == "1";
  Checkpoint.UpdatedAt = *UpdatedAt;
  Checkpoint.Revision = *Revision;
  const CheckpointResult<std::monostate> Valid = ValidateTurn(Checkpoint);
  if (const auto* Failure = std::get_if<CheckpointStoreError>(&Valid);
      Failure != nullptr) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Turn checkpoint violates storage invariants.");
  }
  const auto Existing = Snapshot.LatestByTurn.find(Checkpoint.TurnId);
  if (Existing != Snapshot.LatestByTurn.end()) {
    const TurnCheckpoint& Previous = Existing->second;
    if (Previous.ScopeRef != Checkpoint.ScopeRef ||
        Checkpoint.Revision != Previous.Revision + 1U ||
        !CanAdvanceTurn(Previous.Phase, Checkpoint.Phase) ||
        !CanAdvanceOperation(Previous.Operation, Checkpoint.Operation) ||
        (Previous.UnknownSideEffect && !Checkpoint.UnknownSideEffect) ||
        !StableFieldCompatible(Previous.RequestId, Checkpoint.RequestId) ||
        !StableFieldCompatible(Previous.ContextReference,
                               Checkpoint.ContextReference) ||
        !StableFieldCompatible(Previous.OperationId,
                               Checkpoint.OperationId) ||
        !StableFieldCompatible(Previous.IdempotencyKey,
                               Checkpoint.IdempotencyKey) ||
        !StableFieldCompatible(Previous.ActionId, Checkpoint.ActionId)) {
      return Error(CheckpointStoreErrorCode::CorruptRecord,
                   "Turn checkpoint revision history is corrupt.");
    }
  } else if (Checkpoint.Revision != 1U) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Initial turn checkpoint revision is corrupt.");
  }
  Snapshot.LatestByTurn[Checkpoint.TurnId] = std::move(Checkpoint);
  return std::monostate{};
}

CheckpointResult<std::monostate> ParseScopeRecord(
    const std::vector<std::string_view>& Fields,
    CheckpointSnapshot& Snapshot) {
  if (Fields.size() != 7U || Fields[0] != RecordPrefix ||
      Fields[2] != "S") {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Scope checkpoint record is malformed.");
  }
  if (Fields[1] != RecordVersion) {
    return Error(CheckpointStoreErrorCode::UnsupportedVersion,
                 "Scope checkpoint record version is not supported.");
  }
  ScopeCheckpoint Checkpoint;
  Checkpoint.ScopeRef = std::string(Fields[3]);
  const auto State = ParseScopeState(Fields[4]);
  const auto UpdatedAt = FromMilliseconds(Fields[5]);
  const auto Revision = ParseUnsigned(Fields[6]);
  if (!State.has_value() || !UpdatedAt.has_value() ||
      !Revision.has_value()) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Scope checkpoint fields are corrupt.");
  }
  Checkpoint.State = *State;
  Checkpoint.UpdatedAt = *UpdatedAt;
  Checkpoint.Revision = *Revision;
  const CheckpointResult<std::monostate> Valid = ValidateScope(Checkpoint);
  if (const auto* Failure = std::get_if<CheckpointStoreError>(&Valid);
      Failure != nullptr) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Scope checkpoint violates storage invariants.");
  }
  const auto Existing = Snapshot.LatestScopeById.find(Checkpoint.ScopeRef);
  if (Existing != Snapshot.LatestScopeById.end()) {
    if (Checkpoint.Revision != Existing->second.Revision + 1U ||
        Existing->second.State == Checkpoint.State ||
        !CanAdvanceScope(Existing->second.State, Checkpoint.State)) {
      return Error(CheckpointStoreErrorCode::CorruptRecord,
                   "Scope checkpoint revision history is corrupt.");
    }
  } else if (Checkpoint.Revision != 1U) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Initial scope checkpoint revision is corrupt.");
  }
  Snapshot.LatestScopeById[Checkpoint.ScopeRef] = std::move(Checkpoint);
  return std::monostate{};
}

CheckpointResult<std::monostate> ParseRecord(
    const std::string& Line, CheckpointSnapshot& Snapshot) {
  if (Line.empty() || Line.size() > MaximumRecordBytes) {
    return Error(CheckpointStoreErrorCode::CorruptRecord,
                 "Checkpoint record size is invalid.");
  }
  std::vector<std::string_view> Fields;
  const CheckpointResult<std::monostate> ChecksumResult =
      ValidateChecksumAndSplit(Line, Fields);
  if (const auto* Failure =
          std::get_if<CheckpointStoreError>(&ChecksumResult);
      Failure != nullptr) {
    return *Failure;
  }
  Fields.pop_back();
  if (Fields.size() >= 3U && Fields[2] == "T") {
    return ParseTurnRecord(Fields, Snapshot);
  }
  if (Fields.size() >= 3U && Fields[2] == "S") {
    return ParseScopeRecord(Fields, Snapshot);
  }
  return Error(CheckpointStoreErrorCode::CorruptRecord,
               "Checkpoint record kind is unsupported.");
}

CheckpointResult<std::monostate> AppendLine(
    const std::filesystem::path& Path, std::string_view Line) {
  std::error_code DirectoryError;
  if (Path.has_parent_path()) {
    std::filesystem::create_directories(Path.parent_path(), DirectoryError);
    if (DirectoryError) {
      return Error(CheckpointStoreErrorCode::IoFailure,
                   "Checkpoint directory could not be created.");
    }
  }
  std::ofstream Output(Path, std::ios::binary | std::ios::app);
  if (!Output) {
    return Error(CheckpointStoreErrorCode::IoFailure,
                 "Checkpoint file could not be opened.");
  }
  Output.write(Line.data(), static_cast<std::streamsize>(Line.size()));
  Output.flush();
  if (!Output) {
    return Error(CheckpointStoreErrorCode::IoFailure,
                 "Checkpoint record could not be persisted.");
  }
  return std::monostate{};
}

} // namespace

TurnCheckpointStore::TurnCheckpointStore(std::filesystem::path FilePath)
    : Path(std::move(FilePath)) {}

bool TurnCheckpointStore::IsValidIdentifier(
    std::string_view Identifier) noexcept {
  if (Identifier.empty() ||
      Identifier.size() > MaximumIdentifierBytes) {
    return false;
  }
  const auto First =
      static_cast<unsigned char>(Identifier.front());
  if (!((First >= static_cast<unsigned char>('a') &&
         First <= static_cast<unsigned char>('z')) ||
        (First >= static_cast<unsigned char>('A') &&
         First <= static_cast<unsigned char>('Z')) ||
        (First >= static_cast<unsigned char>('0') &&
         First <= static_cast<unsigned char>('9')))) {
    return false;
  }
  for (const char Character : Identifier) {
    if (!IsContinueCharacter(static_cast<unsigned char>(Character))) {
      return false;
    }
  }
  return true;
}

bool TurnCheckpointStore::IsTerminal(TurnPhase Phase) noexcept {
  return Phase == TurnPhase::Completed || Phase == TurnPhase::Cancelled ||
         Phase == TurnPhase::Failed || Phase == TurnPhase::Unsupported ||
         Phase == TurnPhase::OutcomeUnknown;
}

bool TurnCheckpointStore::ContainsForbiddenContent(
    std::string_view Value) noexcept {
  return Value.find(SecretSentinel) != std::string_view::npos;
}

CheckpointResult<std::monostate> TurnCheckpointStore::Append(
    const TurnCheckpoint& Checkpoint) {
  const CheckpointResult<std::monostate> Validation =
      ValidateTurn(Checkpoint);
  if (const auto* Failure =
          std::get_if<CheckpointStoreError>(&Validation);
      Failure != nullptr) {
    return *Failure;
  }
  const std::lock_guard<std::mutex> Lock(Mutex);
  const CheckpointResult<CheckpointSnapshot> Loaded = LoadUnlocked();
  if (const auto* Failure =
          std::get_if<CheckpointStoreError>(&Loaded);
      Failure != nullptr) {
    return *Failure;
  }
  const CheckpointSnapshot& Snapshot =
      std::get<CheckpointSnapshot>(Loaded);
  const auto Existing = Snapshot.LatestByTurn.find(Checkpoint.TurnId);
  if (Existing == Snapshot.LatestByTurn.end()) {
    if (Checkpoint.Revision != 1U) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "A new checkpoint must start at revision one.");
    }
    if (Checkpoint.Phase != TurnPhase::Created &&
        Checkpoint.Phase != TurnPhase::ReadyToDispatch) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "A new checkpoint must start before dispatch.");
    }
  } else {
    const TurnCheckpoint& Previous = Existing->second;
    if (SameTurnState(Previous, Checkpoint)) {
      return std::monostate{};
    }
    if (Previous.ScopeRef != Checkpoint.ScopeRef ||
        Checkpoint.Revision != Previous.Revision + 1U) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "Checkpoint identity or revision is invalid.");
    }
    if (!StableFieldCompatible(Previous.RequestId, Checkpoint.RequestId) ||
        !StableFieldCompatible(Previous.ContextReference,
                               Checkpoint.ContextReference) ||
        !StableFieldCompatible(Previous.OperationId,
                               Checkpoint.OperationId) ||
        !StableFieldCompatible(Previous.IdempotencyKey,
                               Checkpoint.IdempotencyKey) ||
        !StableFieldCompatible(Previous.ActionId, Checkpoint.ActionId)) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "Checkpoint correlation identifiers drifted.");
    }
    if (IsTerminal(Previous.Phase) ||
        (Previous.UnknownSideEffect && !Checkpoint.UnknownSideEffect)) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "Terminal or isolated checkpoints cannot be reopened.");
    }
    if (!CanAdvanceTurn(Previous.Phase, Checkpoint.Phase)) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "Turn phase transition is invalid.");
    }
    if (!CanAdvanceOperation(Previous.Operation, Checkpoint.Operation)) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "Operation state transition is invalid.");
    }
  }
  const CheckpointResult<std::monostate> Persisted =
      AppendLine(Path, EncodeTurn(Checkpoint));
  if (const auto* Failure =
          std::get_if<CheckpointStoreError>(&Persisted);
      Failure != nullptr) {
    return *Failure;
  }
  return std::monostate{};
}

CheckpointResult<std::monostate> TurnCheckpointStore::MarkScope(
    const ScopeCheckpoint& Checkpoint) {
  const CheckpointResult<std::monostate> Validation =
      ValidateScope(Checkpoint);
  if (const auto* Failure =
          std::get_if<CheckpointStoreError>(&Validation);
      Failure != nullptr) {
    return *Failure;
  }
  const std::lock_guard<std::mutex> Lock(Mutex);
  const CheckpointResult<CheckpointSnapshot> Loaded = LoadUnlocked();
  if (const auto* Failure =
          std::get_if<CheckpointStoreError>(&Loaded);
      Failure != nullptr) {
    return *Failure;
  }
  const CheckpointSnapshot& Snapshot =
      std::get<CheckpointSnapshot>(Loaded);
  const auto Existing =
      Snapshot.LatestScopeById.find(Checkpoint.ScopeRef);
  if (Existing == Snapshot.LatestScopeById.end()) {
    if (Checkpoint.Revision != 1U) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "A new scope checkpoint must start at revision one.");
    }
  } else {
    const ScopeCheckpoint& Previous = Existing->second;
    if (Previous.State == Checkpoint.State) {
      if (Previous.ScopeRef == Checkpoint.ScopeRef &&
          Previous.Revision == Checkpoint.Revision &&
          Previous.Version == Checkpoint.Version) {
        return std::monostate{};
      }
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "Scope checkpoint retry is inconsistent.");
    }
    if (Checkpoint.Revision != Previous.Revision + 1U ||
        !CanAdvanceScope(Previous.State, Checkpoint.State)) {
      return Error(CheckpointStoreErrorCode::InvalidTransition,
                   "Scope checkpoint transition is invalid.");
    }
  }
  return AppendLine(Path, EncodeScope(Checkpoint));
}

CheckpointResult<CheckpointSnapshot> TurnCheckpointStore::Load() const {
  const std::lock_guard<std::mutex> Lock(Mutex);
  return LoadUnlocked();
}

CheckpointResult<CheckpointSnapshot> TurnCheckpointStore::LoadUnlocked()
    const {
  CheckpointSnapshot Snapshot;
  std::error_code ExistsError;
  const bool Exists = std::filesystem::exists(Path, ExistsError);
  if (ExistsError) {
    return Error(CheckpointStoreErrorCode::IoFailure,
                 "Checkpoint file could not be inspected.");
  }
  if (!Exists) {
    return Snapshot;
  }

  std::ifstream Input(Path, std::ios::binary);
  if (!Input) {
    return Error(CheckpointStoreErrorCode::IoFailure,
                 "Checkpoint file could not be opened.");
  }
  std::string Line;
  while (true) {
    const LineReadStatus Status = ReadBoundedLine(Input, Line);
    if (Status == LineReadStatus::End) {
      break;
    }
    if (Status == LineReadStatus::TooLarge) {
      return Error(CheckpointStoreErrorCode::CorruptRecord,
                   "Checkpoint record exceeds the size limit.");
    }
    if (Status == LineReadStatus::Incomplete) {
      return Error(CheckpointStoreErrorCode::CorruptRecord,
                   "Checkpoint record is truncated.");
    }
    if (Status == LineReadStatus::IoFailure) {
      return Error(CheckpointStoreErrorCode::IoFailure,
                   "Checkpoint file could not be read.");
    }
    const CheckpointResult<std::monostate> Parsed =
        ParseRecord(Line, Snapshot);
    if (const auto* Failure =
            std::get_if<CheckpointStoreError>(&Parsed);
        Failure != nullptr) {
      return *Failure;
    }
    if (Snapshot.RecordCount ==
        std::numeric_limits<std::uint64_t>::max()) {
      return Error(CheckpointStoreErrorCode::CorruptRecord,
                   "Checkpoint record count overflowed.");
    }
    ++Snapshot.RecordCount;
  }
  return Snapshot;
}

} // namespace EmulativeDialog::Storage
