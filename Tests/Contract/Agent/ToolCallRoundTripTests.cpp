#include "Source/Agent/AgentEventProjector.hpp"
#include "Source/Agent/OutputBudgetController.hpp"
#include "Source/Agent/ToolCallCoordinator.hpp"
#include "Source/Core/BusinessActionRegistry.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace EmulativeDialog;

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

Core::ConversationScope MakeScope(
    std::string PrincipalValue = "principal-authorized",
    std::string ChannelValue = "Mock",
    std::string ConversationValue = "conversation-1") {
  return Core::ConversationScope{
      .Principal = Core::PrincipalId{std::move(PrincipalValue)},
      .Channel = Channels::ChannelId{std::move(ChannelValue)},
      .Account = Channels::AccountId{"account-1"},
      .Mode = Channels::IntegrationMode{"Mock"},
      .Conversation =
          Channels::ConversationId{std::move(ConversationValue)},
      .ConversationType = Channels::ConversationKind::Private,
      .Sender = Channels::SenderId{"sender-1"},
  };
}

Agent::ToolCallAuthorizationContext MakeAuthorization(
    std::vector<std::string> Capabilities = {"Chat.Lookup"}) {
  return Agent::ToolCallAuthorizationContext{
      .Principal = Core::PrincipalId{"principal-authorized"},
      .Scope = MakeScope(),
      .ActiveTurn = Providers::AgentTurnId{"turn-1"},
      .AuthorizedRequest = Providers::RequestId{"request-1"},
      .GrantedCapabilities = std::move(Capabilities),
  };
}

Core::BusinessActionDefinition MakeLookupAction(
    std::function<Core::BusinessActionExecutionResult(
        const Core::BusinessActionInvocation&)>
        Handler,
    std::string Name = "LookupProfile") {
  return Core::BusinessActionDefinition{
      .Name = std::move(Name),
      .RequiredArgumentFields = {"query"},
      .RequiredCapabilities = {"Chat.Lookup"},
      .Scope =
          Core::BusinessActionScopeRequirement{
              .ChannelId = std::string{"Mock"},
              .ConversationId = std::string{"conversation-1"},
          },
      .Handler = std::move(Handler),
  };
}

Core::BusinessActionExecutionResult Succeeded(
    std::string Code = "ProfileFound") {
  return Core::BusinessActionExecutionResult{
      .Status = Core::BusinessActionExecutionStatus::Succeeded,
      .SanitizedCode = std::move(Code),
  };
}

Agent::AgentToolCallReadyEvent ReadyCall(
    std::string CallId,
    std::string ActionName,
    std::string Arguments,
    std::uint64_t Sequence = 1,
    std::string Turn = "turn-1") {
  return Agent::AgentToolCallReadyEvent{
      .Metadata =
          Agent::AgentEventMetadata{
              .Request = Providers::RequestId{"request-1"},
              .Turn = Providers::AgentTurnId{std::move(Turn)},
              .SequenceNumber = Sequence,
          },
      .Item = Providers::ItemId{"item-tool"},
      .Call = Providers::ToolCallId{std::move(CallId)},
      .ToolName = std::move(ActionName),
      .Arguments = Providers::JsonValue{std::move(Arguments)},
  };
}

Agent::AgentToolHandoffEvent Handoff(
    std::uint64_t Sequence = 2, std::string Turn = "turn-1",
    std::string Request = "request-1") {
  return Agent::AgentToolHandoffEvent{
      .Metadata =
          Agent::AgentEventMetadata{
              .Request = Providers::RequestId{std::move(Request)},
              .Turn = Providers::AgentTurnId{std::move(Turn)},
              .SequenceNumber = Sequence,
          },
  };
}

Agent::AgentEventMetadata Metadata(std::uint64_t Sequence) {
  return Agent::AgentEventMetadata{
      .Request = Providers::RequestId{"request-1"},
      .Turn = Providers::AgentTurnId{"turn-1"},
      .SequenceNumber = Sequence,
  };
}

struct ReceiptCollector final {
  std::vector<Agent::ToolCallReceipt> Receipts;

  Agent::ToolCallCoordinator::ReceiptSink Sink() {
    return [this](const Agent::ToolCallReceipt& Receipt) {
      Receipts.push_back(Receipt);
    };
  }
};

