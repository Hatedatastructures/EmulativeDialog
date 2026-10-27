#include "Source/Core/CoreChatService.h"
#include "Source/Core/CoreAuthorizedAction.h"
#include "Source/Core/IdentityPolicy.h"
#include "Source/Core/OperationRecord.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace EmulativeDialog;
using CoreCredentialLease = Core::CredentialLease;
using ProviderCredentialLease = Providers::CredentialLease;

void Check(bool Condition, std::string_view Message) {
  if (!Condition) throw std::runtime_error(std::string(Message));
}

Core::IdentityBinding MakeBinding(std::string Sender = "SenderA") {
  return {{Channels::ChannelId{"MockChannel"}, Channels::AccountId{"AccountA"},
           Channels::IntegrationMode{"Mock"}},
          Channels::SenderId{std::move(Sender)}, Core::PrincipalId{"PrincipalA"}};
}

Channels::ChannelEnvelope MakeEnvelope(std::string Dedup = "DedupA",
                                       std::string Sender = "SenderA",
                                       std::string Account = "AccountA",
                                       std::string Mode = "Mock") {
  return {Channels::ChannelId{"MockChannel"}, Channels::IntegrationMode{std::move(Mode)},
          Channels::AccountId{std::move(Account)}, Channels::ConversationId{"ConversationA"},
          Channels::ConversationKind::Private, Channels::SenderId{std::move(Sender)},
          Channels::MessageId{"MessageA"}, Channels::PlatformEventId{"PlatformEventA"},
          std::chrono::system_clock::now(), Channels::StableDeduplicationId{std::move(Dedup)}};
}

Channels::InboundChannelMessage MakeMessage(std::string Dedup = "DedupA",
                                            std::string Sender = "SenderA",
                                            std::string Account = "AccountA",
                                            std::string Mode = "Mock",
                                            std::string Text = "hello Core") {
  Channels::CanonicalMessage Message;
  Message.MessageParts.emplace_back(Channels::TextPart{std::move(Text)});
  return {MakeEnvelope(std::move(Dedup), std::move(Sender), std::move(Account), std::move(Mode)),
          std::move(Message)};
}

Channels::CapabilitySnapshot MakeCapabilities(
    std::uint64_t Version = 5,
    Channels::CapabilityStatus ReceiveStatus = Channels::CapabilityStatus::Supported,
    Channels::VerificationStatus ReceiveEvidence = Channels::VerificationStatus::Verified,
    Channels::CapabilityStatus SendStatus = Channels::CapabilityStatus::Supported,
    Channels::VerificationStatus SendEvidence = Channels::VerificationStatus::Verified) {
  const auto Now = std::chrono::system_clock::now();
  return {Channels::ChannelId{"MockChannel"}, Channels::AccountId{"AccountA"},
          Channels::IntegrationMode{"Mock"}, Version, Now, std::nullopt,
          Channels::VerificationStatus::Verified, Channels::VerificationStatus::Verified,
          {{Channels::ChannelCapability::ReceiveText, ReceiveStatus, ReceiveEvidence, Now,
            std::nullopt, std::nullopt},
           {Channels::ChannelCapability::SendText, SendStatus, SendEvidence, Now,
            std::nullopt, std::nullopt}}};
}

Providers::ModelProviderProfile MakeProfile() {
  Providers::ModelProviderProfile Profile;
  Profile.Id = Providers::ProviderProfileId{"ProviderProfileA"};
  Profile.Protocol = Providers::ProviderProtocol::OpenAIResponses;
  Profile.Endpoint = "https://provider.invalid";
  Profile.ModelName = "synthetic-model";
  Profile.Credential = Providers::CredentialRef{"CredentialRefA"};
  return Profile;
}

Core::CoreActionIdentifiers MakeIds(std::string Suffix) {
  return {{"Action" + Suffix}, {"Operation" + Suffix}, {"Key" + Suffix}};
}

class ChannelLease final : public CoreCredentialLease {
public:
  std::string_view GetAuthenticationSecret() const noexcept override {
    return "synthetic-channel-credential";
  }
};

class ProviderLease final : public ProviderCredentialLease {
public:
  std::string_view GetAuthenticationSecret() const noexcept override {
    return "synthetic-provider-credential";
  }
};

class FakeProvider final : public Providers::ModelProviderPort {
public:
  Providers::ProviderResult<Providers::ProviderCapabilities>
  GetCapabilities(const Providers::ProviderProfileId&) const override {
    return Providers::ProviderCapabilities{};
  }
  Providers::ProviderResult<std::monostate>
  ValidateProfile(const Providers::ModelProviderProfile&) const override {
    return std::monostate{};
  }
  Providers::CanonicalTerminalEvent StreamTurn(
      const Providers::ModelProviderProfile&, const ProviderCredentialLease&,
      const Providers::CanonicalModelRequest& Request, std::stop_token,
      const Providers::EventSink&) override {
    Providers::ResponseOutcomeUnknownEvent Event;
    Event.Metadata.Request = Request.Id;
    Event.Metadata.TurnId = Request.TurnId;
    Event.Reason.Code.Value = "unused";
    return Event;
  }
  Providers::ProviderResult<Providers::ProviderHealthStatus>
  HealthCheck(const Providers::ModelProviderProfile&) override {
    return Providers::ProviderHealthStatus{Providers::ProviderHealthState::Available};
  }
};

class FakeAgent final : public Core::AgentRuntimePort {
public:
  std::deque<Core::AgentRunResult> Results;
  std::size_t Calls{};
  bool ThrowOnRun{};
  std::function<void()> OnRun;
  std::optional<Core::AuthorizedTurnContext> LastContext;

  void Push(Core::AgentRunResult Result) { Results.emplace_back(std::move(Result)); }
  Core::AgentRunResult RunTurn(const Core::AuthorizedTurnContext& Context,
      Providers::ModelProviderPort&, const Providers::ModelProviderProfile&,
      const ProviderCredentialLease&, std::stop_token) override {
    ++Calls;
    LastContext = Context;
    if (OnRun) OnRun();
    if (ThrowOnRun) throw std::runtime_error("synthetic AgentRuntime failure");
    if (Results.empty()) return Core::AgentRunResult::Failed(Core::AgentRunFailureCode::InternalError);
    auto Result = std::move(Results.front());
    Results.pop_front();
    return Result;
  }
};

