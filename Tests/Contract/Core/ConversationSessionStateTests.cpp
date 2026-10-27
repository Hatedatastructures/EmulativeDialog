#include "Source/Core/ConversationSessionState.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using EmulativeDialog::Channels::AccountId;
using EmulativeDialog::Channels::ChannelId;
using EmulativeDialog::Channels::ConversationId;
using EmulativeDialog::Channels::ConversationKind;
using EmulativeDialog::Channels::IntegrationMode;
using EmulativeDialog::Channels::MessageId;
using EmulativeDialog::Channels::SenderId;
using EmulativeDialog::Channels::StableDeduplicationId;
using EmulativeDialog::Channels::TextPart;
using EmulativeDialog::Core::ConversationScope;
using EmulativeDialog::Core::ConversationSessionSnapshot;
using EmulativeDialog::Core::ConversationSessionState;
using EmulativeDialog::Core::IncomingSessionMessage;
using EmulativeDialog::Core::PrincipalId;
using EmulativeDialog::Core::SessionFailure;
using EmulativeDialog::Core::SessionFailureCode;
using EmulativeDialog::Core::SessionMutationResult;
using EmulativeDialog::Core::SessionSnapshotResult;
using EmulativeDialog::Core::SessionTurnState;
using EmulativeDialog::Providers::AgentTurnId;

void Check(bool Condition, std::string_view Message, int Line) {
  if (!Condition) {
    throw std::runtime_error("line " + std::to_string(Line) + ": " +
                             std::string(Message));
  }
}

#define CHECK(Condition) Check((Condition), #Condition, __LINE__)

ConversationScope MakeScope() {
  return ConversationScope{
      .Principal = PrincipalId{.Value = "principal-01"},
      .Channel = ChannelId{.Value = "qq"},
      .Account = AccountId{.Value = "account-01"},
      .Mode = IntegrationMode{.Value = "official"},
      .Conversation = ConversationId{.Value = "conversation-01"},
      .ConversationType = ConversationKind::Private,
      .Sender = SenderId{.Value = "sender-01"},
  };
}