class FinalUsageBudgetScope final {
public:
  FinalUsageBudgetScope()
      : Controller(Agent::OutputBudgetController::Shared()) {
    Check(Controller.BeginTurn(Turn, 64),
          "tool round-trip fixture must establish a Core turn budget");
    Check(Controller.BeginRequest(Turn, Request) ==
              Agent::OutputBudgetRequestResult::Accepted,
          "tool round-trip fixture must bind its provider request");
    Check(Controller.CompleteRequest(
              Turn, Request,
              Providers::UsageSnapshot{
                  .InputTokens = std::nullopt,
                  .OutputTokens = 8,
                  .TotalTokens = std::nullopt,
                  .CachedInputReadTokens = std::nullopt,
                  .CachedInputWriteTokens = std::nullopt,
                  .ReasoningOutputTokens = std::nullopt,
                  .Completeness = Providers::UsageCompleteness::Final},
              false, true),
          "tool round-trip fixture must provide final authoritative usage");
  }

  ~FinalUsageBudgetScope() { Controller.EndTurn(Turn); }

  FinalUsageBudgetScope(const FinalUsageBudgetScope&) = delete;
  FinalUsageBudgetScope& operator=(const FinalUsageBudgetScope&) = delete;

private:
  Agent::OutputBudgetController& Controller;
  const Providers::AgentTurnId Turn{"turn-1"};
  const Providers::RequestId Request{"request-1"};
};

void RegistryRejectsUnsafeOrInvalidDefinitions() {
  Core::BusinessActionRegistry Registry;
  const auto Handler = [](const Core::BusinessActionInvocation&) {
    return Succeeded();
  };

  Check(!Registry.Register(MakeLookupAction(Handler)).has_value(),
        "valid business action must register");
  Check(Registry.Count() == 1U, "registry must retain the action");
  Check(Registry.Find("LookupProfile") != nullptr,
        "registered action must be findable");
  Check(Registry.Find("MissingAction") == nullptr,
        "unknown action must not resolve");

  Check(Registry.Register(MakeLookupAction(Handler)).value().Code ==
            Core::BusinessActionRegistrationErrorCode::DuplicateAction,
        "duplicate action registration must fail");

  Core::BusinessActionRegistry ForbiddenRegistry;
  Check(ForbiddenRegistry.Register(MakeLookupAction(Handler, "Shell"))
                .value()
                .Code ==
            Core::BusinessActionRegistrationErrorCode::ForbiddenActionName,
        "generic Shell tool must be forbidden");
  Check(ForbiddenRegistry.Register(MakeLookupAction(Handler, "ReadFile"))
                .value()
                .Code ==
            Core::BusinessActionRegistrationErrorCode::ForbiddenActionName,
        "generic file tool must be forbidden");
  Check(ForbiddenRegistry.Register(MakeLookupAction(Handler, "lookup"))
                .value()
                .Code ==
            Core::BusinessActionRegistrationErrorCode::InvalidActionName,
        "lowercase action names must be rejected");

  Core::BusinessActionDefinition EmptyHandler =
      MakeLookupAction(Handler, "NoHandler");
  EmptyHandler.Handler = {};
  Check(ForbiddenRegistry.Register(EmptyHandler).value().Code ==
            Core::BusinessActionRegistrationErrorCode::EmptyHandler,
        "actions without handlers must be rejected");

  Core::BusinessActionDefinition DuplicateFields =
      MakeLookupAction(Handler, "DuplicateFields");
  DuplicateFields.RequiredArgumentFields = {"query", "query"};
  Check(ForbiddenRegistry.Register(DuplicateFields).value().Code ==
            Core::BusinessActionRegistrationErrorCode::InvalidSchemaField,
        "duplicate schema fields must be rejected");

  Core::BusinessActionDefinition BadField =
      MakeLookupAction(Handler, "BadField");
  BadField.RequiredArgumentFields = {"bad field"};
  Check(ForbiddenRegistry.Register(BadField).value().Code ==
            Core::BusinessActionRegistrationErrorCode::InvalidSchemaField,
        "invalid schema field names must be rejected");
}

