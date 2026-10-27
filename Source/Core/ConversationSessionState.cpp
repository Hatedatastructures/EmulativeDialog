#include "Source/Core/ConversationSessionState.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace EmulativeDialog::Core {
namespace {

struct ExactScopeKey final {
  std::string Principal;
  std::string Channel;
  std::string Account;
  std::string Mode;
  std::string Conversation;
  Channels::ConversationKind ConversationType{
      Channels::ConversationKind::Private};
  std::string Sender;

  friend bool operator==(const ExactScopeKey&, const ExactScopeKey&) =
      default;
};

void CombineHash(std::size_t& Seed, std::string_view Value) {
  Seed ^= std::hash<std::string_view>{}(Value) +
          static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
          (Seed << 6U) + (Seed >> 2U);
}

struct ExactScopeHash final {
  [[nodiscard]] std::size_t operator()(const ExactScopeKey& Key) const {
    std::size_t Seed = 0;
    CombineHash(Seed, Key.Principal);
    CombineHash(Seed, Key.Channel);
    CombineHash(Seed, Key.Account);
    CombineHash(Seed, Key.Mode);
    CombineHash(Seed, Key.Conversation);
    Seed ^= std::hash<int>{}(
                static_cast<int>(Key.ConversationType)) +
            static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
            (Seed << 6U) + (Seed >> 2U);
    CombineHash(Seed, Key.Sender);
    return Seed;
  }
};

[[nodiscard]] bool IsValidScope(const ConversationScope& Scope) {
  return !Scope.Principal.Value.empty() && !Scope.Channel.Value.empty() &&
         !Scope.Account.Value.empty() && !Scope.Mode.Value.empty() &&
         !Scope.Conversation.Value.empty() && !Scope.Sender.Value.empty();
}

[[nodiscard]] ExactScopeKey MakeKey(const ConversationScope& Scope) {
  return ExactScopeKey{
      .Principal = Scope.Principal.Value,
      .Channel = Scope.Channel.Value,
      .Account = Scope.Account.Value,
      .Mode = Scope.Mode.Value,
      .Conversation = Scope.Conversation.Value,
      .ConversationType = Scope.ConversationType,
      .Sender = Scope.Sender.Value,
  };
}

[[nodiscard]] bool IsTerminal(SessionTurnState State) {
  return State == SessionTurnState::Completed ||
         State == SessionTurnState::Cancelled ||
         State == SessionTurnState::UnknownOutcome;
}

[[nodiscard]] SessionMutationResult Failure(SessionFailureCode Code) {
  return SessionFailure{.Code = Code};
}

struct SessionData final {
  ConversationScope Scope;
  std::vector<SessionMessageRecord> Messages;
  std::vector<SessionTurnRecord> Turns;
  std::unordered_set<std::string> MessageIds;
  std::unordered_set<std::string> DeduplicationIds;
  std::unordered_map<std::string, std::size_t> TurnIndexes;
  std::optional<std::size_t> ActiveTurnIndex;
};

} // namespace

class ConversationSessionState::Impl final {
public:
  std::unordered_map<ExactScopeKey, SessionData, ExactScopeHash> Sessions;
};

ConversationSessionState::ConversationSessionState()
    : State(std::make_unique<Impl>()) {}

ConversationSessionState::~ConversationSessionState() = default;

