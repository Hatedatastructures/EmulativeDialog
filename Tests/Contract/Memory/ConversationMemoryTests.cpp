#include "Source/Memory/ConversationMemory.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace EmulativeDialog::Memory;

int Failures = 0;

void Check(bool Condition, std::string_view Name) {
  if (Condition) {
    std::cout << "PASS " << Name << '\n';
  } else {
    ++Failures;
    std::cerr << "FAIL " << Name << '\n';
  }
}

std::chrono::sys_seconds Now() {
  return std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}};
}

std::string ScopeKey(const StoredMemory& Record) {
  return Record.ChannelId + "|" + Record.AccountId + "|" +
         Record.ConversationId + "|" +
         std::to_string(static_cast<int>(Record.ScopeKind)) + "|" +
         Record.PrincipalId;
}

class FakeMemoryStorage final : public MemoryStoragePort {
public:
  std::size_t ApplyCalls{0};
  bool FailApply{false};
  bool FailList{false};
  bool FailBackup{false};
  std::vector<StoredMemory> BackupRecords;
  std::map<std::string, std::vector<StoredMemory>> Rows;

  MemoryStorageResult<std::monostate> ApplyAtomically(
      const MemoryMutationBatch& Batch) override {
    ++ApplyCalls;
    if (FailApply) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected batch failure"};
    }
    auto Staged = Rows;
    auto& ScopeRows = Staged[ScopeKey(Batch.Scope)];
    for (StoredMemory& Record : ScopeRows) {
      if (std::find(Batch.Tombstones.begin(), Batch.Tombstones.end(),
                    Record.MemoryId) != Batch.Tombstones.end()) {
        Record.Tombstoned = true;
        ++Record.Version;
      }
    }
    for (const StoredMemory& Upsert : Batch.Upserts) {
      bool Replaced = false;
      for (StoredMemory& Existing : ScopeRows) {
        if (Existing.MemoryId != Upsert.MemoryId) {
          continue;
        }
        if (Existing.Tombstoned) {
          return MemoryStorageError{
              .Code = MemoryStorageErrorCode::VersionConflict,
              .Detail = "tombstoned records cannot be resurrected"};
        }
        if (Upsert.Version <= Existing.Version) {
          return MemoryStorageError{
              .Code = MemoryStorageErrorCode::VersionConflict,
              .Detail = "injected version conflict"};
        }
        Existing = Upsert;
        Replaced = true;
        break;
      }
      if (!Replaced) {
        ScopeRows.push_back(Upsert);
      }
    }
    Rows = std::move(Staged);
    return std::monostate{};
  }

  MemoryStorageResult<std::vector<StoredMemory>> ListActive(
      const StoredMemory& ScopeTemplate,
      std::chrono::sys_seconds Current) const override {
    if (FailList) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected list failure"};
    }
    std::vector<StoredMemory> Active;
    const auto Scope = Rows.find(ScopeKey(ScopeTemplate));
    if (Scope == Rows.end()) {
      return Active;
    }
    for (const StoredMemory& Record : Scope->second) {
      if (!Record.Tombstoned &&
          (!Record.ExpiresAt.has_value() || *Record.ExpiresAt > Current)) {
        Active.push_back(Record);
      }
    }
    return Active;
  }

  MemoryStorageResult<std::vector<StoredMemory>> RecallBackup(
      const StoredMemory& ScopeTemplate,
      std::chrono::sys_seconds Current) const override {
    if (FailBackup) {
      return MemoryStorageError{.Code = MemoryStorageErrorCode::Failure,
                                .Detail = "injected backup failure"};
    }
    std::vector<StoredMemory> Result;
    for (const StoredMemory& Record : BackupRecords) {
      if (ScopeKey(Record) == ScopeKey(ScopeTemplate) &&
          (!Record.ExpiresAt.has_value() || *Record.ExpiresAt > Current)) {
        Result.push_back(Record);
      }
    }
    return Result;
  }
};

AuthorizedMemoryContext Context(
    ConversationScopeKind Kind = ConversationScopeKind::Direct,
    std::string Principal = "principal-01",
    std::string Conversation = "conversation-01",
    std::uint64_t PolicyVersion = 1) {
  AuthorizedMemoryContext Value;
  Value.ChannelId = "qq";
  Value.AccountId = "account-01";
  Value.ConversationId = std::move(Conversation);
  Value.PrincipalId = std::move(Principal);
  Value.ScopeKind = Kind;
  Value.IdentityPolicyVersion = PolicyVersion;
  Value.IdentityVerified = true;
  return Value;
}

ConsentGrant Grant(MemoryCategory Category,
                   const AuthorizedMemoryContext& Value,
                   std::string Id = "consent-01") {
  ConsentGrant Grant;
  Grant.ConsentId = std::move(Id);
  Grant.GrantorPrincipalId = Value.PrincipalId;
  Grant.ChannelId = Value.ChannelId;
  Grant.AccountId = Value.AccountId;
  Grant.ConversationId = Value.ConversationId;
  Grant.ScopeKind = Value.ScopeKind;
  Grant.Category = Category;
  Grant.SourceMessageId = "fixture-message-consent";
  Grant.GrantedAt = Now();
  Grant.IdentityPolicyVersion = Value.IdentityPolicyVersion;
  if (Category == MemoryCategory::GroupMemory) {
    Grant.AllowedGroupMembers = {Value.PrincipalId};
  }
  return Grant;
}

MemoryWriteRequest Request(std::string Id, MemoryCategory Category,
                           std::string ConsentId = "consent-01") {
  MemoryWriteRequest Request;
  Request.MemoryId = std::move(Id);
  Request.Category = Category;
  Request.SubjectId = "principal-01";
  Request.Content = "Synthetic memory";
  Request.SourceReference = "fixture-message-01";
  Request.ConsentId = std::move(ConsentId);
  Request.CreatedAt = Now() - std::chrono::seconds{100};
  Request.UserConfirmed = true;
  Request.Version = 1;
  return Request;
}

template <typename Value>
const MemoryError* AsError(const MemoryResult<Value>& Result) {
  return std::get_if<MemoryError>(&Result);
}

std::size_t ActiveCount(const FakeMemoryStorage& Storage,
                        const StoredMemory& Scope) {
  const auto It = Storage.Rows.find(ScopeKey(Scope));
  if (It == Storage.Rows.end()) {
    return 0;
  }
  return static_cast<std::size_t>(
      std::count_if(It->second.begin(), It->second.end(),
                    [](const StoredMemory& Record) {
                      return !Record.Tombstoned;
                    }));
}

StoredMemory ScopeOf(const AuthorizedMemoryContext& Value) {
  StoredMemory Scope;
  Scope.ChannelId = Value.ChannelId;
  Scope.AccountId = Value.AccountId;
  Scope.ConversationId = Value.ConversationId;
  Scope.ScopeKind = Value.ScopeKind;
  Scope.PrincipalId = Value.PrincipalId;
  return Scope;
}

std::string ReadFixture(std::string_view Path) {
  std::ifstream Input(std::string(Path), std::ios::binary);
  if (!Input) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(Input),
                     std::istreambuf_iterator<char>());
}