class IdSource final : public Core::CoreActionIdSource {
public:
  std::deque<Core::CoreActionIdentifiers> Values;
  std::size_t Calls{};
  bool ThrowOnNext{};
  void Push(Core::CoreActionIdentifiers Value) { Values.emplace_back(std::move(Value)); }
  Core::CoreActionIdentifiers Next() override {
    ++Calls;
    if (ThrowOnNext) throw std::runtime_error("synthetic ID source failure");
    if (Values.empty()) return {};
    auto Value = std::move(Values.front());
    Values.pop_front();
    return Value;
  }
};

class OperationProbe final : public Core::OperationRecordObserver {
public:
  std::vector<Core::OperationState> States;
  void OnOperationRecordChanged(const Core::OperationRecord& Record) noexcept override {
    try { States.push_back(Record.GetState()); } catch (...) {}
  }
  std::optional<Core::OperationState> LastState() const noexcept {
    if (States.empty()) return std::nullopt;
    return States.back();
  }
};

class FakeChannel final : public Channels::ChannelAdapterPort {
public:
  std::vector<Channels::CapabilitySnapshot> Snapshots{MakeCapabilities()};
  std::size_t SnapshotIndex{};
  std::size_t CapabilityCalls{};
  std::size_t ThrowOnCapabilityCall{};
  std::size_t SendCalls{};
  bool ThrowOnSend{};
  OperationProbe* Probe{};
  std::optional<Core::OperationState> StateAtSend;
  std::string SentText;
  std::string SentAuthorization;
  std::uint64_t SentPolicyVersion{};
  std::uint64_t SentCapabilityVersion{};
  std::function<Channels::ChannelReceipt(const Core::CoreAuthorizedAction&)> Receipt;

  void SetSnapshots(std::vector<Channels::CapabilitySnapshot> Values) {
    Snapshots = std::move(Values);
    SnapshotIndex = 0;
  }
  Channels::CapabilitySnapshot GetCapabilities(const Channels::AccountProfile&) override {
    ++CapabilityCalls;
    if (ThrowOnCapabilityCall == CapabilityCalls) throw std::runtime_error("capability query failed");
    if (Snapshots.empty()) return {};
    const std::size_t Index = std::min(SnapshotIndex, Snapshots.size() - 1);
    if (SnapshotIndex + 1 < Snapshots.size()) ++SnapshotIndex;
    return Snapshots[Index];
  }
  Channels::StartResult Start(const Channels::AccountProfile&, const CoreCredentialLease&) override {
    return {Channels::StartStatus::Started, Channels::ChannelErrorCode::None};
  }
  Channels::StopResult Stop() override {
    return {Channels::StopStatus::Stopped, Channels::ChannelErrorCode::None};
  }
  Channels::SubscriptionResult SubscribeEvents(Channels::ChannelEventSink&) override {
    return {Channels::SubscriptionStatus::Subscribed, Channels::ChannelErrorCode::None};
  }
  Channels::ChannelReceipt SendAction(const Core::CoreAuthorizedAction& Action,
                                       const CoreCredentialLease&) override {
    ++SendCalls;
    if (Probe != nullptr) StateAtSend = Probe->LastState();
    SentText = std::string(Action.GetTextPayload());
    SentAuthorization = Action.GetAuthorizationRef().Value;
    SentPolicyVersion = Action.GetPolicyVersion();
    SentCapabilityVersion = Action.GetCapabilitySnapshotVersion();
    if (ThrowOnSend) throw std::runtime_error("synthetic post-dispatch exception");
    if (Receipt) return Receipt(Action);
    return {Action.GetActionId(), Action.GetOperationId(), std::nullopt,
            std::chrono::system_clock::now(), Channels::ReceiptStatus::Succeeded,
            Channels::ChannelErrorCode::None, std::nullopt};
  }
  Channels::HealthSnapshot GetHealthSnapshot() const override {
    return {Channels::ConnectionStatus::Connected, Channels::AuthenticationStatus::Valid,
            Channels::RateLimitStatus::Normal, std::chrono::system_clock::now(), std::nullopt,
            Channels::ChannelErrorCode::None};
  }
};

struct Fixture final {
  Core::IdentityPolicy Policy{7, {MakeBinding()}};
  FakeAgent Agent;
  FakeProvider Provider;
  Providers::ModelProviderProfile Profile{MakeProfile()};
  ProviderLease ProviderCredential;
  FakeChannel Channel;
  ChannelLease ChannelCredential;
  IdSource Identifiers;
  OperationProbe Probe;
  Core::CoreChatService Service;
  Fixture() : Service(Policy, Agent, Provider, Profile, ProviderCredential, Channel,
                      ChannelCredential, Identifiers, &Probe) { Channel.Probe = &Probe; }
};

void TestAC1TrustedEnvelopeAndExactScope() {
  Core::IdentityPolicy Policy{7, {MakeBinding()}};
  auto Result = Policy.Resolve(MakeEnvelope());
  Check(std::holds_alternative<Core::ResolvedIdentity>(Result), "valid envelope rejected");
  const auto& Mapped = std::get<Core::ResolvedIdentity>(Result);
  Check(Mapped.Principal.Value == "PrincipalA" &&
            Mapped.Scope.Channel.Value == "MockChannel" &&
            Mapped.Scope.Account.Value == "AccountA" &&
            Mapped.Scope.Mode.Value == "Mock" &&
            Mapped.Scope.Conversation.Value == "ConversationA" &&
            Mapped.Scope.Sender.Value == "SenderA",
        "Core did not preserve the trusted envelope identity/scope");
  auto Unknown = Policy.Resolve(MakeEnvelope("DedupU", "UnknownSender"));
  Check(std::get<Core::IdentityPolicyFailure>(Unknown).Code ==
            Core::IdentityPolicyFailureCode::UnknownSender,
        "unknown sender did not fail closed");
  auto WrongMode = Policy.Resolve(MakeEnvelope("DedupM", "SenderA", "AccountA", "Live"));
  Check(std::get<Core::IdentityPolicyFailure>(WrongMode).Code ==
            Core::IdentityPolicyFailureCode::AccountRouteMismatch,
        "account route mismatch did not fail closed");
  Channels::ChannelEnvelope Empty = MakeEnvelope();
  Empty.Conversation.Value.clear();
  Check(std::get<Core::IdentityPolicyFailure>(Policy.Resolve(Empty)).Code ==
            Core::IdentityPolicyFailureCode::InvalidEnvelope,
        "empty trusted envelope field was accepted");
}