IncomingSessionMessage MakeMessage(std::string Id,
                                   std::string Dedup,
                                   std::string Text,
                                   std::chrono::seconds Offset) {
  return IncomingSessionMessage{
      .Message = MessageId{.Value = std::move(Id)},
      .Deduplication = StableDeduplicationId{.Value = std::move(Dedup)},
      .Content =
          EmulativeDialog::Channels::CanonicalMessage{
              .MessageParts = {TextPart{.Utf8Text = std::move(Text)}},
              .ReplyToId = std::nullopt,
              .MentionedIds = {},
              .PlatformMetadata = {},
          },
      .OccurredAt =
          EmulativeDialog::Channels::Timestamp{
              std::chrono::seconds{1'800'000'000} + Offset},
  };
}

SessionFailureCode MutationFailure(const SessionMutationResult& Result) {
  const auto* Failure = std::get_if<SessionFailure>(&Result);
  CHECK(Failure != nullptr);
  return Failure->Code;
}

ConversationSessionSnapshot Snapshot(const SessionSnapshotResult& Result) {
  const auto* Value =
      std::get_if<ConversationSessionSnapshot>(&Result);
  CHECK(Value != nullptr);
  return *Value;
}

SessionFailureCode SnapshotFailure(const SessionSnapshotResult& Result) {
  const auto* Failure = std::get_if<SessionFailure>(&Result);
  CHECK(Failure != nullptr);
  return Failure->Code;
}

bool IsSuccess(const SessionMutationResult& Result) {
  return std::holds_alternative<std::monostate>(Result);
}

void TestMessageOrderAndSequenceAreSeparateFromTimestamps() {
  ConversationSessionState Store;
  const ConversationScope Scope = MakeScope();
  CHECK(IsSuccess(Store.AppendMessage(
      Scope, MakeMessage("message-newer", "dedup-newer", "newer",
                         std::chrono::seconds{10}))));
  CHECK(IsSuccess(Store.AppendMessage(
      Scope, MakeMessage("message-older", "dedup-older", "older",
                         std::chrono::seconds{-10}))));

  const ConversationSessionSnapshot Value =
      Snapshot(Store.GetSnapshot(Scope));
  CHECK(Value.Messages.size() == 2);
  CHECK(Value.Messages[0].SequenceNumber == 1);
  CHECK(Value.Messages[1].SequenceNumber == 2);
  CHECK(Value.Messages[0].Message.Value == "message-newer");
  CHECK(Value.Messages[1].Message.Value == "message-older");
  CHECK(std::get<TextPart>(Value.Messages[0].Content.MessageParts[0])
            .Utf8Text == "newer");
}

void TestDuplicateMessageAndDeduplicationFailClosed() {
  ConversationSessionState Store;
  const ConversationScope Scope = MakeScope();
  CHECK(IsSuccess(Store.AppendMessage(
      Scope, MakeMessage("message-1", "dedup-1", "first", {}))));
  CHECK(MutationFailure(Store.AppendMessage(
            Scope,
            MakeMessage("message-1", "dedup-2", "duplicate-id", {}))) ==
        SessionFailureCode::DuplicateMessage);
  CHECK(MutationFailure(Store.AppendMessage(
            Scope,
            MakeMessage("message-2", "dedup-1", "duplicate-dedup", {}))) ==
        SessionFailureCode::DuplicateMessage);

  const ConversationSessionSnapshot Value =
      Snapshot(Store.GetSnapshot(Scope));
  CHECK(Value.Messages.size() == 1);

  // A rejected duplicate must not reserve the other identifier.
  CHECK(IsSuccess(Store.AppendMessage(
      Scope, MakeMessage("message-2", "dedup-2", "second", {}))));
  CHECK(IsSuccess(Store.AppendMessage(
      Scope, MakeMessage("message-3", "dedup-3", "third", {}))));
  CHECK(Snapshot(Store.GetSnapshot(Scope)).Messages.size() == 3);
}

void TestInvalidScopeAndMessageAreRejected() {
  ConversationSessionState Store;
  ConversationScope Scope = MakeScope();
  Scope.Principal.Value.clear();
  CHECK(MutationFailure(Store.AppendMessage(
            Scope, MakeMessage("message-1", "dedup-1", "text", {}))) ==
        SessionFailureCode::InvalidScope);

  Scope = MakeScope();
  Scope.Mode.Value.clear();
  CHECK(MutationFailure(Store.BeginTurn(
            Scope, AgentTurnId{.Value = "turn-1"})) ==
        SessionFailureCode::InvalidScope);
  CHECK(SnapshotFailure(Store.GetSnapshot(Scope)) ==
        SessionFailureCode::InvalidScope);

  CHECK(MutationFailure(Store.AppendMessage(
            MakeScope(),
            MakeMessage({}, "dedup-2", "text", {}))) ==
        SessionFailureCode::InvalidMessage);
  CHECK(MutationFailure(Store.AppendMessage(
            MakeScope(),
            MakeMessage("message-3", {}, "text", {}))) ==
        SessionFailureCode::InvalidMessage);
}

void TestExactScopeIsolation() {
  ConversationSessionState Store;
  ConversationScope First = MakeScope();
  CHECK(IsSuccess(Store.AppendMessage(
      First, MakeMessage("shared-id", "shared-dedup", "first", {}))));

  std::vector<ConversationScope> Others;
  ConversationScope Other = MakeScope();
  Other.Principal.Value = "principal-02";
  Others.push_back(Other);
  Other = MakeScope();
  Other.Channel.Value = "qq-other";
  Others.push_back(Other);
  Other = MakeScope();
  Other.Account.Value = "account-02";
  Others.push_back(Other);
  Other = MakeScope();
  Other.Mode.Value = "unofficial";
  Others.push_back(Other);
  Other = MakeScope();
  Other.Conversation.Value = "conversation-02";
  Others.push_back(Other);
  Other = MakeScope();
  Other.ConversationType = ConversationKind::Group;
  Others.push_back(Other);
  Other = MakeScope();
  Other.Sender.Value = "sender-02";
  Others.push_back(Other);

  for (const ConversationScope& Isolated : Others) {
    CHECK(SnapshotFailure(Store.GetSnapshot(Isolated)) ==
          SessionFailureCode::UnknownScope);
    CHECK(IsSuccess(Store.AppendMessage(
        Isolated, MakeMessage("shared-id", "shared-dedup", "other", {}))));
  }

  CHECK(Snapshot(Store.GetSnapshot(First)).Messages.size() == 1);
  for (const ConversationScope& Isolated : Others) {
    const ConversationSessionSnapshot Value =
        Snapshot(Store.GetSnapshot(Isolated));
    CHECK(Value.Messages.size() == 1);
    CHECK(std::get<TextPart>(Value.Messages[0].Content.MessageParts[0])
              .Utf8Text == "other");
  }
}

void TestTurnLifecycleAndOrder() {
  ConversationSessionState Store;
  const ConversationScope Scope = MakeScope();
  CHECK(IsSuccess(
      Store.BeginTurn(Scope, AgentTurnId{.Value = "turn-1"})));
  CHECK(IsSuccess(Store.EndTurn(
      Scope, AgentTurnId{.Value = "turn-1"},
      SessionTurnState::Completed)));
  CHECK(IsSuccess(
      Store.BeginTurn(Scope, AgentTurnId{.Value = "turn-2"})));
  CHECK(IsSuccess(Store.EndTurn(
      Scope, AgentTurnId{.Value = "turn-2"},
      SessionTurnState::Cancelled)));
  CHECK(IsSuccess(
      Store.BeginTurn(Scope, AgentTurnId{.Value = "turn-3"})));
  CHECK(IsSuccess(Store.EndTurn(
      Scope, AgentTurnId{.Value = "turn-3"},
      SessionTurnState::UnknownOutcome)));

  const ConversationSessionSnapshot Value =
      Snapshot(Store.GetSnapshot(Scope));
  CHECK(Value.Turns.size() == 3);
  CHECK(Value.Turns[0].AgentTurn.Value == "turn-1");
  CHECK(Value.Turns[0].State == SessionTurnState::Completed);
  CHECK(Value.Turns[1].AgentTurn.Value == "turn-2");
  CHECK(Value.Turns[1].State == SessionTurnState::Cancelled);
  CHECK(Value.Turns[2].AgentTurn.Value == "turn-3");
  CHECK(Value.Turns[2].State == SessionTurnState::UnknownOutcome);
}

void TestInvalidTurnTransitionsFailClosed() {
  ConversationSessionState Store;
  const ConversationScope Scope = MakeScope();
  CHECK(MutationFailure(Store.BeginTurn(
            Scope, AgentTurnId{.Value = {}})) ==
        SessionFailureCode::InvalidTurn);
  CHECK(MutationFailure(Store.EndTurn(
            Scope, AgentTurnId{.Value = "missing"},
            SessionTurnState::Completed)) ==
        SessionFailureCode::UnknownScope);

  CHECK(IsSuccess(
      Store.BeginTurn(Scope, AgentTurnId{.Value = "turn-1"})));
  CHECK(MutationFailure(Store.BeginTurn(
            Scope, AgentTurnId{.Value = "turn-1"})) ==
        SessionFailureCode::DuplicateTurn);
  CHECK(MutationFailure(Store.BeginTurn(
            Scope, AgentTurnId{.Value = "turn-2"})) ==
        SessionFailureCode::ActiveTurnExists);
  CHECK(MutationFailure(Store.EndTurn(
            Scope, AgentTurnId{.Value = "turn-1"},
            SessionTurnState::Active)) ==
        SessionFailureCode::InvalidTurnTransition);
  CHECK(MutationFailure(Store.EndTurn(
            Scope, AgentTurnId{.Value = "missing"},
            SessionTurnState::Completed)) ==
        SessionFailureCode::UnknownTurn);
  CHECK(IsSuccess(Store.EndTurn(
      Scope, AgentTurnId{.Value = "turn-1"},
      SessionTurnState::Completed)));
  CHECK(MutationFailure(Store.EndTurn(
            Scope, AgentTurnId{.Value = "turn-1"},
            SessionTurnState::Cancelled)) ==
        SessionFailureCode::InvalidTurnTransition);
  CHECK(MutationFailure(Store.BeginTurn(
            Scope, AgentTurnId{.Value = "turn-1"})) ==
        SessionFailureCode::DuplicateTurn);
}

void TestUnknownScopeAndInvalidSnapshotScope() {
  ConversationSessionState Store;
  CHECK(SnapshotFailure(Store.GetSnapshot(MakeScope())) ==
        SessionFailureCode::UnknownScope);
  ConversationScope Invalid = MakeScope();
  Invalid.Sender.Value.clear();
  CHECK(SnapshotFailure(Store.GetSnapshot(Invalid)) ==
        SessionFailureCode::InvalidScope);
  CHECK(MutationFailure(Store.EndTurn(
            Invalid, AgentTurnId{.Value = "turn"},
            SessionTurnState::Completed)) ==
        SessionFailureCode::InvalidScope);
}

void TestMessagesAndTurnsRemainAssociated() {
  ConversationSessionState Store;
  const ConversationScope Scope = MakeScope();
  CHECK(IsSuccess(Store.AppendMessage(
      Scope, MakeMessage("message-1", "dedup-1", "question", {}))));
  CHECK(IsSuccess(
      Store.BeginTurn(Scope, AgentTurnId{.Value = "turn-1"})));
  CHECK(IsSuccess(Store.AppendMessage(
      Scope, MakeMessage("message-2", "dedup-2", "follow-up", {}))));
  CHECK(IsSuccess(Store.EndTurn(
      Scope, AgentTurnId{.Value = "turn-1"},
      SessionTurnState::Completed)));

  const ConversationSessionSnapshot Value =
      Snapshot(Store.GetSnapshot(Scope));
  CHECK(Value.Scope.Principal.Value == "principal-01");
  CHECK(Value.Scope.Channel.Value == "qq");
  CHECK(Value.Scope.Account.Value == "account-01");
  CHECK(Value.Scope.Conversation.Value == "conversation-01");
  CHECK(Value.Scope.Sender.Value == "sender-01");
  CHECK(Value.Messages.size() == 2);
  CHECK(Value.Turns.size() == 1);
}

} // namespace

int main() {
  const std::vector<std::pair<std::string_view, void (*)()>> Tests{
      {"message order", TestMessageOrderAndSequenceAreSeparateFromTimestamps},
      {"duplicate message", TestDuplicateMessageAndDeduplicationFailClosed},
      {"invalid input", TestInvalidScopeAndMessageAreRejected},
      {"scope isolation", TestExactScopeIsolation},
      {"turn lifecycle", TestTurnLifecycleAndOrder},
      {"invalid turn transitions", TestInvalidTurnTransitionsFailClosed},
      {"unknown scope", TestUnknownScopeAndInvalidSnapshotScope},
      {"association", TestMessagesAndTurnsRemainAssociated},
  };

  int Failures = 0;
  for (const auto& [Name, Test] : Tests) {
    try {
      Test();
      std::cout << "PASS " << Name << '\n';
    } catch (const std::exception& Error) {
      ++Failures;
      std::cerr << "FAIL " << Name << ": " << Error.what() << '\n';
    }
  }
  std::cout << "ConversationSessionStateTests "
            << (Tests.size() - static_cast<std::size_t>(Failures)) << "/"
            << Tests.size() << " passed\n";
  return Failures == 0 ? 0 : 1;
}