std::optional<std::string> JsonStringField(std::string_view Object,
                                           std::string_view Key) {
  std::string Needle;
  Needle.reserve(Key.size() + 2U);
  Needle.push_back('"');
  Needle.append(Key);
  Needle.push_back('"');
  const std::size_t KeyPosition = Object.find(Needle);
  if (KeyPosition == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t Position = Object.find(':', KeyPosition + Needle.size());
  if (Position == std::string_view::npos) {
    return std::nullopt;
  }
  ++Position;
  while (Position < Object.size() &&
         (Object[Position] == ' ' || Object[Position] == '\t' ||
          Object[Position] == '\r' || Object[Position] == '\n')) {
    ++Position;
  }
  if (Position >= Object.size() || Object[Position] != '"') {
    return std::nullopt;
  }
  ++Position;
  std::string Value;
  bool Escaped = false;
  for (; Position < Object.size(); ++Position) {
    const char Character = Object[Position];
    if (Escaped) {
      Value.push_back(Character);
      Escaped = false;
    } else if (Character == '\\') {
      Escaped = true;
    } else if (Character == '"') {
      return Value;
    } else {
      Value.push_back(Character);
    }
  }
  return std::nullopt;
}

std::string_view FindJsonObjectWithStringField(std::string_view Json,
                                               std::string_view Key,
                                               std::string_view Expected) {
  for (std::size_t Start = 0; Start < Json.size(); ++Start) {
    if (Json[Start] != '{') {
      continue;
    }
    std::size_t Depth = 0;
    bool InString = false;
    bool Escaped = false;
    for (std::size_t Position = Start; Position < Json.size(); ++Position) {
      const char Character = Json[Position];
      if (InString) {
        if (Escaped) {
          Escaped = false;
        } else if (Character == '\\') {
          Escaped = true;
        } else if (Character == '"') {
          InString = false;
        }
        continue;
      }
      if (Character == '"') {
        InString = true;
      } else if (Character == '{') {
        ++Depth;
      } else if (Character == '}') {
        if (Depth == 0U) {
          break;
        }
        --Depth;
        if (Depth == 0U) {
          const auto Object = Json.substr(Start, Position - Start + 1U);
          const auto Value = JsonStringField(Object, Key);
          if (Value.has_value() && *Value == Expected) {
            return Object;
          }
          break;
        }
      }
    }
  }
  return {};
}

std::vector<std::string> JsonStringArrayField(std::string_view Object,
                                              std::string_view Key) {
  std::string Needle;
  Needle.reserve(Key.size() + 2U);
  Needle.push_back('"');
  Needle.append(Key);
  Needle.push_back('"');
  const std::size_t KeyPosition = Object.find(Needle);
  if (KeyPosition == std::string_view::npos) {
    return {};
  }
  const std::size_t Colon = Object.find(':', KeyPosition + Needle.size());
  const std::size_t ArrayStart =
      Colon == std::string_view::npos ? Colon : Object.find('[', Colon + 1U);
  if (ArrayStart == std::string_view::npos) {
    return {};
  }
  std::vector<std::string> Values;
  std::size_t Position = ArrayStart + 1U;
  while (Position < Object.size()) {
    while (Position < Object.size() &&
           (Object[Position] == ' ' || Object[Position] == '\t' ||
            Object[Position] == '\r' || Object[Position] == '\n' ||
            Object[Position] == ',')) {
      ++Position;
    }
    if (Position >= Object.size() || Object[Position] == ']') {
      break;
    }
    if (Object[Position] != '"') {
      return {};
    }
    ++Position;
    std::string Value;
    bool Escaped = false;
    for (; Position < Object.size(); ++Position) {
      const char Character = Object[Position];
      if (Escaped) {
        Value.push_back(Character);
        Escaped = false;
      } else if (Character == '\\') {
        Escaped = true;
      } else if (Character == '"') {
        ++Position;
        break;
      } else {
        Value.push_back(Character);
      }
    }
    Values.push_back(std::move(Value));
  }
  return Values;
}

std::optional<std::uint64_t> JsonUnsignedField(std::string_view Object,
                                               std::string_view Key) {
  std::string Needle;
  Needle.reserve(Key.size() + 2U);
  Needle.push_back('"');
  Needle.append(Key);
  Needle.push_back('"');
  const std::size_t KeyPosition = Object.find(Needle);
  if (KeyPosition == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t Position = Object.find(':', KeyPosition + Needle.size());
  if (Position == std::string_view::npos) {
    return std::nullopt;
  }
  ++Position;
  while (Position < Object.size() &&
         (Object[Position] == ' ' || Object[Position] == '\t' ||
          Object[Position] == '\r' || Object[Position] == '\n')) {
    ++Position;
  }
  std::uint64_t Value = 0;
  const char* Begin = Object.data() + Position;
  const char* End = Object.data() + Object.size();
  const auto [ParsedEnd, Error] = std::from_chars(Begin, End, Value);
  if (Error != std::errc{} || ParsedEnd == Begin) {
    return std::nullopt;
  }
  return Value;
}

void TestIdentitySessionAndPolicy() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  auto Session = Context(ConversationScopeKind::Session);
  Check(AsError(Memory.Write(
            Session, Request("memory-1", MemoryCategory::IdentityPreference),
            Now()))
            ->Code == MemoryErrorCode::SessionScopeVolatile,
        "SessionScope write rejected");
  Check(AsError(Memory.Recall(Session, Now(), 100))->Code ==
            MemoryErrorCode::SessionScopeVolatile,
        "SessionScope recall rejected");
  auto Unverified = Context();
  Unverified.IdentityVerified = false;
  Check(AsError(Memory.Recall(Unverified, Now(), 100))->Code ==
            MemoryErrorCode::UnauthorizedIdentity,
        "unverified identity rejected");
  auto MismatchedPolicy = Context(ConversationScopeKind::Direct,
                                  "principal-01", "conversation-01", 2);
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Context())));
  Check(AsError(Memory.Write(
            MismatchedPolicy,
            Request("memory-2", MemoryCategory::IdentityPreference), Now()))
            ->Code == MemoryErrorCode::ConsentMismatch,
        "identity policy version change invalidates consent");
}

void TestConsentValidationAndProposalBinding() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context();
  auto Invalid = Grant(MemoryCategory::IdentityPreference, Value);
  Invalid.SourceMessageId.clear();
  Check(AsError(Memory.RegisterConsent(Invalid))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires source message");
  auto ProposalGrant = Grant(MemoryCategory::IdentityPreference, Value);
  ProposalGrant.ProposalId = "proposal-a";
  Check(std::holds_alternative<std::monostate>(
            Memory.RegisterConsent(ProposalGrant)),
        "proposal-bound consent registered");
  auto WrongProposal =
      Request("memory-1", MemoryCategory::IdentityPreference);
  WrongProposal.ProposalId = "proposal-b";
  Check(AsError(Memory.Write(Value, WrongProposal, Now()))->Code ==
            MemoryErrorCode::ConsentMismatch,
        "consent A cannot authorize proposal B");
  auto MissingProposal =
      Request("memory-1", MemoryCategory::IdentityPreference);
  Check(AsError(Memory.Write(Value, MissingProposal, Now()))->Code ==
            MemoryErrorCode::ConsentMismatch,
        "proposal-bound consent requires matching proposal");
  auto CorrectProposal =
      Request("memory-1", MemoryCategory::IdentityPreference);
  CorrectProposal.ProposalId = "proposal-a";
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Value, CorrectProposal, Now())),
        "matching proposal writes");
  Check(AsError(Memory.RegisterConsent(ProposalGrant))->Code ==
            MemoryErrorCode::InvalidArgument,
        "duplicate consent rejected");
}