void TestAC1UnknownAccountRouteMismatchAndMalformedEnvelopeFailClosed() {
  {
    Fixture FixtureValue;
    const auto Result = FixtureValue.Service.Process(
        MakeMessage("DedupUnknownAccount", "SenderA", "OtherAccount"));
    Check(Result.Status == Core::CoreChatStatus::Rejected &&
              Result.Failure == Core::CoreChatFailureCode::UnknownAccount &&
              FixtureValue.Agent.Calls == 0 && FixtureValue.Identifiers.Calls == 0,
          "unregistered account was not rejected before runtime");
  }
  {
    Fixture FixtureValue;
    const auto Result = FixtureValue.Service.Process(
        MakeMessage("DedupWrongRoute", "SenderA", "AccountA", "Live"));
    Check(Result.Status == Core::CoreChatStatus::Rejected &&
              Result.Failure == Core::CoreChatFailureCode::AccountRouteMismatch &&
              FixtureValue.Agent.Calls == 0 && FixtureValue.Identifiers.Calls == 0,
          "integration-mode route mismatch was not rejected before runtime");
  }
  {
    Fixture FixtureValue;
    Channels::InboundChannelMessage Message = MakeMessage("DedupMalformed");
    Message.Envelope.Channel.Value.clear();
    const auto Result = FixtureValue.Service.Process(Message);
    Check(Result.Status == Core::CoreChatStatus::Rejected &&
              Result.Failure == Core::CoreChatFailureCode::InvalidEnvelope &&
              FixtureValue.Agent.Calls == 0 && FixtureValue.Identifiers.Calls == 0,
          "malformed trusted envelope was not rejected before runtime");
  }
}
void TestAC2DeniedDuplicateAndUnsupportedInputsDoNotRunOrSend() {
  Fixture UnknownFixture;
  UnknownFixture.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, "never used"));
  UnknownFixture.Identifiers.Push(MakeIds("A2"));
  const auto Unknown = UnknownFixture.Service.Process(MakeMessage("DedupUnknown", "Stranger"));
  Check(Unknown.Status == Core::CoreChatStatus::Rejected &&
            UnknownFixture.Agent.Calls == 0 && UnknownFixture.Identifiers.Calls == 0 &&
            UnknownFixture.Channel.SendCalls == 0,
        "untrusted sender invoked runtime, ID source, or Channel");

  Fixture DuplicateFixture;
  DuplicateFixture.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, "one reply"));
  DuplicateFixture.Identifiers.Push(MakeIds("A2"));
  const auto Message = MakeMessage("DedupOnce");
  const auto First = DuplicateFixture.Service.Process(Message);
  const auto Duplicate = DuplicateFixture.Service.Process(Message);
  Check(First.Status == Core::CoreChatStatus::Completed &&
            Duplicate.Status == Core::CoreChatStatus::Duplicate &&
            DuplicateFixture.Agent.Calls == 1 && DuplicateFixture.Identifiers.Calls == 1 &&
            DuplicateFixture.Channel.SendCalls == 1,
        "duplicate stable deduplication ID caused a second turn or action");

  Fixture UnsupportedFixture;
  Channels::InboundChannelMessage Media = MakeMessage("DedupMedia");
  Media.Message.MessageParts.clear();
  Media.Message.MessageParts.emplace_back(Channels::MediaReferencePart{
      Channels::MediaReferenceId{"SyntheticImage"}, Channels::MediaReferenceKind::Image});
  const auto Unsupported = UnsupportedFixture.Service.Process(Media);
  Check(Unsupported.Status == Core::CoreChatStatus::Unsupported &&
            UnsupportedFixture.Agent.Calls == 0 && UnsupportedFixture.Identifiers.Calls == 0 &&
            UnsupportedFixture.Channel.SendCalls == 0,
        "unsupported input reached runtime, ID source, or Channel");
}