SessionMutationResult ConversationSessionState::AppendMessage(
    const ConversationScope& Scope,
    IncomingSessionMessage Message) {
  if (!IsValidScope(Scope)) {
    return Failure(SessionFailureCode::InvalidScope);
  }
  if (Message.Message.Value.empty() ||
      Message.Deduplication.Value.empty()) {
    return Failure(SessionFailureCode::InvalidMessage);
  }

  SessionData& Session = State->Sessions[MakeKey(Scope)];
  if (Session.Scope.Principal.Value.empty()) {
    Session.Scope = Scope;
  }
  if (Session.MessageIds.contains(Message.Message.Value) ||
      Session.DeduplicationIds.contains(Message.Deduplication.Value)) {
    return Failure(SessionFailureCode::DuplicateMessage);
  }
  Session.MessageIds.insert(Message.Message.Value);
  Session.DeduplicationIds.insert(Message.Deduplication.Value);

  Session.Messages.push_back(SessionMessageRecord{
      .SequenceNumber =
          static_cast<std::uint64_t>(Session.Messages.size() + 1U),
      .Message = std::move(Message.Message),
      .Deduplication = std::move(Message.Deduplication),
      .Content = std::move(Message.Content),
      .OccurredAt = Message.OccurredAt,
  });
  return std::monostate{};
}

SessionMutationResult ConversationSessionState::BeginTurn(
    const ConversationScope& Scope,
    Providers::AgentTurnId AgentTurn) {
  if (!IsValidScope(Scope)) {
    return Failure(SessionFailureCode::InvalidScope);
  }
  if (AgentTurn.Value.empty()) {
    return Failure(SessionFailureCode::InvalidTurn);
  }

  SessionData& Session = State->Sessions[MakeKey(Scope)];
  if (Session.Scope.Principal.Value.empty()) {
    Session.Scope = Scope;
  }
  if (Session.TurnIndexes.contains(AgentTurn.Value)) {
    return Failure(SessionFailureCode::DuplicateTurn);
  }
  if (Session.ActiveTurnIndex.has_value()) {
    return Failure(SessionFailureCode::ActiveTurnExists);
  }

  const std::size_t Index = Session.Turns.size();
  Session.Turns.push_back(SessionTurnRecord{
      .AgentTurn = std::move(AgentTurn),
      .State = SessionTurnState::Active,
  });
  Session.TurnIndexes.emplace(Session.Turns.back().AgentTurn.Value, Index);
  Session.ActiveTurnIndex = Index;
  return std::monostate{};
}

SessionMutationResult ConversationSessionState::EndTurn(
    const ConversationScope& Scope,
    const Providers::AgentTurnId& AgentTurn,
    SessionTurnState TerminalState) {
  if (!IsValidScope(Scope)) {
    return Failure(SessionFailureCode::InvalidScope);
  }
  if (!IsTerminal(TerminalState)) {
    return Failure(SessionFailureCode::InvalidTurnTransition);
  }

  const auto SessionIterator = State->Sessions.find(MakeKey(Scope));
  if (SessionIterator == State->Sessions.end()) {
    return Failure(SessionFailureCode::UnknownScope);
  }
  SessionData& Session = SessionIterator->second;
  const auto TurnIterator = Session.TurnIndexes.find(AgentTurn.Value);
  if (TurnIterator == Session.TurnIndexes.end()) {
    return Failure(SessionFailureCode::UnknownTurn);
  }

  SessionTurnRecord& Turn = Session.Turns[TurnIterator->second];
  if (Turn.State != SessionTurnState::Active) {
    return Failure(SessionFailureCode::InvalidTurnTransition);
  }
  Turn.State = TerminalState;
  if (Session.ActiveTurnIndex == TurnIterator->second) {
    Session.ActiveTurnIndex.reset();
  }
  return std::monostate{};
}

SessionSnapshotResult ConversationSessionState::GetSnapshot(
    const ConversationScope& Scope) const {
  if (!IsValidScope(Scope)) {
    return SessionFailure{.Code = SessionFailureCode::InvalidScope};
  }
  const auto SessionIterator = State->Sessions.find(MakeKey(Scope));
  if (SessionIterator == State->Sessions.end()) {
    return SessionFailure{.Code = SessionFailureCode::UnknownScope};
  }
  return ConversationSessionSnapshot{
      .Scope = SessionIterator->second.Scope,
      .Messages = SessionIterator->second.Messages,
      .Turns = SessionIterator->second.Turns,
  };
}

} // namespace EmulativeDialog::Core