void TestOptionalProposalConsentRequiresExactMatch() {
  const auto Direct = Context();
  auto MakeRecord = [](const AuthorizedMemoryContext& Scope,
                       std::string MemoryId, std::string ConsentId,
                       std::optional<std::string> ProposalId) {
    StoredMemory Record = ScopeOf(Scope);
    Record.MemoryId = std::move(MemoryId);
    Record.Category = Scope.ScopeKind == ConversationScopeKind::Group
                          ? MemoryCategory::GroupMemory
                          : MemoryCategory::IdentityPreference;
    Record.SubjectId = Scope.PrincipalId;
    Record.Content = "Synthetic proposal memory";
    Record.SourceReference = "fixture-message-01";
    Record.ConsentId = std::move(ConsentId);
    Record.ProposalId = std::move(ProposalId);
    Record.CreatedAt = Now() - std::chrono::seconds{100};
    Record.Version = 1;
    return Record;
  };

  {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    constexpr std::string_view ConsentId = "consent-unbound-write";
    Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(
              Grant(MemoryCategory::IdentityPreference, Direct,
                    std::string(ConsentId)))),
          "unbound consent registers for proposal matching test");
    auto BoundRequest =
        Request("memory-unbound-write", MemoryCategory::IdentityPreference,
                std::string(ConsentId));
    BoundRequest.ProposalId = "proposal-a";
    const std::size_t WritesBefore = Storage.ApplyCalls;
    const auto Rejected = Memory.Write(Direct, BoundRequest, Now());
    const auto* RejectedError = AsError(Rejected);
    Check(RejectedError != nullptr &&
              RejectedError->Code == MemoryErrorCode::ConsentMismatch,
          "unbound consent cannot write a proposal-bound request");
    Check(Storage.ApplyCalls == WritesBefore,
          "proposal mismatch is rejected before storage mutation");
    Check(Storage.Rows.empty(),
          "proposal mismatch leaves fake storage unchanged");

    const auto NullNullWrite = Memory.Write(
        Direct,
        Request("memory-null-null", MemoryCategory::IdentityPreference,
                std::string(ConsentId)),
        Now());
    Check(std::holds_alternative<std::monostate>(NullNullWrite),
          "null consent proposal matches null request proposal");
  }

  {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    constexpr std::string_view ConsentId = "consent-unbound-recall";
    static_cast<void>(Memory.RegisterConsent(
        Grant(MemoryCategory::IdentityPreference, Direct,
              std::string(ConsentId))));
    auto BoundRecord = MakeRecord(Direct, "memory-bound-recall",
                                  std::string(ConsentId), "proposal-a");
    Storage.Rows[ScopeKey(BoundRecord)].push_back(BoundRecord);

    const auto Recalled = Memory.Recall(Direct, Now(), 1000);
    const auto* Items = std::get_if<
        std::vector<EmulativeDialog::Context::ContextCandidate>>(&Recalled);
    Check(Items != nullptr && Items->empty(),
          "unbound consent cannot recall a proposal-bound record");
  }

  {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    constexpr std::string_view ConsentId = "consent-bound-recall";
    auto BoundGrant =
        Grant(MemoryCategory::IdentityPreference, Direct,
              std::string(ConsentId));
    BoundGrant.ProposalId = "proposal-a";
    static_cast<void>(Memory.RegisterConsent(BoundGrant));
    auto BoundRecord = MakeRecord(Direct, "memory-exact-recall",
                                  std::string(ConsentId), "proposal-a");
    Storage.Rows[ScopeKey(BoundRecord)].push_back(BoundRecord);

    const auto Recalled = Memory.Recall(Direct, Now(), 1000);
    const auto* Items = std::get_if<
        std::vector<EmulativeDialog::Context::ContextCandidate>>(&Recalled);
    Check(Items != nullptr && Items->size() == 1U &&
              Items->front().Id.Value == "memory-exact-recall",
          "exact proposal-bound consent can recall its matching record");
  }

  {
    const auto GroupOwner = Context(ConversationScopeKind::Group,
                                    "principal-02", "group-01");
    const auto GroupReader = Context(ConversationScopeKind::Group,
                                     "principal-01", "group-01");
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    constexpr std::string_view ConsentId = "consent-unbound-group";
    auto UnboundGrant = Grant(MemoryCategory::GroupMemory, GroupOwner,
                              std::string(ConsentId));
    UnboundGrant.AllowedGroupMembers = {GroupReader.PrincipalId};
    static_cast<void>(Memory.RegisterConsent(UnboundGrant));
    auto BoundRecord = MakeRecord(GroupOwner, "memory-bound-group",
                                  std::string(ConsentId), "proposal-a");
    Storage.Rows[ScopeKey(BoundRecord)].push_back(BoundRecord);

    const auto Recalled = Memory.Recall(GroupReader, Now(), 1000);
    const auto* Items = std::get_if<
        std::vector<EmulativeDialog::Context::ContextCandidate>>(&Recalled);
    Check(Items != nullptr && Items->empty(),
          "unbound group consent cannot expose a proposal-bound record");
  }

  {
    const auto GroupOwner = Context(ConversationScopeKind::Group,
                                    "principal-02", "group-01");
    const auto GroupReader = Context(ConversationScopeKind::Group,
                                     "principal-01", "group-01");
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    constexpr std::string_view ConsentId = "consent-bound-group";
    auto BoundGrant = Grant(MemoryCategory::GroupMemory, GroupOwner,
                            std::string(ConsentId));
    BoundGrant.AllowedGroupMembers = {GroupReader.PrincipalId};
    BoundGrant.ProposalId = "proposal-a";
    static_cast<void>(Memory.RegisterConsent(BoundGrant));
    auto BoundRecord = MakeRecord(GroupOwner, "memory-exact-group",
                                  std::string(ConsentId), "proposal-a");
    Storage.Rows[ScopeKey(BoundRecord)].push_back(BoundRecord);

    const auto Recalled = Memory.Recall(GroupReader, Now(), 1000);
    const auto* Items = std::get_if<
        std::vector<EmulativeDialog::Context::ContextCandidate>>(&Recalled);
    Check(Items != nullptr && Items->size() == 1U &&
              Items->front().Id.Value == "memory-exact-group",
          "exact proposal-bound group consent can share its matching record");
  }

  {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    constexpr std::string_view ConsentId = "consent-unbound-restore";
    static_cast<void>(Memory.RegisterConsent(
        Grant(MemoryCategory::IdentityPreference, Direct,
              std::string(ConsentId))));
    Storage.BackupRecords = {MakeRecord(Direct, "memory-bound-restore",
                                        std::string(ConsentId), "proposal-a")};

    const auto Restored = Memory.RestoreBackup(Direct, Now());
    const auto* RestoredCount = std::get_if<std::size_t>(&Restored);
    Check(RestoredCount != nullptr && *RestoredCount == 0U,
          "unbound consent cannot restore a proposal-bound record");
    Check(Storage.ApplyCalls == 0U && Storage.Rows.empty(),
          "proposal mismatch cannot mutate storage during restore");
  }

  {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    constexpr std::string_view ConsentId = "consent-bound-restore";
    auto BoundGrant =
        Grant(MemoryCategory::IdentityPreference, Direct,
              std::string(ConsentId));
    BoundGrant.ProposalId = "proposal-a";
    static_cast<void>(Memory.RegisterConsent(BoundGrant));
    Storage.BackupRecords = {MakeRecord(Direct, "memory-exact-restore",
                                        std::string(ConsentId), "proposal-a")};

    const auto Restored = Memory.RestoreBackup(Direct, Now());
    const auto* RestoredCount = std::get_if<std::size_t>(&Restored);
    Check(RestoredCount != nullptr && *RestoredCount == 1U,
          "exact proposal-bound consent can restore its matching record");
    const auto Rows = Storage.Rows.find(ScopeKey(ScopeOf(Direct)));
    Check(Rows != Storage.Rows.end() && Rows->second.size() == 1U &&
              Rows->second.front().ProposalId ==
                  std::optional<std::string>{"proposal-a"},
          "restored record retains the exact proposal binding");
  }
}