void TestAC3RunResultAndFailureMappingFailClosed() {
  const std::array<std::pair<Providers::FailureCode, Core::AgentRunFailureCode>, 9> Mappings{{
      {Providers::FailureCode::InvalidProfile, Core::AgentRunFailureCode::InvalidProfile},
      {Providers::FailureCode::InvalidRequest, Core::AgentRunFailureCode::InvalidRequest},
      {Providers::FailureCode::UnsupportedCapability, Core::AgentRunFailureCode::UnsupportedCapability},
      {Providers::FailureCode::AuthenticationFailure, Core::AgentRunFailureCode::AuthenticationFailure},
      {Providers::FailureCode::RateLimited, Core::AgentRunFailureCode::RateLimited},
      {Providers::FailureCode::TransportFailure, Core::AgentRunFailureCode::TransportFailure},
      {Providers::FailureCode::InvalidStream, Core::AgentRunFailureCode::InvalidStream},
      {Providers::FailureCode::ProviderRejected, Core::AgentRunFailureCode::ProviderRejected},
      {Providers::FailureCode::ContentFiltered, Core::AgentRunFailureCode::ContentFiltered}}};
  for (const auto& Mapping : Mappings) {
    Check(Core::MapProviderFailureCode(Mapping.first) == Mapping.second,
          "Provider failure code mapping is not one-to-one");
  }

  Fixture FailedFixture;
  FailedFixture.Agent.Push(Core::AgentRunResult::Failed(Core::AgentRunFailureCode::OutputLimitReached));
  FailedFixture.Identifiers.Push(MakeIds("A3"));
  const auto Failed = FailedFixture.Service.Process(MakeMessage("DedupLimit"));
  Check(Failed.Status == Core::CoreChatStatus::Failed &&
            Failed.AgentFailure == Core::AgentRunFailureCode::OutputLimitReached &&
            FailedFixture.Identifiers.Calls == 0 && FailedFixture.Channel.SendCalls == 0,
        "OutputLimitReached created an action or send");

  for (const auto Completion : {Providers::CompletionKind::ToolHandoff}) {
    Fixture HandoffFixture;
    HandoffFixture.Agent.Push(Core::AgentRunResult::ResponseReady(Completion, "tool text"));
    HandoffFixture.Identifiers.Push(MakeIds("A4"));
    const auto Handoff = HandoffFixture.Service.Process(MakeMessage("DedupHandoff"));
    Check(Handoff.Status == Core::CoreChatStatus::Failed &&
              Handoff.Failure == Core::CoreChatFailureCode::InvalidAgentResult &&
              HandoffFixture.Identifiers.Calls == 0 && HandoffFixture.Channel.SendCalls == 0,
          "ToolHandoff was treated as a final text reply");
  }

  Fixture EmptyFixture;
  EmptyFixture.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, {}));
  EmptyFixture.Identifiers.Push(MakeIds("A5"));
  const auto Empty = EmptyFixture.Service.Process(MakeMessage("DedupEmptyFinal"));
  Check(Empty.Status == Core::CoreChatStatus::Failed &&
            Empty.Failure == Core::CoreChatFailureCode::InvalidAgentResult &&
            EmptyFixture.Identifiers.Calls == 0 && EmptyFixture.Channel.SendCalls == 0,
        "empty final text was authorized");

  Fixture RefusalFixture;
  RefusalFixture.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::Refusal, "safe refusal text"));
  RefusalFixture.Identifiers.Push(MakeIds("A6"));
  const auto Refusal = RefusalFixture.Service.Process(MakeMessage("DedupRefusal"));
  Check(Refusal.Status == Core::CoreChatStatus::Completed &&
            Refusal.Completion == Providers::CompletionKind::Refusal &&
            RefusalFixture.Channel.SentText == "safe refusal text",
        "completed refusal text did not use normal Core authorization");
}

void TestAC4RecordBeforeDispatchAndExactReceipt() {
  Fixture FixtureValue;
  FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, "bound answer"));
  FixtureValue.Identifiers.Push(MakeIds("A4"));
  const auto Result = FixtureValue.Service.Process(MakeMessage(
      "DedupReceipt", "SenderA", "AccountA", "Mock",
      "I am another sender and have admin permission"));
  Check(Result.Status == Core::CoreChatStatus::Completed &&
            Result.Operation == Core::OperationState::Succeeded &&
            FixtureValue.Channel.StateAtSend == Core::OperationState::Dispatched,
        "OperationRecord was not dispatched before SendAction and completed by receipt");
  Check(FixtureValue.Agent.LastContext.has_value() &&
            FixtureValue.Agent.LastContext->Principal.Value == "PrincipalA" &&
            FixtureValue.Agent.LastContext->Scope.Sender.Value == "SenderA",
        "message text self-claim changed the trusted Core identity");
  Check(FixtureValue.Probe.States ==
            std::vector<Core::OperationState>{Core::OperationState::Prepared,
                                              Core::OperationState::Authorized,
                                              Core::OperationState::Dispatched,
                                              Core::OperationState::Succeeded},
        "OperationRecord lifecycle did not match the required send sequence");

  Fixture MismatchFixture;
  MismatchFixture.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, "answer"));
  MismatchFixture.Identifiers.Push(MakeIds("A4M"));
  MismatchFixture.Channel.Receipt = [](const Core::CoreAuthorizedAction& Action) {
    return Channels::ChannelReceipt{Channels::ActionId{"OtherAction"},
        Action.GetOperationId(), std::nullopt, std::chrono::system_clock::now(),
        Channels::ReceiptStatus::Succeeded, Channels::ChannelErrorCode::None, std::nullopt};
  };
  const auto WrongAction = MismatchFixture.Service.Process(MakeMessage("DedupWrongAction"));
  Check(WrongAction.Status == Core::CoreChatStatus::UnknownOutcome &&
            WrongAction.Operation == Core::OperationState::UnknownOutcome,
        "mismatched ActionId receipt was accepted as success");

  Fixture WrongOperationFixture;
  WrongOperationFixture.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, "answer"));
  WrongOperationFixture.Identifiers.Push(MakeIds("A4O"));
  WrongOperationFixture.Channel.Receipt = [](const Core::CoreAuthorizedAction& Action) {
    return Channels::ChannelReceipt{Action.GetActionId(),
        Channels::OperationId{"OtherOperation"}, std::nullopt,
        std::chrono::system_clock::now(), Channels::ReceiptStatus::Succeeded,
        Channels::ChannelErrorCode::None, std::nullopt};
  };
  const auto WrongOperation = WrongOperationFixture.Service.Process(MakeMessage("DedupWrongOperation"));
  Check(WrongOperation.Status == Core::CoreChatStatus::UnknownOutcome &&
            WrongOperation.Operation == Core::OperationState::UnknownOutcome &&
            WrongOperationFixture.Channel.SendCalls == 1,
        "mismatched OperationId receipt was accepted as success");
}
void TestAC5CapabilityVersionsAndExactScopeAreEnforced() {
  Fixture FixtureValue;
  FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, "versioned reply"));
  FixtureValue.Identifiers.Push(MakeIds("A5"));
  const auto Result = FixtureValue.Service.Process(MakeMessage("DedupVersion"));
  Check(Result.Status == Core::CoreChatStatus::Completed &&
            FixtureValue.Channel.SentPolicyVersion == FixtureValue.Policy.GetPolicyVersion() &&
            FixtureValue.Channel.SentCapabilityVersion == 5 &&
            FixtureValue.Channel.SentAuthorization.starts_with("CoreAuthorization:"),
        "action did not carry exact current policy/capability versions and Core authorization reference");

  Fixture Unverified;
  Unverified.Channel.SetSnapshots({MakeCapabilities(),
      MakeCapabilities(5, Channels::CapabilityStatus::Supported,
                       Channels::VerificationStatus::Verified,
                       Channels::CapabilityStatus::Supported,
                       Channels::VerificationStatus::Pending)});
  Unverified.Agent.Push(Core::AgentRunResult::ResponseReady(
      Providers::CompletionKind::FinalAnswer, "must not send"));
  Unverified.Identifiers.Push(MakeIds("A5U"));
  const auto Denied = Unverified.Service.Process(MakeMessage("DedupUnverifiedSend"));
  Check(Denied.Status == Core::CoreChatStatus::Unsupported &&
            Unverified.Identifiers.Calls == 0 && Unverified.Channel.SendCalls == 0,
        "unverified SendText capability was treated as authorization");
}