void ArgumentValidationAcceptsOnlyStrictObjects() {
  const std::vector<std::string> Required = {"query", "limit"};
  Check(Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"x","limit":1})", Required),
        "valid object with required fields must pass");
  Check(Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":{"inner":[1,true,null,"x\u0041"]},"limit":2e3})",
            Required),
        "nested values and escapes must pass");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"x"})", Required),
        "missing required fields must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments("[1,2,3]",
                                                         Required),
        "non-object arguments must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":})", Required),
        "malformed JSON must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"a","query":"b","limit":1})", Required),
        "duplicate top-level keys must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"x","limit":1} trailing)", Required),
        "trailing garbage must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"x","limit":01})", Required),
        "invalid JSON numbers must fail");

  Check(Core::BusinessActionRegistry::ValidateArguments("{}", {}),
        "empty objects may satisfy empty schemas");
  Check(Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":false,"limit":null})", Required),
        "boolean and null values must parse");
  Check(Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":-1.5e+2,"limit":-0.5})", Required),
        "signed and exponent numbers must parse");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"x","limit":1,})", Required),
        "trailing commas must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":[1,],"limit":1})", Required),
        "array trailing commas must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            std::string{"{\"query\":\"bad\ncontrol\",\"limit\":1}"},
            Required),
        "raw control characters in strings must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"bad\q","limit":1})", Required),
        "unknown string escapes must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"\u12G4","limit":1})", Required),
        "invalid unicode escapes must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":"truncated)", Required),
        "unterminated strings must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":-,"limit":1})", Required),
        "bare minus numbers must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":1.,"limit":1})", Required),
        "numbers without fraction digits must fail");
  Check(!Core::BusinessActionRegistry::ValidateArguments(
            R"({"query":1e,"limit":1})", Required),
        "numbers without exponent digits must fail");
  std::string DeeplyNested = R"({"query":)";
  DeeplyNested.append(40U, '[');
  DeeplyNested.append("1");
  DeeplyNested.append(40U, ']');
  DeeplyNested.append(R"(,"limit":1})");
  Check(!Core::BusinessActionRegistry::ValidateArguments(DeeplyNested,
                                                         Required),
        "excessive nesting depth must fail");
}

void FullRoundTripThroughProjectorExecutesOnce() {
  FinalUsageBudgetScope Budget;
  int HandlerCalls = 0;
  std::vector<Core::BusinessActionInvocation> Invocations;
  Core::BusinessActionRegistry Registry;
  Check(!Registry.Register(MakeLookupAction(
                [&HandlerCalls, &Invocations](
                    const Core::BusinessActionInvocation& Invocation) {
                  ++HandlerCalls;
                  Invocations.push_back(Invocation);
                  return Succeeded();
                }))
             .has_value(),
        "lookup action must register");

  ReceiptCollector Collected;
  Agent::ToolCallCoordinator Coordinator(
      Registry, MakeAuthorization(), Collected.Sink());
  std::optional<Agent::ToolCoordinatorErrorCode> ProjectionError;
  Agent::AgentEventProjector Projector(
      Providers::RequestId{"request-1"},
      Providers::AgentTurnId{"turn-1"},
      [&Coordinator, &ProjectionError](const Agent::AgentLifecycleEvent& E) {
        if (const auto Error = Coordinator.Accept(E); Error.has_value()) {
          ProjectionError = Error->Code;
        }
      });

  const auto CanonicalMeta = [](std::uint64_t Sequence) {
    return Providers::ProviderEventMetadata{
        .Request = Providers::RequestId{"request-1"},
        .TurnId = Providers::AgentTurnId{"turn-1"},
        .SequenceNumber = Sequence,
    };
  };

  Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                Providers::ToolCallStartedEvent{
                    .Metadata = CanonicalMeta(1),
                    .Item = Providers::ItemId{"item-tool"},
                    .Call = Providers::ToolCallId{"call-1"},
                    .ToolName = "LookupProfile"}})
             .has_value(),
        "tool start must project");
  Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                Providers::ToolArgumentDeltaEvent{
                    .Metadata = CanonicalMeta(2),
                    .Item = Providers::ItemId{"item-tool"},
                    .Call = Providers::ToolCallId{"call-1"},
                    .JsonTextDelta = R"({"query":"alice"})"}})
             .has_value(),
        "tool arguments must project");
  Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                Providers::ToolCallReadyEvent{
                    .Metadata = CanonicalMeta(3),
                    .Item = Providers::ItemId{"item-tool"},
                    .Call = Providers::ToolCallId{"call-1"},
                    .ToolName = "LookupProfile",
                    .Arguments =
                        Providers::JsonValue{R"({"query":"alice"})"}}})
             .has_value(),
        "tool ready must project");
  Check(HandlerCalls == 0,
        "tool must not execute before the terminal handoff");
  Check(!Projector.Accept(Providers::CanonicalTerminalEvent{
                Providers::ResponseCompletedEvent{
                    .Metadata = CanonicalMeta(4),
                    .Kind = Providers::CompletionKind::ToolHandoff,
                    .StopSequenceCode = std::nullopt,
                    .FinalUsage = std::nullopt,
                    .StopDetails = std::nullopt}})
             .has_value(),
        "tool handoff terminal must project");

  Check(!ProjectionError.has_value(), "coordinator must accept lifecycle events");
  Check(HandlerCalls == 1, "business action must execute exactly once");
  Check(Invocations.size() == 1U &&
            Invocations[0].Name == "LookupProfile" &&
            Invocations[0].Principal.Value == "principal-authorized" &&
            Invocations[0].Turn.Value == "turn-1",
        "invocation must carry Core authorization context");
  Check(Collected.Receipts.size() == 1U &&
            Collected.Receipts[0].ReceiptId == "tool-receipt-1" &&
            Collected.Receipts[0].ToolCallId == "call-1" &&
            Collected.Receipts[0].Status ==
                Agent::ToolCallReceiptStatus::Succeeded &&
            Collected.Receipts[0].ResultCode == "ProfileFound",
        "successful action must emit a correlated receipt");
  Check(Coordinator.HandlerExecutionCount() == 1U &&
            Coordinator.ReceiptCount() == 1U &&
            Coordinator.HasTerminal(),
        "coordinator state must reflect one terminal and one execution");
}