void TestScopeKindAndGroupMembership() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Direct = Context(ConversationScopeKind::Direct);
  const auto Group = Context(ConversationScopeKind::Group);
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Direct, "consent-direct")));
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::GroupMemory, Group, "consent-group")));
  static_cast<void>(Memory.Write(
      Direct,
      Request("memory-direct", MemoryCategory::IdentityPreference,
              "consent-direct"),
      Now()));
  static_cast<void>(Memory.Write(
      Group,
      Request("memory-group", MemoryCategory::GroupMemory, "consent-group"),
      Now()));
  const auto DirectRecall = Memory.Recall(Direct, Now(), 1000);
  const auto GroupRecall = Memory.Recall(Group, Now(), 1000);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            DirectRecall)
            .size() == 1,
        "direct scope reads only direct records");
  const auto& GroupCandidates =
      std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
          GroupRecall);
  Check(GroupCandidates.size() == 1 &&
            GroupCandidates.front().Scope.Kind ==
                EmulativeDialog::Context::ConversationKind::Group,
        "group recall keeps group scope kind");
  auto ExcludedMember =
      Context(ConversationScopeKind::Group, "principal-03");
  ExcludedMember.ConversationId = Group.ConversationId;
  auto ExcludedConsent = Grant(MemoryCategory::GroupMemory, Group,
                               "consent-group-excluded");
  ExcludedConsent.GrantorPrincipalId = "principal-02";
  ExcludedConsent.AllowedGroupMembers = {"principal-01"};
  Check(AsError(Memory.RegisterConsent(ExcludedConsent)) == nullptr,
        "group consent registration accepts member list");
  Check(AsError(Memory.Write(
            ExcludedMember,
            Request("memory-excluded", MemoryCategory::GroupMemory,
                    "consent-group-excluded"),
            Now()))
            ->Code == MemoryErrorCode::ConsentMismatch,
        "group consent denies unlisted member");
}

void TestFixtureGroupConsentAllowsListedMemberRecall() {
  const std::string Fixture =
      ReadFixture("Tests/Contract/Memory/Fixtures/ScopeConsentCases.json");
  const auto GroupCase =
      FindJsonObjectWithStringField(Fixture, "CaseId", "SC-008");
  Check(!GroupCase.empty(), "SC-008 group consent fixture loaded");
  if (GroupCase.empty()) {
    return;
  }

  const auto ConsentObject = FindJsonObjectWithStringField(
      GroupCase, "ConsentId", "fixture-consent-group-01");
  const auto ConsentId = JsonStringField(ConsentObject, "ConsentId");
  const auto ChannelId = JsonStringField(ConsentObject, "ChannelId");
  const auto AccountId = JsonStringField(ConsentObject, "AccountId");
  const auto GroupId = JsonStringField(ConsentObject, "GroupConversationId");
  const auto RecordKinds =
      JsonStringArrayField(ConsentObject, "AllowedRecordKinds");
  const auto AllowedPrincipalIds =
      JsonStringArrayField(ConsentObject, "AllowedPrincipalIds");
  Check(ConsentId.has_value() && ChannelId.has_value() &&
            AccountId.has_value() && GroupId.has_value() &&
            RecordKinds == std::vector<std::string>{"Preference"} &&
            AllowedPrincipalIds.size() == 2U,
        "SC-008 consent shape parsed");
  if (!ConsentId.has_value() || !ChannelId.has_value() ||
      !AccountId.has_value() || !GroupId.has_value() ||
      RecordKinds != std::vector<std::string>{"Preference"} ||
      AllowedPrincipalIds.size() != 2U) {
    return;
  }

  const auto MemberAttempt = FindJsonObjectWithStringField(
      GroupCase, "AttemptId", "fixture-group-read-authorized-principal");
  Check(!MemberAttempt.empty(),
        "SC-008 authorized read attempt parsed");

  const std::string OwnerPrincipal = AllowedPrincipalIds.front();
  AuthorizedMemoryContext Owner =
      Context(ConversationScopeKind::Group, OwnerPrincipal, *GroupId);
  Owner.ChannelId = *ChannelId;
  Owner.AccountId = *AccountId;
  const std::string MemberPrincipal =
      JsonStringField(MemberAttempt, "PrincipalId").value_or("");
  const auto ExpectedMemberRead = JsonStringField(MemberAttempt, "Expected");
  Check(MemberPrincipal == AllowedPrincipalIds[1] &&
            ExpectedMemberRead == std::optional<std::string>{"Allow"},
        "SC-008 member read fields parsed from fixture");

  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  ConsentGrant GroupConsent = Grant(MemoryCategory::IdentityPreference, Owner,
                                    *ConsentId);
  GroupConsent.AllowedGroupMembers = AllowedPrincipalIds;
  GroupConsent.SourceMessageId = "fixture-group-consent-source";
  Check(std::holds_alternative<std::monostate>(
            Memory.RegisterConsent(GroupConsent)),
        "SC-008 group consent registered");
  auto SharedRequest = Request("fixture-memory-group-preference",
                               MemoryCategory::IdentityPreference,
                               *ConsentId);
  SharedRequest.SubjectId = OwnerPrincipal;
  SharedRequest.Content = "Fixture-authorized group preference";
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Owner, SharedRequest, Now())),
        "SC-008 grantor writes group-scoped preference");
  StoredMemory UnlistedKind = ScopeOf(Owner);
  UnlistedKind.MemoryId = "fixture-memory-unlisted-active-task";
  UnlistedKind.Category = MemoryCategory::ActiveConversationContext;
  UnlistedKind.SubjectId = OwnerPrincipal;
  UnlistedKind.Content = "Not authorized by the preference grant";
  UnlistedKind.SourceReference = "fixture-group-source-unlisted-kind";
  UnlistedKind.ConsentId = *ConsentId;
  UnlistedKind.CreatedAt = Now();
  UnlistedKind.Version = 1;
  Storage.Rows[ScopeKey(ScopeOf(Owner))].push_back(UnlistedKind);

  // The fixture's "Preference" record kind maps to IdentityPreference, while
  // Group scope and the explicit principal list provide the shared-read grant.
  const std::string AttemptIds[] = {
      "fixture-group-read-authorized-principal",
      "fixture-group-read-unlisted-principal",
      "fixture-group-read-other-account",
      "fixture-group-read-other-group",
      "fixture-group-read-unlisted-category"};
  for (const std::string& AttemptId : AttemptIds) {
    const auto Attempt =
        FindJsonObjectWithStringField(GroupCase, "AttemptId", AttemptId);
    Check(!Attempt.empty(), "SC-008 read attempt found in fixture");
    if (Attempt.empty()) {
      continue;
    }
    auto Reader = Context(ConversationScopeKind::Group,
                          JsonStringField(Attempt, "PrincipalId").value_or(""),
                          JsonStringField(Attempt, "GroupConversationId")
                              .value_or(""));
    Reader.ChannelId =
        JsonStringField(Attempt, "ChannelId").value_or("");
    Reader.AccountId =
        JsonStringField(Attempt, "AccountId").value_or("");
    const auto Expected = JsonStringField(Attempt, "Expected");
    const auto RecordKind = JsonStringField(Attempt, "RecordKind");
    const auto Recall = Memory.Recall(Reader, Now(), 1000);
    const auto* Candidates =
        std::get_if<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            &Recall);
    Check(Candidates != nullptr,
          "SC-008 read returns a candidate collection");
    if (Candidates == nullptr) {
      continue;
    }
    if (Expected == std::optional<std::string>{"Allow"}) {
      Check(Candidates->size() == 1U &&
                Candidates->front().Id.Value ==
                    "fixture-memory-group-preference" &&
                Candidates->front().Scope.PrincipalId == Reader.PrincipalId &&
                Candidates->front().Scope.Kind ==
                    EmulativeDialog::Context::ConversationKind::Group,
            "SC-008 allowed member reads owner record in request scope");
    } else if (RecordKind ==
               std::optional<std::string>{"ActiveChatTask"}) {
      Check(Candidates->size() == 1U &&
                std::none_of(Candidates->begin(), Candidates->end(),
                             [](const auto& Candidate) {
                               return Candidate.Kind ==
                                      EmulativeDialog::Context::ContextItemKind::
                                          ActiveChatTask;
                             }),
            "SC-008 unlisted record kind is not recalled");
    } else {
      Check(Candidates->empty(), "SC-008 denied scope/member reads nothing");
    }
  }
}