void TestAgentFailuresCancelledUnsupportedAndUnknownNeverAllocateIds() {
  const std::vector<std::pair<Core::AgentRunResult, Core::CoreChatStatus>> Cases{
      {Core::AgentRunResult::Failed(Core::AgentRunFailureCode::OutputLimitReached), Core::CoreChatStatus::Failed},
      {Core::AgentRunResult::Failed(Core::AgentRunFailureCode::InvalidStream), Core::CoreChatStatus::Failed},
      {Core::AgentRunResult::Cancelled(), Core::CoreChatStatus::Cancelled},
      {Core::AgentRunResult::Unsupported(), Core::CoreChatStatus::Unsupported},
      {Core::AgentRunResult::UnknownOutcome(), Core::CoreChatStatus::UnknownOutcome}};
  std::size_t Index = 0;
  for (const auto& Case : Cases) {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Case.first);
    FixtureValue.Identifiers.Push(MakeIds("Status" + std::to_string(Index++)));
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupStatus" + std::to_string(Index)));
    Check(Result.Status == Case.second && FixtureValue.Identifiers.Calls == 0 &&
              FixtureValue.Channel.SendCalls == 0,
          "non-ready AgentRunResult caused ID allocation or Channel dispatch");
  }
}

void TestAC7NextCallCountsAndTupleRejection() {
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Identifiers.Push(MakeIds("AC7A"));
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupAC7Approved"));
    Check(Result.Status == Core::CoreChatStatus::Completed &&
              FixtureValue.Identifiers.Calls == 1 && FixtureValue.Channel.SendCalls == 1,
          "approved send did not call CoreActionIdSource::Next exactly once");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Identifiers.Push(Core::CoreActionIdentifiers{});
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupAC7Empty"));
    Check(Result.Status == Core::CoreChatStatus::Failed &&
              Result.Failure == Core::CoreChatFailureCode::InvalidActionIdentifiers &&
              FixtureValue.Identifiers.Calls == 1 && FixtureValue.Channel.SendCalls == 0,
          "empty ID tuple did not fail closed");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Identifiers.Push(Core::CoreActionIdentifiers{
        Channels::ActionId{"Same"}, Channels::OperationId{"Same"},
        Channels::IdempotencyKey{"Same"}});
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupAC7RepeatedField"));
    Check(Result.Status == Core::CoreChatStatus::Failed &&
              Result.Failure == Core::CoreChatFailureCode::InvalidActionIdentifiers &&
              FixtureValue.Identifiers.Calls == 1 && FixtureValue.Channel.SendCalls == 0,
          "duplicate IDs within one tuple did not fail closed");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "first"));
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "second"));
    const auto SameTuple = MakeIds("AC7Duplicate");
    FixtureValue.Identifiers.Push(SameTuple);
    FixtureValue.Identifiers.Push(SameTuple);
    const auto First = FixtureValue.Service.Process(MakeMessage("DedupAC7First"));
    const auto Second = FixtureValue.Service.Process(MakeMessage("DedupAC7Second"));
    Check(First.Status == Core::CoreChatStatus::Completed &&
              Second.Status == Core::CoreChatStatus::Failed &&
              Second.Failure == Core::CoreChatFailureCode::InvalidActionIdentifiers &&
              FixtureValue.Identifiers.Calls == 2 && FixtureValue.Channel.SendCalls == 1,
          "reused ID tuple did not fail closed without a second send");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Identifiers.ThrowOnNext = true;
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupAC7Throw"));
    Check(Result.Status == Core::CoreChatStatus::Failed &&
              Result.Failure == Core::CoreChatFailureCode::InvalidActionIdentifiers &&
              FixtureValue.Identifiers.Calls == 1 && FixtureValue.Channel.SendCalls == 0,
          "ID source exception did not fail closed");
  }
}
void TestAC7DeniedPathsDoNotCallNext() {
  {
    Fixture FixtureValue;
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupAC7Denied", "UnknownSender"));
    Check(Result.Status == Core::CoreChatStatus::Rejected && FixtureValue.Identifiers.Calls == 0,
          "rejected identity called Next");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Identifiers.Push(MakeIds("AC7D"));
    const auto Message = MakeMessage("DedupAC7Replay");
    Check(FixtureValue.Service.Process(Message).Status == Core::CoreChatStatus::Completed,
          "first inbound send failed");
    const auto Duplicate = FixtureValue.Service.Process(Message);
    Check(Duplicate.Status == Core::CoreChatStatus::Duplicate && FixtureValue.Identifiers.Calls == 1,
          "duplicate inbound called Next again");
  }
  {
    Fixture FixtureValue;
    Channels::InboundChannelMessage Media = MakeMessage("DedupAC7Media");
    Media.Message.MessageParts.clear();
    Media.Message.MessageParts.emplace_back(Channels::MediaReferencePart{
        Channels::MediaReferenceId{"SyntheticMedia"}, Channels::MediaReferenceKind::Image});
    Check(FixtureValue.Service.Process(Media).Status == Core::CoreChatStatus::Unsupported &&
              FixtureValue.Identifiers.Calls == 0,
          "unsupported inbound path called Next");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Channel.SetSnapshots({MakeCapabilities(),
        MakeCapabilities(5, Channels::CapabilityStatus::Supported,
                         Channels::VerificationStatus::Verified,
                         Channels::CapabilityStatus::Supported,
                         Channels::VerificationStatus::Pending)});
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(Providers::CompletionKind::FinalAnswer, "reply"));
    Check(FixtureValue.Service.Process(MakeMessage("DedupAC7Capability")).Status ==
              Core::CoreChatStatus::Unsupported && FixtureValue.Identifiers.Calls == 0,
          "unverified capability path called Next");
  }
}