void NonHandoffTerminalsDiscardPendingCalls() {
  FinalUsageBudgetScope Budget;
  const std::vector<std::pair<const char*, Agent::AgentLifecycleEvent>>
      Cases = {
          {"final",
           Agent::AgentLifecycleEvent{
               Agent::AgentFinalEvent{.Metadata = Metadata(2)}}},
          {"refusal",
           Agent::AgentLifecycleEvent{
               Agent::AgentRefusalEvent{.Metadata = Metadata(2),
                                        .Category = std::nullopt}}},
          {"output-limited",
           Agent::AgentLifecycleEvent{
               Agent::AgentOutputLimitedEvent{
                   .Metadata = Metadata(2),
                   .Kind = Providers::LimitKind::OutputTokens,
                   .Usage = Providers::UsageSnapshot{},
                   .HasUnsendablePartialText = false}}},
          {"provider-failed",
           Agent::AgentLifecycleEvent{
               Agent::AgentFailedEvent{
                   .Metadata = Metadata(2),
                   .Code = Providers::FailureCode::ProviderRejected,
                   .Retryable = false}}},
          {"cancelled",
           Agent::AgentLifecycleEvent{
               Agent::AgentCancelledEvent{.Metadata = Metadata(2),
                                          .ReasonCode = "Cancelled"}}},
          {"outcome-unknown",
           Agent::AgentLifecycleEvent{
               Agent::AgentOutcomeUnknownEvent{
                   .Metadata = Metadata(2),
                   .ReasonCode = "OutcomeUnknown"}}},
      };

  for (const auto& [Name, Terminal] : Cases) {
    int HandlerCalls = 0;
    Core::BusinessActionRegistry Registry;
    Check(!Registry.Register(MakeLookupAction(
                  [&HandlerCalls](const Core::BusinessActionInvocation&) {
                    ++HandlerCalls;
                    return Succeeded();
                  }))
               .has_value(),
          "action must register");
    ReceiptCollector Collected;
    Agent::ToolCallCoordinator Coordinator(
        Registry, MakeAuthorization(), Collected.Sink());
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{ReadyCall(
                  "call-terminal", "LookupProfile", R"({"query":"x"})")})
               .has_value(),
          "ready call must buffer before terminal");
    Check(Coordinator.PendingCount() == 1U && HandlerCalls == 0,
          "buffered calls must not execute early");
    Check(!Coordinator.Accept(Terminal).has_value(),
          "non-handoff terminal must be accepted");
    Check(Coordinator.PendingCount() == 0U && Coordinator.HasTerminal(),
          "non-handoff terminal must clear pending calls");
    Check(HandlerCalls == 0 && Collected.Receipts.empty() &&
              Coordinator.HandlerExecutionCount() == 0U &&
              Coordinator.ReceiptCount() == 0U,
          "non-handoff terminal must not dispatch or emit receipts");
    const auto AfterTerminal =
        Coordinator.Accept(Agent::AgentLifecycleEvent{
            Agent::AgentStartedEvent{.Metadata = Metadata(3),
                                     .Response = std::nullopt}});
    Check(AfterTerminal.has_value() &&
              AfterTerminal->Code ==
                  Agent::ToolCoordinatorErrorCode::EventAfterTerminal &&
              HandlerCalls == 0 &&
              Coordinator.HandlerExecutionCount() == 0U &&
              Coordinator.ReceiptCount() == 0U,
          "events after each non-handoff terminal must fail closed");
    if (HandlerCalls != 0 || !Collected.Receipts.empty()) {
      std::cerr << "terminal case: " << Name << '\n';
    }
  }
}

