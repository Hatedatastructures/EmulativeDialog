#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>

namespace EmulativeDialog::Storage {

enum class TurnPhase : std::uint8_t {
  Created,
  ReadyToDispatch,
  ProviderDispatched,
  ProviderStreaming,
  ProviderCompleted,
  ToolDispatched,
  Sending,
  Completed,
  Cancelled,
  Failed,
  Unsupported,
  OutcomeUnknown
};

enum class ToolExecutionState : std::uint8_t {
  NotRequested,
  Authorized,
  Dispatched,
  Completed,
  Failed,
  UnknownOutcome
};

enum class OperationState : std::uint8_t {
  None,
  Prepared,
  Authorized,
  Dispatched,
  Succeeded,
  Rejected,
  Unsupported,
  Failed,
  UnknownOutcome
};

enum class ScopeState : std::uint8_t {
  Active,
  Revoked,
  Deleted,
  Tombstoned
};

// A checkpoint contains correlation and lifecycle state only. ContextReference
// is an opaque lookup key into Core-owned session state; prompts, messages,
// credentials, tool arguments, and tool results never belong here.
struct TurnCheckpoint final {
  std::string TurnId;
  std::string RequestId;
  std::string ScopeRef;
  std::string ContextReference;
  TurnPhase Phase{TurnPhase::Created};
  ToolExecutionState ToolState{ToolExecutionState::NotRequested};
  OperationState Operation{OperationState::None};
  // Reconciliation identity for an authorized business action. Persisting these
  // lets a restarted Core resume or reconcile the exact operation with the same
  // idempotency key instead of creating a duplicate side effect.
  std::string OperationId;
  std::string IdempotencyKey;
  std::string ActionId;
  bool UnknownSideEffect{false};
  std::chrono::system_clock::time_point UpdatedAt{};
  std::uint64_t Revision{1};
  std::uint32_t Version{1};
};

struct ScopeCheckpoint final {
  std::string ScopeRef;
  ScopeState State{ScopeState::Active};
  std::chrono::system_clock::time_point UpdatedAt{};
  std::uint64_t Revision{1};
  std::uint32_t Version{1};
};

enum class CheckpointStoreErrorCode : std::uint8_t {
  IoFailure,
  CorruptRecord,
  UnsupportedVersion,
  InvalidCheckpoint,
  InvalidTransition
};

struct CheckpointStoreError final {
  CheckpointStoreErrorCode Code{CheckpointStoreErrorCode::IoFailure};
  // Stable, sanitized diagnostics only. Never include identifiers, context
  // values, message text, credentials, or parser input.
  std::string SanitizedDescription;
};

template <typename Value>
using CheckpointResult = std::variant<Value, CheckpointStoreError>;

struct CheckpointSnapshot final {
  std::map<std::string, TurnCheckpoint> LatestByTurn;
  std::map<std::string, ScopeCheckpoint> LatestScopeById;
  std::uint64_t RecordCount{0};
};

// Append-only, versioned checkpoint store for interrupted-turn recovery.
// Exact retries are idempotent. Revision changes must be contiguous, terminal
// turns cannot be reopened, and any malformed or truncated record makes the
// complete snapshot fail closed.
//
// Single-writer contract: one owner (Core strand or equivalent serialized
// executor) owns writes for a checkpoint file. Append/MarkScope are also
// serialized by an internal mutex so concurrent writers cannot interleave
// records or fork revisions; they are not a substitute for the Core owner.
class TurnCheckpointStore final {
public:
  inline static constexpr std::string_view SecretSentinel{
      "SECRET_SENTINEL"};

  explicit TurnCheckpointStore(std::filesystem::path FilePath);

  [[nodiscard]] CheckpointResult<std::monostate> Append(
      const TurnCheckpoint& Checkpoint);
  [[nodiscard]] CheckpointResult<std::monostate> MarkScope(
      const ScopeCheckpoint& Checkpoint);
  [[nodiscard]] CheckpointResult<CheckpointSnapshot> Load() const;

  [[nodiscard]] const std::filesystem::path& FilePath() const noexcept {
    return Path;
  }

  [[nodiscard]] static bool IsValidIdentifier(
      std::string_view Identifier) noexcept;
  [[nodiscard]] static bool IsTerminal(TurnPhase Phase) noexcept;
  [[nodiscard]] static bool ContainsForbiddenContent(
      std::string_view Value) noexcept;

private:
  [[nodiscard]] CheckpointResult<CheckpointSnapshot> LoadUnlocked() const;

  std::filesystem::path Path;
  mutable std::mutex Mutex;
};

} // namespace EmulativeDialog::Storage