void TestRevocationAtomicityAndScope() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context();
  auto OtherConversation =
      Context(ConversationScopeKind::Direct, "principal-01",
              "conversation-02");
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-1", MemoryCategory::IdentityPreference), Now()));
  Check(AsError(Memory.RevokeConsent(OtherConversation, "consent-01", Now()))
            ->Code == MemoryErrorCode::UnauthorizedIdentity,
        "revoke requires exact conversation scope");
  Storage.FailApply = true;
  Check(AsError(Memory.RevokeConsent(Value, "consent-01", Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "revoke batch failure reported");
  Check(Memory.IsConsentActive("consent-01"),
        "consent remains active after failed tombstone batch");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 1,
        "record remains active after failed revoke");
  Storage.FailApply = false;
  Check(std::holds_alternative<std::monostate>(
            Memory.RevokeConsent(Value, "consent-01", Now())),
        "exact scope revoke succeeds");
  Check(!Memory.IsConsentActive("consent-01"), "consent revoked after batch");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 0,
        "affected records tombstoned");
}

void TestForgetAndExpiry() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context();
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::GroupMemory, Value, "consent-group")));
  static_cast<void>(Memory.Write(
      Value, Request("memory-1", MemoryCategory::IdentityPreference), Now()));
  static_cast<void>(Memory.Write(
      Value, Request("memory-2", MemoryCategory::GroupMemory, "consent-group"),
      Now()));
  ForgetRequest None;
  Check(AsError(Memory.Forget(Value, None, Now()))->Code ==
            MemoryErrorCode::ForgetScopeRequired,
        "broad forget rejected");
  ForgetRequest Missing;
  Missing.MemoryId = "memory-missing";
  Check(AsError(Memory.Forget(Value, Missing, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "unmatched forget rejected");
  ForgetRequest Category;
  Category.Category = MemoryCategory::GroupMemory;
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Value, Category, Now())),
        "category forget succeeds");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 1,
        "category forget tombstones matching record");
  auto Expired = Request("memory-3", MemoryCategory::IdentityPreference);
  Expired.ExpiresAt = Now() - std::chrono::seconds{1};
  Check(AsError(Memory.Write(Value, Expired, Now()))->Code ==
            MemoryErrorCode::InvalidArgument,
        "already-expired write rejected");
}

void TestCapacityAndWriteAtomicity() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage, 2);
  const auto Value = Context();
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  for (int Index = 0; Index < 2; ++Index) {
    auto Write =
        Request("memory-" + std::to_string(Index),
                MemoryCategory::IdentityPreference);
    Write.CreatedAt = Now() - std::chrono::seconds{100 - Index};
    static_cast<void>(Memory.Write(Value, Write, Now()));
  }
  Storage.FailApply = true;
  auto Third = Request("memory-2", MemoryCategory::IdentityPreference);
  Third.CreatedAt = Now();
  Check(AsError(Memory.Write(Value, Third, Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "capacity write batch failure reported");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 2,
        "failed write preserves eviction candidate");
  Storage.FailApply = false;
  static_cast<void>(Memory.Write(Value, Third, Now()));
  Check(ActiveCount(Storage, ScopeOf(Value)) == 2,
        "successful capacity write keeps bounded count");
  auto Conflict = Request("memory-0", MemoryCategory::IdentityPreference);
  Conflict.Version = 1;
  Check(AsError(Memory.Write(Value, Conflict, Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "version conflict maps to storage failure");
}

void TestDurableContentRequiresStrictUtf8() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context();
  const std::string OpaqueProposalId("\0\xFF\xC3", 3U);
  auto Consent = Grant(MemoryCategory::IdentityPreference, Value);
  Consent.ProposalId = OpaqueProposalId;
  Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(Consent)),
        "opaque binary ProposalId consent is accepted");

  const std::string ValidContent(
      "A\xC3\xB1\xE4\xB8\xAD\xF0\x9F\x8C\x8D", 10U);
  auto ValidRequest =
      Request("memory-valid-utf8", MemoryCategory::IdentityPreference);
  ValidRequest.Content = ValidContent;
  ValidRequest.ProposalId = OpaqueProposalId;
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Value, ValidRequest, Now())),
        "valid multibyte Content and opaque ProposalId are accepted");
  const auto ScopeRows = Storage.Rows.find(ScopeKey(ScopeOf(Value)));
  Check(ScopeRows != Storage.Rows.end() && ScopeRows->second.size() == 1U &&
            ScopeRows->second.front().Content == ValidContent &&
            ScopeRows->second.front().ProposalId == OpaqueProposalId,
        "valid Content and binary ProposalId persist byte-for-byte");

  const std::vector<std::string> InvalidContents{
      std::string("\0tail", 5U),
      std::string("\x80", 1U),
      std::string("\xC2", 1U),
      std::string("\xE2\x82", 2U),
      std::string("\xE2\x28\xA1", 3U),
      std::string("\xE2\x82\x28", 3U),
      std::string("\xC0\xAF", 2U),
      std::string("\xE0\x80\x80", 3U),
      std::string("\xED\xA0\x80", 3U),
      std::string("\xF0\x80\x80\x80", 4U),
      std::string("\xF0\x90\x80\x28", 4U),
      std::string("\xF4\x90\x80\x80", 4U),
      std::string("\xF5\x80\x80\x80", 4U),
  };
  const std::size_t ApplyCallsBeforeInvalid = Storage.ApplyCalls;
  for (std::size_t Index = 0; Index < InvalidContents.size(); ++Index) {
    auto InvalidRequest = Request(
        "memory-invalid-utf8-" + std::to_string(Index),
        MemoryCategory::IdentityPreference);
    InvalidRequest.Content = InvalidContents[Index];
    InvalidRequest.ProposalId = OpaqueProposalId;
    const auto Result = Memory.Write(Value, InvalidRequest, Now());
    const auto* Error = AsError(Result);
    Check(Error != nullptr && Error->Code == MemoryErrorCode::InvalidArgument,
          "invalid durable Content is rejected with InvalidArgument");
    if (Error != nullptr) {
      Check(Error->Detail.find(InvalidContents[Index]) == std::string::npos,
            "invalid Content is not echoed in the error detail");
    }
  }
  Check(Storage.ApplyCalls == ApplyCallsBeforeInvalid,
        "invalid Content never reaches storage mutation");

  auto InvalidUpdate = ValidRequest;
  InvalidUpdate.Content = std::string("replacement\0content", 19U);
  ++InvalidUpdate.Version;
  const std::size_t ApplyCallsBeforeInvalidUpdate = Storage.ApplyCalls;
  const auto InvalidUpdateResult = Memory.Write(Value, InvalidUpdate, Now());
  const auto* InvalidUpdateError = AsError(InvalidUpdateResult);
  Check(InvalidUpdateError != nullptr &&
            InvalidUpdateError->Code == MemoryErrorCode::InvalidArgument,
        "invalid Content update is rejected");
  Check(Storage.ApplyCalls == ApplyCallsBeforeInvalidUpdate,
        "invalid Content update never reaches storage mutation");
  const auto RowsAfterInvalidUpdate =
      Storage.Rows.find(ScopeKey(ScopeOf(Value)));
  Check(RowsAfterInvalidUpdate != Storage.Rows.end() &&
            RowsAfterInvalidUpdate->second.size() == 1U &&
            RowsAfterInvalidUpdate->second.front().MemoryId ==
                ValidRequest.MemoryId &&
            RowsAfterInvalidUpdate->second.front().Content == ValidContent &&
            RowsAfterInvalidUpdate->second.front().Version ==
                ValidRequest.Version,
        "invalid Content update preserves the existing record");
}