void HandoffCorrelationIsAuthoritative() {
  FinalUsageBudgetScope Budget;
  int HandlerCalls = 0;
  Core::BusinessActionRegistry Registry;
  Check(!Registry.Register(MakeLookupAction(
                [&HandlerCalls](const Core::BusinessActionInvocation&) {
                  ++HandlerCalls;
                  return Succeeded();
                }))
             .has_value(),
        "action must register");
  ReceiptCollector Collected;
  Agent::ToolCallCoordinator Coordinator(
      Registry, MakeAuthorization(), Collected.Sink());
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{ReadyCall(
                "call-handoff", "LookupProfile", R"({"query":"x"})")})
             .has_value() &&
            Coordinator.PendingCount() == 1U,
        "authorized ready call must be buffered");

  const auto ForeignTurn =
      Coordinator.Accept(Agent::AgentLifecycleEvent{
          Handoff(2, "foreign-turn", "request-1")});
  Check(ForeignTurn.has_value() &&
            ForeignTurn->Code ==
                Agent::ToolCoordinatorErrorCode::ForeignHandoff &&
            Coordinator.PendingCount() == 1U &&
            !Coordinator.HasTerminal() && HandlerCalls == 0 &&
            Collected.Receipts.empty(),
        "foreign-turn handoff must preserve pending state");

  const auto ForeignRequest =
      Coordinator.Accept(Agent::AgentLifecycleEvent{
          Handoff(3, "turn-1", "foreign-request")});
  Check(ForeignRequest.has_value() &&
            ForeignRequest->Code ==
                Agent::ToolCoordinatorErrorCode::RequestMismatch &&
            Coordinator.PendingCount() == 1U &&
            !Coordinator.HasTerminal() && HandlerCalls == 0 &&
            Collected.Receipts.empty(),
        "foreign-request handoff must preserve pending state");

  const auto MissingRequest =
      Coordinator.Accept(Agent::AgentLifecycleEvent{
          Handoff(4, "turn-1", "")});
  Check(MissingRequest.has_value() &&
            MissingRequest->Code ==
                Agent::ToolCoordinatorErrorCode::RequestMismatch &&
            Coordinator.PendingCount() == 1U &&
            !Coordinator.HasTerminal() && HandlerCalls == 0,
        "missing handoff request must preserve pending state");

  auto UnconfiguredAuthorization = MakeAuthorization();
  UnconfiguredAuthorization.AuthorizedRequest = {};
  Agent::ToolCallCoordinator Unconfigured(
      Registry, std::move(UnconfiguredAuthorization), Collected.Sink());
  const auto MissingAuthorization =
      Unconfigured.Accept(Agent::AgentLifecycleEvent{Handoff()});
  Check(MissingAuthorization.has_value() &&
            MissingAuthorization->Code ==
                Agent::ToolCoordinatorErrorCode::RequestMismatch &&
            !Unconfigured.HasTerminal(),
        "missing authorized request must fail closed");

  Check(!Coordinator.Accept(
                Agent::AgentLifecycleEvent{Handoff(5, "turn-1", "request-1")})
             .has_value(),
        "matching handoff must be accepted after rejected foreign terminals");
  Check(Coordinator.PendingCount() == 0U && Coordinator.HasTerminal() &&
            HandlerCalls == 1 && Collected.Receipts.size() == 1U,
        "matching handoff must execute the preserved pending call once");
}