void TestCapabilityVersionChangeFailsBeforeDispatch() {
  Fixture FixtureValue;
  FixtureValue.Channel.SetSnapshots({MakeCapabilities(8), MakeCapabilities(8), MakeCapabilities(9)});
  FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(Providers::CompletionKind::FinalAnswer, "reply"));
  FixtureValue.Identifiers.Push(MakeIds("VersionChange"));
  const auto Result = FixtureValue.Service.Process(MakeMessage("DedupVersionChange"));
  Check(Result.Status == Core::CoreChatStatus::Unsupported &&
            Result.Failure == Core::CoreChatFailureCode::CapabilityVersionChanged &&
            Result.Operation == Core::OperationState::Failed &&
            FixtureValue.Identifiers.Calls == 1 && FixtureValue.Channel.SendCalls == 0 &&
            FixtureValue.Probe.LastState() == Core::OperationState::Failed,
        "capability version change was not detected immediately before dispatch");
}

void TestUnknownOutcomeReceiptIsNeverRetried() {
  Fixture FixtureValue;
  FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(Providers::CompletionKind::FinalAnswer, "reply"));
  FixtureValue.Identifiers.Push(MakeIds("Unknown"));
  FixtureValue.Channel.Receipt = [](const Core::CoreAuthorizedAction& Action) {
    return Channels::ChannelReceipt{Action.GetActionId(), Action.GetOperationId(), std::nullopt,
        std::chrono::system_clock::now(), Channels::ReceiptStatus::UnknownOutcome,
        Channels::ChannelErrorCode::OutcomeUnknown, std::nullopt};
  };
  const auto Message = MakeMessage("DedupUnknownReceipt");
  const auto First = FixtureValue.Service.Process(Message);
  const auto Retry = FixtureValue.Service.Process(Message);
  Check(First.Status == Core::CoreChatStatus::UnknownOutcome &&
            First.Operation == Core::OperationState::UnknownOutcome &&
            Retry.Status == Core::CoreChatStatus::Duplicate &&
            FixtureValue.Channel.SendCalls == 1 && FixtureValue.Agent.Calls == 1,
        "UnknownOutcome was falsely reported or blindly retried");
}

void TestOperationRecordTransitionsAndReceiptCorrelation() {
  const auto Identity = std::get<Core::ResolvedIdentity>(
      Core::IdentityPolicy{7, {MakeBinding()}}.Resolve(MakeEnvelope()));
  Core::OperationRecord Record{{"OperationState"}, {"KeyState"}, {"TurnState"},
      {"ActionState"}, Identity.Scope, 7, 5};
  Check(Record.GetState() == Core::OperationState::Prepared && Record.Authorize() &&
            !Record.Authorize() && Record.MarkDispatched() && !Record.MarkDispatched(),
        "OperationRecord accepted an illegal lifecycle transition");
  Check(Record.GetOperationId().Value == "OperationState" &&
            Record.GetIdempotencyKey().Value == "KeyState" &&
            Record.GetAgentTurnId().Value == "TurnState" &&
            Record.GetActionId().Value == "ActionState" &&
            Record.GetScope().Principal.Value == "PrincipalA" &&
            Record.GetPolicyVersion() == 7 &&
            Record.GetCapabilitySnapshotVersion() == 5 &&
            !Record.GetErrorCode().has_value(),
        "OperationRecord did not preserve its scoped identifiers and version pins");
  Check(!Record.FailBeforeDispatch(Channels::ChannelErrorCode::TransportFailure), "dispatched OperationRecord accepted a pre-dispatch transition");
  const Channels::ChannelReceipt Mismatch{{"OtherAction"}, {"OperationState"}, std::nullopt,
      std::chrono::system_clock::now(), Channels::ReceiptStatus::Succeeded,
      Channels::ChannelErrorCode::None, std::nullopt};
  Check(!Record.ApplyReceipt(Mismatch) && Record.GetState() == Core::OperationState::UnknownOutcome &&
            Record.GetErrorCode() == Channels::ChannelErrorCode::OutcomeUnknown &&
            !Record.ApplyReceipt(Mismatch) &&
            !Record.MarkUnknownOutcome(Channels::ChannelErrorCode::OutcomeUnknown),
        "mismatched receipt was accepted or replayed by OperationRecord");

  Core::OperationRecord UnknownDefaultError{{"OperationUnknownDefault"}, {"KeyUnknownDefault"},
      {"TurnUnknownDefault"}, {"ActionUnknownDefault"}, Identity.Scope, 7, 5};
  Check(UnknownDefaultError.Authorize() && UnknownDefaultError.MarkDispatched() &&
            UnknownDefaultError.MarkUnknownOutcome(Channels::ChannelErrorCode::None) &&
            UnknownDefaultError.GetErrorCode() == Channels::ChannelErrorCode::OutcomeUnknown,
        "UnknownOutcome did not receive its stable fallback error code");
  Core::OperationRecord UnknownReceiptRecord{{"OperationUnknownReceipt"}, {"KeyUnknownReceipt"},
      {"TurnUnknownReceipt"}, {"ActionUnknownReceipt"}, Identity.Scope, 7, 5};
  const Channels::ChannelReceipt UnknownReceipt{{"ActionUnknownReceipt"},
      {"OperationUnknownReceipt"}, std::nullopt, std::chrono::system_clock::now(),
      Channels::ReceiptStatus::UnknownOutcome, Channels::ChannelErrorCode::None, std::nullopt};
  Check(UnknownReceiptRecord.Authorize() && UnknownReceiptRecord.MarkDispatched() &&
            UnknownReceiptRecord.ApplyReceipt(UnknownReceipt) &&
            UnknownReceiptRecord.GetState() == Core::OperationState::UnknownOutcome &&
            UnknownReceiptRecord.GetErrorCode() == Channels::ChannelErrorCode::OutcomeUnknown,
        "UnknownOutcome receipt did not receive a stable fallback error code");
  Core::OperationRecord PreDispatch{{"OperationPre"}, {"KeyPre"}, {"TurnPre"},
      {"ActionPre"}, Identity.Scope, 7, 5};
  Check(PreDispatch.FailBeforeDispatch(Channels::ChannelErrorCode::UnsupportedCapability) &&
            PreDispatch.GetState() == Core::OperationState::Failed && !PreDispatch.MarkDispatched() &&
            PreDispatch.GetErrorCode() == Channels::ChannelErrorCode::UnsupportedCapability &&
            !PreDispatch.MarkUnknownOutcome(Channels::ChannelErrorCode::OutcomeUnknown),
        "pre-dispatch failure did not prevent dispatch or preserve its error");

  bool InvalidRecordRejected = false;
  try {
    const Core::OperationRecord Invalid{{""}, {"KeyInvalid"}, {"TurnInvalid"},
        {"ActionInvalid"}, Identity.Scope, 7, 5};
    (void)Invalid;
  } catch (const std::invalid_argument&) {
    InvalidRecordRejected = true;
  }
  Check(InvalidRecordRejected, "OperationRecord accepted an empty operation ID");
}