void TestRecallPriorityAndSummaryMapping() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Value = Context(ConversationScopeKind::Group);
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::ConversationSummary, Value, "consent-summary")));
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::ActiveConversationContext, Value,
            "consent-task")));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-summary", MemoryCategory::ConversationSummary,
              "consent-summary"),
      Now()));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-task", MemoryCategory::ActiveConversationContext,
              "consent-task"),
      Now()));
  const auto Recall = Memory.Recall(Value, Now(), 1000);
  const auto& Candidates =
      std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
          Recall);
  Check(Candidates.size() == 2 &&
            Candidates.front().Kind ==
                EmulativeDialog::Context::ContextItemKind::
                    ConversationHistory,
        "tier 3 summary recalls before tier 4 active task");
  const auto Summary = std::find_if(
      Candidates.begin(), Candidates.end(),
      [](const auto& Candidate) {
        return Candidate.Id.Value == "memory-summary";
      });
  Check(Summary != Candidates.end() &&
            Summary->Kind ==
                EmulativeDialog::Context::ContextItemKind::
                    ConversationHistory &&
            Summary->Priority ==
                EmulativeDialog::Context::ContextPriorityTier::Tier3,
        "summary maps to conversation history tier 3");
}

void TestRestoreBackupBoundaries() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage, 2);
  const auto Value = Context();
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  StoredMemory Base = ScopeOf(Value);
  Base.MemoryId = "memory-restore";
  Base.Category = MemoryCategory::IdentityPreference;
  Base.SubjectId = "principal-01";
  Base.Content = "Synthetic memory";
  Base.SourceReference = "fixture-message-01";
  Base.ConsentId = "consent-01";
  Base.CreatedAt = Now() - std::chrono::seconds{100};
  Base.Version = 1;
  Storage.BackupRecords = {Base};
  const auto Restored = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Restored) &&
            std::get<std::size_t>(Restored) == 1,
        "eligible backup restored");
  Storage.BackupRecords = {Base};
  const auto Conflict = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Conflict) &&
            std::get<std::size_t>(Conflict) == 0,
        "version conflict backup skipped");
  auto Tombstoned = Base;
  Tombstoned.MemoryId = "memory-tombstoned";
  Tombstoned.Tombstoned = true;
  Storage.BackupRecords = {Tombstoned};
  const auto NoResurrection = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(NoResurrection) &&
            std::get<std::size_t>(NoResurrection) == 0,
        "tombstoned backup never resurrects");
  auto Sensitive = Base;
  Sensitive.MemoryId = "memory-sensitive";
  Sensitive.Content = "api_key=synthetic";
  Storage.BackupRecords = {Sensitive};
  const auto Denied = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Denied) &&
            std::get<std::size_t>(Denied) == 0,
        "sensitive backup denied");

  FakeMemoryStorage AtomicStorage;
  ConversationMemory AtomicMemory(AtomicStorage);
  static_cast<void>(AtomicMemory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  auto ValidCandidate = Base;
  ValidCandidate.MemoryId = "memory-valid-before-invalid";
  ValidCandidate.Content = "\xE4\xB8\xad";
  auto InvalidCandidate = Base;
  InvalidCandidate.MemoryId = "memory-invalid-after-valid";
  InvalidCandidate.Content = std::string("\xED\xA0\x80", 3U);
  AtomicStorage.BackupRecords = {ValidCandidate, InvalidCandidate};
  const auto InvalidRestore = AtomicMemory.RestoreBackup(Value, Now());
  const auto* InvalidRestoreError = AsError(InvalidRestore);
  Check(InvalidRestoreError != nullptr &&
            InvalidRestoreError->Code == MemoryErrorCode::InvalidArgument,
        "later invalid backup candidate rejects the whole restore");
  Check(AtomicStorage.ApplyCalls == 0U && AtomicStorage.Rows.empty(),
        "restore prevalidates every candidate before any storage mutation");

  Storage.BackupRecords = {Base};
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-capacity", MemoryCategory::IdentityPreference), Now()));
  const auto Full = Memory.RestoreBackup(Value, Now());
  Check(std::holds_alternative<std::size_t>(Full) &&
            std::get<std::size_t>(Full) == 0,
        "backup restore respects capacity");
}

void TestFixtureBackupTombstoneCannotBeResurrected() {
  const std::string Fixture =
      ReadFixture("Tests/Contract/Memory/Fixtures/ForgetRecoveryCases.json");
  const auto RecoveryCase =
      FindJsonObjectWithStringField(Fixture, "CaseId", "FR-006");
  const auto TombstonedBackup = FindJsonObjectWithStringField(
      RecoveryCase, "MemoryId", "fixture-memory-tombstoned-01");
  const auto CurrentBackup = FindJsonObjectWithStringField(
      RecoveryCase, "MemoryId", "fixture-memory-current-01");
  Check(!RecoveryCase.empty() && !TombstonedBackup.empty() &&
            !CurrentBackup.empty(),
        "FR-006 backup recovery fixture loaded");
  if (RecoveryCase.empty() || TombstonedBackup.empty() ||
      CurrentBackup.empty()) {
    return;
  }

  const auto TombstonedId =
      JsonStringField(TombstonedBackup, "MemoryId");
  const auto BackupVersion =
      JsonUnsignedField(TombstonedBackup, "BackupVersion");
  const auto TombstoneVersion =
      JsonUnsignedField(TombstonedBackup, "TombstoneVersion");
  const auto ExpectedRestore =
      JsonStringField(TombstonedBackup, "ExpectedRestore");
  const auto CurrentId = JsonStringField(CurrentBackup, "MemoryId");
  const auto CurrentBackupVersion =
      JsonUnsignedField(CurrentBackup, "BackupVersion");
  const auto CurrentVersion =
      JsonUnsignedField(CurrentBackup, "CurrentVersion");
  const auto ExpectedCurrentRestore =
      JsonStringField(CurrentBackup, "ExpectedRestore");
  Check(TombstonedId.has_value() && BackupVersion.has_value() &&
            TombstoneVersion.has_value() && CurrentId.has_value() &&
            CurrentBackupVersion.has_value() && CurrentVersion.has_value() &&
            ExpectedRestore ==
                std::optional<std::string>{"RejectAsTombstoned"} &&
            ExpectedCurrentRestore == std::optional<std::string>{
                                          "AllowIfConsentStillValid"} &&
            *CurrentBackupVersion == *CurrentVersion,
        "FR-006 expected restore outcomes parsed");
  if (!TombstonedId.has_value() || !BackupVersion.has_value() ||
      !TombstoneVersion.has_value() || !CurrentId.has_value() ||
      !CurrentBackupVersion.has_value() || !CurrentVersion.has_value()) {
    return;
  }

  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  const auto Scope = Context();
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Scope)));
  auto Original = Request(*TombstonedId, MemoryCategory::IdentityPreference);
  Original.Version = *BackupVersion;
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Scope, Original, Now())),
        "FR-006 active record written before tombstone");
  ForgetRequest ForgetOne;
  ForgetOne.MemoryId = *TombstonedId;
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Scope, ForgetOne, Now())),
        "FR-006 record tombstoned before restore");
  const auto TombstoneStillRecorded = [&]() {
    const auto ScopeRows = Storage.Rows.find(ScopeKey(ScopeOf(Scope)));
    if (ScopeRows == Storage.Rows.end()) {
      return false;
    }
    const auto TombstoneRow =
        std::find_if(ScopeRows->second.begin(), ScopeRows->second.end(),
                     [&TombstonedId](const StoredMemory& Record) {
                       return Record.MemoryId == *TombstonedId;
                     });
    return TombstoneRow != ScopeRows->second.end() &&
           TombstoneRow->Tombstoned &&
           TombstoneRow->Version == *TombstoneVersion;
  };
  Check(TombstoneStillRecorded(),
        "FR-006 fixture tombstone version established");

  StoredMemory Backup = ScopeOf(Scope);
  Backup.MemoryId = *TombstonedId;
  Backup.Category = MemoryCategory::IdentityPreference;
  Backup.SubjectId = Original.SubjectId;
  Backup.Content = Original.Content;
  Backup.SourceReference = Original.SourceReference;
  Backup.ConsentId = Original.ConsentId;
  Backup.CreatedAt = Original.CreatedAt;
  Backup.Version = *BackupVersion;
  Storage.BackupRecords = {Backup};
  const auto RestoredOld = Memory.RestoreBackup(Scope, Now());
  Check(std::holds_alternative<std::size_t>(RestoredOld) &&
            std::get<std::size_t>(RestoredOld) == 0U,
        "FR-006 older tombstoned backup is rejected");

  Backup.Version = *TombstoneVersion + 1U;
  Storage.BackupRecords = {Backup};
  const auto RestoredNewer = Memory.RestoreBackup(Scope, Now());
  Check(std::holds_alternative<std::size_t>(RestoredNewer) &&
            std::get<std::size_t>(RestoredNewer) == 0U,
        "higher-version backup cannot resurrect tombstone");
  Check(TombstoneStillRecorded(),
        "tombstone remains irreversible after restore attempts");

  auto Keeper = Request("fixture-memory-keeper", MemoryCategory::IdentityPreference);
  Check(std::holds_alternative<std::monostate>(Memory.Write(Scope, Keeper, Now())),
        "atomicity keeper record written");
  MemoryMutationBatch Batch;
  Batch.Scope = ScopeOf(Scope);
  Batch.Tombstones.push_back(Keeper.MemoryId);
  Backup.Version = *TombstoneVersion + 2U;
  Batch.Upserts.push_back(Backup);
  const auto FailedBatch = Storage.ApplyAtomically(Batch);
  Check(std::holds_alternative<MemoryStorageError>(FailedBatch),
        "batch with tombstone resurrection is rejected");
  Check(ActiveCount(Storage, ScopeOf(Scope)) == 1U &&
            TombstoneStillRecorded(),
        "failed resurrection batch rolls back unrelated tombstones");

  const auto CurrentExpected =
      JsonStringField(CurrentBackup, "DeleteState");
  Check(CurrentExpected == std::optional<std::string>{"Active"},
        "FR-006 current backup is active");
  auto Current = Request(*CurrentId, MemoryCategory::IdentityPreference);
  Current.Version = *CurrentBackupVersion;
  StoredMemory CurrentStored = ScopeOf(Scope);
  CurrentStored.MemoryId = Current.MemoryId;
  CurrentStored.Category = Current.Category;
  CurrentStored.SubjectId = Current.SubjectId;
  CurrentStored.Content = Current.Content;
  CurrentStored.SourceReference = Current.SourceReference;
  CurrentStored.ConsentId = Current.ConsentId;
  CurrentStored.CreatedAt = Now();
  CurrentStored.Version = Current.Version;
  Storage.BackupRecords = {CurrentStored};
  const auto RestoredCurrent = Memory.RestoreBackup(Scope, Now());
  Check(std::holds_alternative<std::size_t>(RestoredCurrent) &&
            std::get<std::size_t>(RestoredCurrent) == 1U,
        "FR-006 active backup restores with valid consent");
}