void BusinessValidationRejectsBeforeDispatch() {
  FinalUsageBudgetScope Budget;
  const auto RunRejected = [](
      Core::BusinessActionDefinition Definition,
      Agent::ToolCallAuthorizationContext Authorization,
      Agent::AgentToolCallReadyEvent Ready, const char* ExpectedCode) {
    Core::BusinessActionRegistry Registry;
    Check(!Registry.Register(std::move(Definition)).has_value(),
          "action must register");
    ReceiptCollector Collected;
    Agent::ToolCallCoordinator Coordinator(
        Registry, std::move(Authorization), Collected.Sink());
    Check(!Coordinator.Accept(
                 Agent::AgentLifecycleEvent{std::move(Ready)})
               .has_value(),
          "ready call must be buffered");
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff()})
               .has_value(),
          "handoff terminal must be accepted");
    Check(Collected.Receipts.size() == 1U &&
              Collected.Receipts[0].Status ==
                  Agent::ToolCallReceiptStatus::Rejected &&
              Collected.Receipts[0].ResultCode == ExpectedCode,
          "invalid call must produce the expected rejection receipt");
    Check(Coordinator.HandlerExecutionCount() == 0,
          "rejected calls must never execute");
  };

  int Calls = 0;
  const auto Handler = [&Calls](const Core::BusinessActionInvocation&) {
    ++Calls;
    return Succeeded();
  };

  RunRejected(MakeLookupAction(Handler), MakeAuthorization({}),
              ReadyCall("call-cap", "LookupProfile", R"({"query":"x"})"),
              "MissingCapability");

  Core::BusinessActionDefinition WrongChannel =
      MakeLookupAction(Handler);
  WrongChannel.Scope.ChannelId = std::string{"OtherChannel"};
  RunRejected(WrongChannel, MakeAuthorization(),
              ReadyCall("call-scope", "LookupProfile", R"({"query":"x"})"),
              "ScopeMismatch");

  auto WrongPrincipalAuthorization = MakeAuthorization();
  WrongPrincipalAuthorization.Scope.Principal =
      Core::PrincipalId{"other-principal"};
  RunRejected(MakeLookupAction(Handler), WrongPrincipalAuthorization,
              ReadyCall("call-principal", "LookupProfile",
                        R"({"query":"x"})"),
              "ScopeMismatch");

  RunRejected(MakeLookupAction(Handler), MakeAuthorization(),
              ReadyCall("call-unknown", "UnknownAction",
                        R"({"query":"x"})"),
              "UnknownAction");

  RunRejected(MakeLookupAction(Handler), MakeAuthorization(),
              ReadyCall("call-invalid", "LookupProfile", R"({"limit":1})"),
              "InvalidArguments");

  RunRejected(MakeLookupAction(Handler), MakeAuthorization(),
              ReadyCall("call-malformed", "LookupProfile", "{\"query\":"),
              "InvalidArguments");
}

void ReplaysAndDuplicateCallsExecuteAtMostOnce() {
  FinalUsageBudgetScope Budget;
  int HandlerCalls = 0;
  Core::BusinessActionRegistry Registry;
  Check(!Registry.Register(MakeLookupAction(
                [&HandlerCalls](const Core::BusinessActionInvocation&) {
                  ++HandlerCalls;
                  return Succeeded();
                }))
             .has_value(),
        "action must register");
  ReceiptCollector Collected;
  Agent::ToolCallCoordinator Coordinator(
      Registry, MakeAuthorization(), Collected.Sink());
  const auto Ready =
      ReadyCall("call-replay", "LookupProfile", R"({"query":"x"})");
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Ready}).has_value(),
        "first ready must buffer");
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Ready}).has_value(),
        "duplicate ready must buffer");
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff(2)})
             .has_value(),
        "handoff must be accepted");
  Check(HandlerCalls == 1, "duplicate call ids must execute at most once");
  Check(Collected.Receipts.size() == 2U &&
            Collected.Receipts[0].Status ==
                Agent::ToolCallReceiptStatus::Succeeded &&
            Collected.Receipts[1].Status ==
                Agent::ToolCallReceiptStatus::Rejected &&
            Collected.Receipts[1].ResultCode == "ReplayRejected",
        "replayed call must produce a rejection receipt");
}

void ModelArgumentsCannotSpoofIdentity() {
  FinalUsageBudgetScope Budget;
  std::string ObservedPrincipal;
  Core::BusinessActionRegistry Registry;
  Check(!Registry.Register(MakeLookupAction(
                [&ObservedPrincipal](
                    const Core::BusinessActionInvocation& Invocation) {
                  ObservedPrincipal = Invocation.Principal.Value;
                  return Succeeded();
                }))
             .has_value(),
        "action must register");
  ReceiptCollector Collected;
  Agent::ToolCallCoordinator Coordinator(
      Registry, MakeAuthorization(), Collected.Sink());
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{ReadyCall(
                "call-spoof", "LookupProfile",
                R"({"query":"x","principal":"attacker"})")})
             .has_value(),
        "spoofing arguments must still buffer");
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff(2)})
             .has_value(),
        "handoff must be accepted");
  Check(ObservedPrincipal == "principal-authorized",
        "Core identity must override model-provided identity claims");
}

