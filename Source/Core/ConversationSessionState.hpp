#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "Source/Core/AgentRuntimePort.h"

namespace EmulativeDialog::Core {

enum class SessionTurnState : std::uint8_t {
  Active,
  Completed,
  Cancelled,
  UnknownOutcome,
};

struct IncomingSessionMessage final {
  Channels::MessageId Message;
  Channels::StableDeduplicationId Deduplication;
  Channels::CanonicalMessage Content;
  Channels::Timestamp OccurredAt{};
};

struct SessionMessageRecord final {
  std::uint64_t SequenceNumber{};
  Channels::MessageId Message;
  Channels::StableDeduplicationId Deduplication;
  Channels::CanonicalMessage Content;
  Channels::Timestamp OccurredAt{};
};

struct SessionTurnRecord final {
  Providers::AgentTurnId AgentTurn;
  SessionTurnState State{SessionTurnState::Active};
};

struct ConversationSessionSnapshot final {
  ConversationScope Scope;
  std::vector<SessionMessageRecord> Messages;
  std::vector<SessionTurnRecord> Turns;
};

enum class SessionFailureCode : std::uint8_t {
  InvalidScope,
  InvalidMessage,
  DuplicateMessage,
  InvalidTurn,
  DuplicateTurn,
  ActiveTurnExists,
  UnknownScope,
  UnknownTurn,
  InvalidTurnTransition,
};

struct SessionFailure final {
  SessionFailureCode Code{SessionFailureCode::InvalidScope};

  friend bool operator==(const SessionFailure&, const SessionFailure&) =
      default;
};

using SessionMutationResult =
    std::variant<std::monostate, SessionFailure>;
using SessionSnapshotResult =
    std::variant<ConversationSessionSnapshot, SessionFailure>;

// In-memory, single-owner state for authorized conversations. Persistence and
// restart recovery are intentionally outside this type.
class ConversationSessionState final {
public:
  ConversationSessionState();
  ~ConversationSessionState();

  ConversationSessionState(const ConversationSessionState&) = delete;
  ConversationSessionState& operator=(const ConversationSessionState&) =
      delete;
  ConversationSessionState(ConversationSessionState&&) = delete;
  ConversationSessionState& operator=(ConversationSessionState&&) = delete;

  [[nodiscard]] SessionMutationResult AppendMessage(
      const ConversationScope& Scope,
      IncomingSessionMessage Message);

  [[nodiscard]] SessionMutationResult BeginTurn(
      const ConversationScope& Scope,
      Providers::AgentTurnId AgentTurn);

  [[nodiscard]] SessionMutationResult EndTurn(
      const ConversationScope& Scope,
      const Providers::AgentTurnId& AgentTurn,
      SessionTurnState TerminalState);

  [[nodiscard]] SessionSnapshotResult GetSnapshot(
      const ConversationScope& Scope) const;

private:
  class Impl;
  std::unique_ptr<Impl> State;
};

} // namespace EmulativeDialog::Core