void TestAgentOrChannelExceptionsFailClosed() {
  {
    Fixture FixtureValue;
    FixtureValue.Agent.ThrowOnRun = true;
    FixtureValue.Identifiers.Push(MakeIds("AgentException"));
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupAgentException"));
    Check(Result.Status == Core::CoreChatStatus::Failed &&
              Result.AgentFailure == Core::AgentRunFailureCode::InternalError &&
              FixtureValue.Identifiers.Calls == 0 && FixtureValue.Channel.SendCalls == 0,
          "AgentRuntime exception failed to stop dispatch");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Identifiers.Push(MakeIds("ChannelException"));
    FixtureValue.Channel.ThrowOnSend = true;
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupChannelException"));
    Check(Result.Status == Core::CoreChatStatus::UnknownOutcome &&
              Result.Operation == Core::OperationState::UnknownOutcome &&
              FixtureValue.Channel.SendCalls == 1,
          "post-dispatch exception did not become UnknownOutcome");
  }
}

} // namespace

void TestDuplicateBindingsAreRejectedAtPolicyConstruction() {
  bool Rejected = false;
  try {
    const Core::IdentityPolicy Policy{7, {MakeBinding(), MakeBinding()}};
    (void)Policy;
  } catch (const std::invalid_argument&) {
    Rejected = true;
  }
  Check(Rejected, "duplicate identity binding was accepted");
}

void TestZeroPolicyVersionIsRejected() {
  bool Rejected = false;
  try {
    const Core::IdentityPolicy Policy{0, {MakeBinding()}};
    (void)Policy;
  } catch (const std::invalid_argument&) {
    Rejected = true;
  }
  Check(Rejected, "zero policy version was accepted");
}
void TestReceiptStatusesMapWithoutFalseSuccess() {
  const std::array<std::pair<Channels::ReceiptStatus, Core::CoreChatStatus>, 7> Cases{{
      {Channels::ReceiptStatus::Rejected, Core::CoreChatStatus::Rejected},
      {Channels::ReceiptStatus::Forbidden, Core::CoreChatStatus::Rejected},
      {Channels::ReceiptStatus::UnsupportedCapability, Core::CoreChatStatus::Unsupported},
      {Channels::ReceiptStatus::RateLimited, Core::CoreChatStatus::Failed},
      {Channels::ReceiptStatus::Failed, Core::CoreChatStatus::Failed},
      {Channels::ReceiptStatus::Succeeded, Core::CoreChatStatus::Completed},
      {Channels::ReceiptStatus::UnknownOutcome, Core::CoreChatStatus::UnknownOutcome}}};
  std::size_t Index = 0;
  for (const auto& Case : Cases) {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "answer"));
    FixtureValue.Identifiers.Push(MakeIds("Receipt" + std::to_string(Index++)));
    FixtureValue.Channel.Receipt = [Status = Case.first](
        const Core::CoreAuthorizedAction& Action) {
      const Channels::ChannelErrorCode Error =
          Status == Channels::ReceiptStatus::Succeeded
              ? Channels::ChannelErrorCode::None
              : (Status == Channels::ReceiptStatus::UnknownOutcome
                     ? Channels::ChannelErrorCode::OutcomeUnknown
                     : Channels::ChannelErrorCode::PlatformRejected);
      return Channels::ChannelReceipt{Action.GetActionId(), Action.GetOperationId(),
          std::nullopt, std::chrono::system_clock::now(), Status, Error, std::nullopt};
    };
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupReceipt" + std::to_string(Index)));
    Check(Result.Status == Case.second && FixtureValue.Channel.SendCalls == 1,
          "receipt status was incorrectly reported as success");
  }
}
void TestCapabilitySnapshotsFailClosed() {
  const std::vector<Channels::CapabilitySnapshot> BadSnapshots = [&] {
    std::vector<Channels::CapabilitySnapshot> Values;
    auto WrongAccount = MakeCapabilities();
    WrongAccount.Account.Value = "OtherAccount";
    Values.push_back(std::move(WrongAccount));
    auto BadAuth = MakeCapabilities();
    BadAuth.AuthenticationStatus = Channels::VerificationStatus::Rejected;
    Values.push_back(std::move(BadAuth));
    auto BadAudit = MakeCapabilities();
    BadAudit.AuditStatus = Channels::VerificationStatus::Pending;
    Values.push_back(std::move(BadAudit));
    auto Expired = MakeCapabilities();
    Expired.ValidUntil = std::chrono::system_clock::now() - std::chrono::seconds{1};
    Values.push_back(std::move(Expired));
    auto DuplicateReceive = MakeCapabilities();
    DuplicateReceive.Capabilities.push_back(DuplicateReceive.Capabilities.front());
    Values.push_back(std::move(DuplicateReceive));
    return Values;
  }();
  std::size_t Index = 0;
  for (const Channels::CapabilitySnapshot& Snapshot : BadSnapshots) {
    Fixture FixtureValue;
    FixtureValue.Channel.SetSnapshots({Snapshot});
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "must not run"));
    FixtureValue.Identifiers.Push(MakeIds("BadCapability" + std::to_string(Index++)));
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupBadCapability" + std::to_string(Index)));
    Check(Result.Status == Core::CoreChatStatus::Unsupported &&
              FixtureValue.Agent.Calls == 0 && FixtureValue.Identifiers.Calls == 0 &&
              FixtureValue.Channel.SendCalls == 0,
          "invalid/expired/unverified capability snapshot was accepted");
  }
}