void HandlerOutcomesProduceCorrelatedReceipts() {
  FinalUsageBudgetScope Budget;
  {
    Core::BusinessActionRegistry Registry;
    Check(!Registry.Register(MakeLookupAction(
                  [](const Core::BusinessActionInvocation&)
                      -> Core::BusinessActionExecutionResult {
                    throw std::runtime_error("synthetic handler failure");
                  }))
               .has_value(),
          "throwing action must register");
    ReceiptCollector Collected;
    Agent::ToolCallCoordinator Coordinator(
        Registry, MakeAuthorization(), Collected.Sink());
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{
                  ReadyCall("call-throw", "LookupProfile",
                            R"({"query":"x"})")})
               .has_value(),
          "throwing call must buffer");
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff(2)})
               .has_value(),
          "handoff must be accepted");
    Check(Collected.Receipts.size() == 1U &&
              Collected.Receipts[0].Status ==
                  Agent::ToolCallReceiptStatus::UnknownOutcome &&
              Collected.Receipts[0].ResultCode == "HandlerException",
          "handler exceptions must become unknown-outcome receipts");
    Check(Coordinator.HandlerExecutionCount() == 1U,
          "exceptions must not be retried automatically");
  }
  {
    Core::BusinessActionRegistry Registry;
    Check(!Registry.Register(MakeLookupAction(
                  [](const Core::BusinessActionInvocation&) {
                    return Core::BusinessActionExecutionResult{
                        .Status =
                            Core::BusinessActionExecutionStatus::UnknownOutcome,
                        .SanitizedCode = "Timeout",
                    };
                  }))
               .has_value(),
          "unknown-outcome action must register");
    ReceiptCollector Collected;
    Agent::ToolCallCoordinator Coordinator(
        Registry, MakeAuthorization(), Collected.Sink());
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{
                  ReadyCall("call-unknown-outcome", "LookupProfile",
                            R"({"query":"x"})")})
               .has_value(),
          "unknown-outcome call must buffer");
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff(2)})
               .has_value(),
          "handoff must be accepted");
    Check(Collected.Receipts.size() == 1U &&
              Collected.Receipts[0].Status ==
                  Agent::ToolCallReceiptStatus::UnknownOutcome &&
              Collected.Receipts[0].ResultCode == "Timeout",
          "unknown outcomes must be correlated without blind retry");
    Check(Coordinator.HandlerExecutionCount() == 1U,
          "unknown outcomes must execute at most once");
    Check(Coordinator.Accept(Agent::AgentLifecycleEvent{
                  ReadyCall("call-unknown-outcome", "LookupProfile",
                            R"({"query":"x"})", 3)})
              .value()
              .Code == Agent::ToolCoordinatorErrorCode::EventAfterTerminal,
          "post-terminal replays must fail closed");
    Check(Coordinator.HandlerExecutionCount() == 1U,
          "post-terminal replays must not execute again");
  }
  {
    Core::BusinessActionRegistry Registry;
    Check(!Registry.Register(MakeLookupAction(
                  [](const Core::BusinessActionInvocation&) {
                    return Core::BusinessActionExecutionResult{
                        .Status =
                            Core::BusinessActionExecutionStatus::Failed,
                        .SanitizedCode = "ActionFailed",
                    };
                  }))
               .has_value(),
          "failing action must register");
    ReceiptCollector Collected;
    Agent::ToolCallCoordinator Coordinator(
        Registry, MakeAuthorization(), Collected.Sink());
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{
                  ReadyCall("call-failed", "LookupProfile",
                            R"({"query":"x"})")})
               .has_value(),
          "failing call must buffer");
    Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff(2)})
               .has_value(),
          "handoff must be accepted");
    Check(Collected.Receipts.size() == 1U &&
              Collected.Receipts[0].Status ==
                  Agent::ToolCallReceiptStatus::Failed &&
              Collected.Receipts[0].ResultCode == "ActionFailed",
          "failed actions must emit failed receipts");
  }
}