void TestRestartBoundaryAndStorageFailures() {
  FakeMemoryStorage Storage;
  const auto Value = Context();
  {
    ConversationMemory Memory(Storage);
    static_cast<void>(Memory.RegisterConsent(
        Grant(MemoryCategory::IdentityPreference, Value)));
    static_cast<void>(Memory.Write(
        Value,
        Request("memory-durable", MemoryCategory::IdentityPreference), Now()));
  }
  ConversationMemory Restarted(Storage);
  static_cast<void>(Restarted.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  const auto Recall = Restarted.Recall(Value, Now(), 1000);
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Recall)
            .size() == 1,
        "durable scope recovers after restart");
  Storage.FailList = true;
  Check(AsError(Restarted.Recall(Value, Now(), 1000))->Code ==
            MemoryErrorCode::StorageFailure,
        "list failure mapped");
  Storage.FailList = false;
  Storage.FailBackup = true;
  Check(AsError(Restarted.RestoreBackup(Value, Now()))->Code ==
            MemoryErrorCode::StorageFailure,
        "backup failure mapped");
}

void TestAdditionalReviewScenarios() {
  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage, 3);
  const auto Value = Context();
  auto OtherPrincipal =
      Context(ConversationScopeKind::Direct, "principal-02");
  OtherPrincipal.ConversationId = Value.ConversationId;
  Check(AsError(Memory.RevokeConsent(OtherPrincipal, "consent-01", Now()))
            ->Code == MemoryErrorCode::UnauthorizedIdentity,
        "revoke denies foreign principal on same conversation");
  auto EmptyGrantor = Grant(MemoryCategory::IdentityPreference, Value);
  EmptyGrantor.GrantorPrincipalId.clear();
  Check(AsError(Memory.RegisterConsent(EmptyGrantor))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires grantor principal");
  auto EmptyPolicy = Grant(MemoryCategory::IdentityPreference, Value);
  EmptyPolicy.IdentityPolicyVersion = 0;
  Check(AsError(Memory.RegisterConsent(EmptyPolicy))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires identity policy version");
  auto EmptyConversation = Grant(MemoryCategory::IdentityPreference, Value);
  EmptyConversation.ConversationId.clear();
  Check(AsError(Memory.RegisterConsent(EmptyConversation))->Code ==
            MemoryErrorCode::InvalidArgument,
        "consent requires exact conversation scope");
  static_cast<void>(Memory.RegisterConsent(
      Grant(MemoryCategory::IdentityPreference, Value)));
  auto CardSensitive =
      Request("memory-sensitive", MemoryCategory::IdentityPreference);
  CardSensitive.Content = "card_number=synthetic";
  Check(AsError(Memory.Write(Value, CardSensitive, Now()))->Code ==
            MemoryErrorCode::SensitiveContent,
        "payment marker rejected");
  auto CvvSensitive =
      Request("memory-sensitive", MemoryCategory::IdentityPreference);
  CvvSensitive.Content = "cvv=synthetic";
  Check(AsError(Memory.Write(Value, CvvSensitive, Now()))->Code ==
            MemoryErrorCode::SensitiveContent,
        "cvv marker rejected");
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-a", MemoryCategory::IdentityPreference), Now()));
  static_cast<void>(Memory.Write(
      Value,
      Request("memory-b", MemoryCategory::IdentityPreference), Now()));
  ForgetRequest Precise;
  Precise.MemoryId = "memory-a";
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Value, Precise, Now())),
        "precise forget succeeds");
  ForgetRequest WholeScope;
  WholeScope.EntireAuthorizedScope = true;
  Check(std::holds_alternative<std::monostate>(
            Memory.Forget(Value, WholeScope, Now())),
        "entire authorized scope forget succeeds");
  Check(ActiveCount(Storage, ScopeOf(Value)) == 0,
        "scope forget tombstones all matching records");
  auto Session = Context(ConversationScopeKind::Session);
  Check(AsError(Memory.Forget(Session, Precise, Now()))->Code ==
            MemoryErrorCode::SessionScopeVolatile,
        "SessionScope forget rejected");
  Check(AsError(Memory.RevokeConsent(Value, "missing-consent", Now()))->Code ==
            MemoryErrorCode::UnauthorizedIdentity,
        "unknown consent revoke denied");
  auto FutureExpiry =
      Request("memory-expiry", MemoryCategory::IdentityPreference);
  FutureExpiry.ExpiresAt = Now() + std::chrono::seconds{5};
  Check(std::holds_alternative<std::monostate>(
            Memory.Write(Value, FutureExpiry, Now())),
        "future expiry accepted");
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Memory.Recall(Value, Now() + std::chrono::seconds{6}, 1000))
            .empty(),
        "expired recall filtered");
  Check(std::get<std::vector<EmulativeDialog::Context::ContextCandidate>>(
            Memory.Recall(Value, Now(), 1))
            .empty(),
        "tiny token budget yields no candidates");
}