void TestCapabilityQueryExceptionsFailClosed() {
  {
    Fixture FixtureValue;
    FixtureValue.Channel.ThrowOnCapabilityCall = 1;
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupInitialCapabilityError"));
    Check(Result.Status == Core::CoreChatStatus::Failed && FixtureValue.Agent.Calls == 0 &&
              FixtureValue.Identifiers.Calls == 0 && FixtureValue.Channel.SendCalls == 0,
          "initial capability query exception did not fail closed");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Channel.ThrowOnCapabilityCall = 2;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "reply"));
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupSendCapabilityError"));
    Check(Result.Status == Core::CoreChatStatus::Failed && FixtureValue.Agent.Calls == 1 &&
              FixtureValue.Identifiers.Calls == 0 && FixtureValue.Channel.SendCalls == 0,
          "send capability query exception allocated identifiers");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Channel.ThrowOnCapabilityCall = 3;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Identifiers.Push(MakeIds("DispatchCapabilityError"));
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupDispatchCapabilityError"));
    Check(Result.Status == Core::CoreChatStatus::Failed &&
              Result.Operation == Core::OperationState::Failed &&
              FixtureValue.Channel.SendCalls == 0 &&
              FixtureValue.Probe.LastState() == Core::OperationState::Failed,
          "dispatch-time capability query exception did not fail the authorized operation");
  }
}

void TestCancellationPreventsRuntimeOrActionIds() {
  {
    Fixture FixtureValue;
    FixtureValue.Identifiers.Push(MakeIds("CancelledBefore"));
    std::stop_source StopSource;
    StopSource.request_stop();
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupCancelledBefore"),
                                                       StopSource.get_token());
    Check(Result.Status == Core::CoreChatStatus::Cancelled && FixtureValue.Agent.Calls == 0 &&
              FixtureValue.Identifiers.Calls == 0 && FixtureValue.Channel.SendCalls == 0,
          "pre-run cancellation reached runtime or ID source");
  }
  {
    Fixture FixtureValue;
    FixtureValue.Agent.Push(Core::AgentRunResult::ResponseReady(
        Providers::CompletionKind::FinalAnswer, "reply"));
    FixtureValue.Agent.OnRun = [&FixtureValue] {};
    std::stop_source StopSource;
    FixtureValue.Agent.OnRun = [&StopSource] { StopSource.request_stop(); };
    FixtureValue.Identifiers.Push(MakeIds("CancelledAfter"));
    const auto Result = FixtureValue.Service.Process(MakeMessage("DedupCancelledAfter"),
                                                       StopSource.get_token());
    Check(Result.Status == Core::CoreChatStatus::Cancelled && FixtureValue.Agent.Calls == 1 &&
              FixtureValue.Identifiers.Calls == 0 && FixtureValue.Channel.SendCalls == 0,
          "post-runtime cancellation allocated IDs or dispatched");
  }
}
int main() {
  const std::array<std::pair<std::string_view, void (*)()>, 19> Tests{{
      {"AC-1 trusted envelope mapping", TestAC1TrustedEnvelopeAndExactScope},
      {"AC-1 unknown account/route/malformed envelope", TestAC1UnknownAccountRouteMismatchAndMalformedEnvelopeFailClosed},
      {"AC-2 denial, dedup, unsupported", TestAC2DeniedDuplicateAndUnsupportedInputsDoNotRunOrSend},
      {"AC-3 result and failure mapping", TestAC3RunResultAndFailureMappingFailClosed},
      {"AC-4 record-before-send and receipt", TestAC4RecordBeforeDispatchAndExactReceipt},
      {"AC-5 version and capability checks", TestAC5CapabilityVersionsAndExactScopeAreEnforced},
      {"AC-7 Next count and ID validation", TestAC7NextCallCountsAndTupleRejection},
      {"AC-7 denied paths do not call Next", TestAC7DeniedPathsDoNotCallNext},
      {"capability version revalidation", TestCapabilityVersionChangeFailsBeforeDispatch},
      {"invalid capability snapshots fail closed", TestCapabilitySnapshotsFailClosed},
      {"capability query exceptions fail closed", TestCapabilityQueryExceptionsFailClosed},
      {"cancellation prevents ID allocation", TestCancellationPreventsRuntimeOrActionIds},
      {"unknown outcome no retry", TestUnknownOutcomeReceiptIsNeverRetried},
      {"OperationRecord state machine", TestOperationRecordTransitionsAndReceiptCorrelation},
      {"Agent and Channel exception fail-closed", TestAgentOrChannelExceptionsFailClosed},
      {"Agent failures and non-ready states", TestAgentFailuresCancelledUnsupportedAndUnknownNeverAllocateIds},
      {"AC-1 policy duplicate binding", TestDuplicateBindingsAreRejectedAtPolicyConstruction},
      {"AC-1 zero policy version", TestZeroPolicyVersionIsRejected},
      {"AC-7 receipt outcomes", TestReceiptStatusesMapWithoutFalseSuccess},
  }};
  std::size_t Failures = 0;
  for (const auto& Test : Tests) {
    try {
      Test.second();
      std::cout << "PASS " << Test.first << '\n';
    } catch (const std::exception& Error) {
      ++Failures;
      std::cerr << "FAIL " << Test.first << ": " << Error.what() << '\n';
    }
  }
  if (Failures != 0) {
    std::cerr << Failures << " contract test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All Core policy/action contract tests passed\n";
  return EXIT_SUCCESS;
}