void TurnMismatchAndEmptySinkAreHandled() {
  FinalUsageBudgetScope Budget;
  Core::BusinessActionRegistry Registry;
  int Calls = 0;
  Check(!Registry.Register(MakeLookupAction(
                [&Calls](const Core::BusinessActionInvocation&) {
                  ++Calls;
                  return Succeeded();
                }))
             .has_value(),
        "action must register");
  ReceiptCollector Collected;
  Agent::ToolCallCoordinator Coordinator(
      Registry, MakeAuthorization(), Collected.Sink());
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{ReadyCall(
                "call-turn", "LookupProfile", R"({"query":"x"})", 1,
                "other-turn")})
             .has_value(),
        "turn mismatch must produce a receipt without hard failure");
  Check(Collected.Receipts.size() == 1U &&
            Collected.Receipts[0].Status ==
                Agent::ToolCallReceiptStatus::Rejected &&
            Collected.Receipts[0].ResultCode == "TurnMismatch",
        "turn mismatch must be rejected");
  Check(Calls == 0, "turn mismatch must not execute");

  Agent::ToolCallCoordinator SinklessCoordinator(
      Registry, MakeAuthorization(), {});
  Check(!SinklessCoordinator.Accept(Agent::AgentLifecycleEvent{
                ReadyCall("call-sinkless", "LookupProfile",
                          R"({"query":"x"})")})
             .has_value(),
        "sinkless coordinator must buffer");
  Check(!SinklessCoordinator.Accept(Agent::AgentLifecycleEvent{Handoff(2)})
             .has_value(),
        "sinkless handoff must be accepted");
  Check(Calls == 1 && SinklessCoordinator.ReceiptCount() == 1U,
        "sinkless coordinator must still execute and count receipts");
}

void DuplicateTerminalsFailClosed() {
  FinalUsageBudgetScope Budget;
  Core::BusinessActionRegistry Registry;
  ReceiptCollector Collected;
  Agent::ToolCallCoordinator Coordinator(
      Registry, MakeAuthorization(), Collected.Sink());
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff(1)})
             .has_value(),
        "handoff terminal must be accepted");
  Check(Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff(2)}).value().Code ==
            Agent::ToolCoordinatorErrorCode::EventAfterTerminal,
        "second terminal must fail closed");
  Check(Collected.Receipts.empty(),
        "terminal-only handoffs must not fabricate receipts");
}

void MissingUsageCannotAuthorizeBufferedToolDispatch() {
  Core::BusinessActionRegistry Registry;
  std::size_t HandlerCalls = 0;
  Check(!Registry.Register(MakeLookupAction(
              [&HandlerCalls](const Core::BusinessActionInvocation&) {
                ++HandlerCalls;
                return Succeeded();
              })).has_value(),
        "the synthetic lookup action should register");

  auto& Budget = Agent::OutputBudgetController::Shared();
  const Providers::AgentTurnId Turn{"turn-1"};
  const Providers::RequestId Request{"request-1"};
  Check(Budget.BeginTurn(Turn, 64),
        "Core must establish the turn budget before provider execution");
  Check(Budget.BeginRequest(Turn, Request) ==
            Agent::OutputBudgetRequestResult::Accepted,
        "the first provider request should be associated with the turn");
  Check(Budget.CompleteRequest(Turn, Request, std::nullopt, false, true),
        "a ToolHandoff with no usage must be recorded as unknown usage");

  ReceiptCollector Collected;
  Agent::ToolCallCoordinator Coordinator(
      Registry, MakeAuthorization(), Collected.Sink());
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{
                ReadyCall("call-no-usage", "LookupProfile",
                          R"({"query":"private profile"})")})
             .has_value(),
        "complete tool arguments should buffer pending the provider terminal");
  Check(!Coordinator.Accept(Agent::AgentLifecycleEvent{Handoff()}).has_value(),
        "unknown usage should fail closed without an event protocol error");
  Check(HandlerCalls == 0 && Coordinator.HandlerExecutionCount() == 0,
        "missing usage must prevent business action dispatch");
  Check(Collected.Receipts.size() == 1 &&
            Collected.Receipts[0].Status ==
                Agent::ToolCallReceiptStatus::Rejected &&
            Collected.Receipts[0].ResultCode ==
                "UsageBudgetNotAuthoritative",
        "rejected tool call should report a correlated budget-gate receipt");
  Budget.EndTurn(Turn);
}

} // namespace

int main() {
  RegistryRejectsUnsafeOrInvalidDefinitions();
  ArgumentValidationAcceptsOnlyStrictObjects();
  FullRoundTripThroughProjectorExecutesOnce();
  NonHandoffTerminalsDiscardPendingCalls();
  HandoffCorrelationIsAuthoritative();
  BusinessValidationRejectsBeforeDispatch();
  ReplaysAndDuplicateCallsExecuteAtMostOnce();
  ModelArgumentsCannotSpoofIdentity();
  HandlerOutcomesProduceCorrelatedReceipts();
  TurnMismatchAndEmptySinkAreHandled();
  DuplicateTerminalsFailClosed();
  MissingUsageCannotAuthorizeBufferedToolDispatch();
  std::cout << "ToolCallRoundTrip contract tests passed\n";
  return 0;
}