void TestSensitiveMarkersAreRejectedAcrossPersistedFields() {
  const auto BaseContext = Context();
  auto CheckContextField = [&](std::string_view Name,
                               AuthorizedMemoryContext Candidate) {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    const auto Result = Memory.Write(
        Candidate,
        Request("memory-safe", MemoryCategory::IdentityPreference), Now());
    Check(AsError(Result) != nullptr &&
              AsError(Result)->Code == MemoryErrorCode::SensitiveContent,
          Name);
    Check(Storage.Rows.empty(), "sensitive scope does not touch storage");
  };

  auto SensitiveContext = BaseContext;
  SensitiveContext.ChannelId = "qq ACCOUNT_TOKEN = synthetic";
  CheckContextField("case-insensitive scope marker rejected", SensitiveContext);
  SensitiveContext = BaseContext;
  SensitiveContext.AccountId = "account CLIENT_SECRET=synthetic";
  CheckContextField("account secret marker rejected", SensitiveContext);
  SensitiveContext = BaseContext;
  SensitiveContext.ConversationId = "conversation PRIVATE KEY=synthetic";
  CheckContextField("conversation secret marker rejected", SensitiveContext);
  SensitiveContext = BaseContext;
  SensitiveContext.PrincipalId = "principal AUTHORIZATION=Bearer synthetic";
  CheckContextField("principal credential marker rejected", SensitiveContext);

  auto CheckConsentField = [&](std::string_view Name, ConsentGrant Candidate) {
    FakeMemoryStorage Storage;
    ConversationMemory Memory(Storage);
    const auto Result = Memory.RegisterConsent(Candidate);
    Check(AsError(Result) != nullptr &&
              AsError(Result)->Code == MemoryErrorCode::SensitiveContent,
          Name);
    Check(Memory.IsConsentActive("consent-01") == false,
          "sensitive consent is not registered");
  };

  auto SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ConsentId = "consent API_KEY = synthetic";
  CheckConsentField("consent identifier credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.GrantorPrincipalId = "principal PASSWORD=synthetic";
  CheckConsentField("consent grantor credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ChannelId = "channel AWS_SECRET_ACCESS_KEY=synthetic";
  CheckConsentField("consent channel credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.AccountId = "account payment token = synthetic";
  CheckConsentField("consent account payment marker rejected", SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ConversationId = "conversation card_number=synthetic";
  CheckConsentField("consent conversation payment marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.SourceMessageId = "message Bearer synthetic";
  CheckConsentField("consent source credential marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::IdentityPreference, BaseContext);
  SensitiveGrant.ProposalId = "proposal CVV = synthetic";
  CheckConsentField("consent proposal payment marker rejected",
                    SensitiveGrant);
  SensitiveGrant = Grant(MemoryCategory::GroupMemory,
                         Context(ConversationScopeKind::Group));
  SensitiveGrant.AllowedGroupMembers = {"principal-01", "PAN=synthetic"};
  CheckConsentField("consent member payment marker rejected", SensitiveGrant);

  FakeMemoryStorage Storage;
  ConversationMemory Memory(Storage);
  Check(std::holds_alternative<std::monostate>(Memory.RegisterConsent(
            Grant(MemoryCategory::IdentityPreference, BaseContext))),
        "safe consent registers before field checks");
  auto CheckRequestField = [&](std::string_view Name,
                               MemoryWriteRequest Candidate) {
    const auto Result = Memory.Write(BaseContext, Candidate, Now());
    Check(AsError(Result) != nullptr &&
              AsError(Result)->Code == MemoryErrorCode::SensitiveContent,
          Name);
    Check(Storage.Rows.empty(), "sensitive record does not touch storage");
  };

  auto SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.MemoryId = "memory password = synthetic";
  CheckRequestField("memory id credential marker rejected", SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.SubjectId = "subject client_secret=synthetic";
  CheckRequestField("subject credential marker rejected", SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.Content = "PRIVATE KEY = synthetic";
  CheckRequestField("content marker is case-insensitive and whitespace-safe",
                    SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.SourceReference = "message credit_card=synthetic";
  CheckRequestField("source reference payment marker rejected",
                    SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.ConsentId = "consent access_token=synthetic";
  CheckRequestField("request consent credential marker rejected",
                    SensitiveRequest);
  SensitiveRequest =
      Request("memory-safe", MemoryCategory::IdentityPreference);
  SensitiveRequest.ProposalId = "proposal SSN=synthetic";
  CheckRequestField("request proposal personal-data marker rejected",
                    SensitiveRequest);

  auto CheckBackupField = [&](std::string_view Name,
                              StoredMemory Candidate) {
    FakeMemoryStorage BackupStorage;
    ConversationMemory BackupMemory(BackupStorage);
    Check(std::holds_alternative<std::monostate>(BackupMemory.RegisterConsent(
              Grant(MemoryCategory::IdentityPreference, BaseContext))),
          "safe consent registers before backup field check");
    BackupStorage.BackupRecords = {std::move(Candidate)};
    const auto Result = BackupMemory.RestoreBackup(BaseContext, Now());
    Check(std::holds_alternative<std::size_t>(Result) &&
              std::get<std::size_t>(Result) == 0U,
          Name);
    Check(BackupStorage.Rows.empty(), "sensitive backup does not persist");
  };

  StoredMemory Backup = ScopeOf(BaseContext);
  Backup.MemoryId = "memory-safe";
  Backup.Category = MemoryCategory::IdentityPreference;
  Backup.SubjectId = "principal-01";
  Backup.Content = "Synthetic memory";
  Backup.SourceReference = "fixture-message-01";
  Backup.ConsentId = "consent-01";
  Backup.CreatedAt = Now();
  Backup.Version = 1;
  auto SensitiveBackup = Backup;
  SensitiveBackup.MemoryId = "memory API_KEY = synthetic";
  CheckBackupField("backup memory identifier marker rejected",
                   SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.SubjectId = "subject CLIENT_SECRET=synthetic";
  CheckBackupField("backup subject marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.Content = "PASSWORD = synthetic";
  CheckBackupField("backup content marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.SourceReference = "source Bearer synthetic";
  CheckBackupField("backup source marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.ConsentId = "consent AWS_SECRET_ACCESS_KEY=synthetic";
  CheckBackupField("backup consent marker rejected", SensitiveBackup);
  SensitiveBackup = Backup;
  SensitiveBackup.ProposalId = "proposal PAYMENT TOKEN = synthetic";
  CheckBackupField("backup proposal marker rejected", SensitiveBackup);

  FakeMemoryStorage SensitiveScopeStorage;
  ConversationMemory SensitiveScopeMemory(SensitiveScopeStorage);
  auto SensitiveScope = BaseContext;
  SensitiveScope.ConversationId = "conversation CARD_NUMBER=synthetic";
  const auto SensitiveScopeResult =
      SensitiveScopeMemory.RestoreBackup(SensitiveScope, Now());
  Check(AsError(SensitiveScopeResult) != nullptr &&
            AsError(SensitiveScopeResult)->Code ==
                MemoryErrorCode::SensitiveContent,
        "restore rejects sensitive scope identifiers");
  Check(SensitiveScopeStorage.Rows.empty(),
        "sensitive restore scope does not touch storage");
}

} // namespace

int main() {
  TestIdentitySessionAndPolicy();
  TestConsentValidationAndProposalBinding();
  TestOptionalProposalConsentRequiresExactMatch();
  TestScopeKindAndGroupMembership();
  TestFixtureGroupConsentAllowsListedMemberRecall();
  TestRevocationAtomicityAndScope();
  TestForgetAndExpiry();
  TestCapacityAndWriteAtomicity();
  TestDurableContentRequiresStrictUtf8();
  TestRecallPriorityAndSummaryMapping();
  TestRestoreBackupBoundaries();
  TestFixtureBackupTombstoneCannotBeResurrected();
  TestRestartBoundaryAndStorageFailures();
  TestAdditionalReviewScenarios();
  TestSensitiveMarkersAreRejectedAcrossPersistedFields();
  if (Failures != 0) {
    std::cerr << Failures << " checks failed\n";
    return 1;
  }
  std::cout << "All ConversationMemory checks passed\n";
  return 0;
}
