#include "Source/Agent/AgentEventProjector.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace EmulativeDialog;

template <typename Value, typename = void>
struct HasSanitizedDescription : std::false_type {};

template <typename Value>
struct HasSanitizedDescription<
    Value, std::void_t<decltype(std::declval<Value&>()
                                    .SanitizedDescription)>>
    : std::true_type {};

template <typename Value, typename = void>
struct HasSafeGeneratedText : std::false_type {};

template <typename Value>
struct HasSafeGeneratedText<
    Value,
    std::void_t<decltype(std::declval<Value&>().SafeGeneratedText)>>
    : std::true_type {};

template <typename Value, typename = void>
struct HasReasoning : std::false_type {};

template <typename Value>
struct HasReasoning<
    Value,
    std::void_t<decltype(std::declval<Value&>().Reasoning)>>
    : std::true_type {};

static_assert(std::variant_size_v<Agent::AgentLifecycleEvent> == 14);
static_assert(!HasSanitizedDescription<Agent::AgentFailedEvent>::value);
static_assert(!HasSafeGeneratedText<Agent::AgentOutputLimitedEvent>::value);
static_assert(!HasReasoning<Agent::AgentTextDeltaEvent>::value);

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

Providers::RequestId ExpectedRequest() {
  return Providers::RequestId{"request-synthetic"};
}

Providers::AgentTurnId ExpectedTurn() {
  return Providers::AgentTurnId{"turn-synthetic"};
}

Providers::ProviderEventMetadata Meta(
    std::uint64_t Sequence,
    std::string Request = ExpectedRequest().Value,
    std::string Turn = ExpectedTurn().Value) {
  return Providers::ProviderEventMetadata{
      .Request = Providers::RequestId{std::move(Request)},
      .TurnId = Providers::AgentTurnId{std::move(Turn)},
      .SequenceNumber = Sequence,
  };
}

Providers::CanonicalTerminalEvent FinalAnswer(std::uint64_t Sequence) {
  return Providers::ResponseCompletedEvent{
      .Metadata = Meta(Sequence),
      .Kind = Providers::CompletionKind::FinalAnswer,
      .StopSequenceCode = std::nullopt,
      .FinalUsage = std::nullopt,
      .StopDetails = std::nullopt,
  };
}

struct Collector final {
  std::vector<Agent::AgentLifecycleEvent> Events;

  Agent::AgentEventProjector::Sink Sink() {
    return [this](const Agent::AgentLifecycleEvent& Event) {
      Events.push_back(Event);
    };
  }
};

const Agent::AgentEventMetadata& MetadataOf(
    const Agent::AgentLifecycleEvent& Event) {
  return std::visit(
      [](const auto& Value) -> const Agent::AgentEventMetadata& {
        return Value.Metadata;
      },
      Event);
}

std::optional<Agent::AgentProjectionErrorCode> ErrorOf(
    const std::optional<Agent::AgentProjectionError>& Error) {
  if (!Error.has_value()) {
    return std::nullopt;
  }
  return Error->Code;
}

void ProjectsStartedTextUsageAndFinal() {
  Collector Collected;
  Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                       Collected.Sink());

  const Providers::CanonicalNonTerminalEvent Started =
      Providers::ResponseStartedEvent{
          .Metadata = Meta(1),
          .ProviderResponse =
              Providers::ProviderResponseId{"provider-response-1"},
      };
  const Providers::CanonicalNonTerminalEvent FirstText =
      Providers::TextDeltaEvent{
          .Metadata = Meta(2),
          .Item = Providers::ItemId{"item-text"},
          .Utf8Delta = "hel",
      };
  const Providers::CanonicalNonTerminalEvent SecondText =
      Providers::TextDeltaEvent{
          .Metadata = Meta(3),
          .Item = Providers::ItemId{"item-text"},
          .Utf8Delta = "lo",
      };
  const Providers::CanonicalNonTerminalEvent Usage =
      Providers::UsageUpdatedEvent{
          .Metadata = Meta(4),
          .Usage =
              Providers::UsageSnapshot{
                  .InputTokens = 10,
                  .OutputTokens = 20,
                  .TotalTokens = 30,
                  .CachedInputReadTokens = std::nullopt,
                  .CachedInputWriteTokens = std::nullopt,
                  .ReasoningOutputTokens = std::nullopt,
                  .Completeness = Providers::UsageCompleteness::Final,
              },
      };

  Check(!Projector.Accept(Started).has_value(), "started event must project");
  Check(!Projector.Accept(FirstText).has_value(), "first text must project");
  Check(!Projector.Accept(SecondText).has_value(), "second text must project");
  Check(!Projector.Accept(Usage).has_value(), "usage must project");
  Check(!Projector.Accept(FinalAnswer(5)).has_value(), "final must project");

  Check(Collected.Events.size() == 5U, "expected five projected events");
  Check(std::holds_alternative<Agent::AgentStartedEvent>(
            Collected.Events[0]),
        "first event must be started");
  Check(std::holds_alternative<Agent::AgentTextDeltaEvent>(
            Collected.Events[1]) &&
            std::holds_alternative<Agent::AgentTextDeltaEvent>(
                Collected.Events[2]),
        "text deltas must remain typed");
  Check(std::holds_alternative<Agent::AgentUsageUpdatedEvent>(
            Collected.Events[3]),
        "usage must project as usage");
  Check(std::holds_alternative<Agent::AgentFinalEvent>(Collected.Events[4]),
        "final answer must project as final");
  Check(MetadataOf(Collected.Events[0]).SequenceNumber == 1U &&
            MetadataOf(Collected.Events[4]).SequenceNumber == 5U,
        "projected metadata must preserve upstream sequence numbers");
  Check(Projector.HasTerminal(), "final answer must close the projector");
  Check(Projector.NextExpectedSequence() == 6U,
        "next expected sequence must advance past the terminal");
  Check(Projector.EmittedCount() == 5U, "emitted count must match");
}

void ProjectsRefusalWithCategory() {
  Collector Collected;
  Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                       Collected.Sink());
  const Providers::CanonicalNonTerminalEvent RefusalDelta =
      Providers::RefusalDeltaEvent{
          .Metadata = Meta(1),
          .Item = Providers::ItemId{"item-refusal"},
          .Utf8Delta = "cannot comply",
          .Category = Providers::RefusalCategory::GeneralHarms,
      };
  const Providers::CanonicalTerminalEvent Refusal =
      Providers::ResponseCompletedEvent{
          .Metadata = Meta(2),
          .Kind = Providers::CompletionKind::Refusal,
          .StopSequenceCode = std::nullopt,
          .FinalUsage = std::nullopt,
          .StopDetails =
              Providers::RefusalStopDetails{
                  .Type = Providers::RefusalStopDetailsType::Refusal,
                  .Category = Providers::RefusalCategory::Bio,
              },
      };

  Check(!Projector.Accept(RefusalDelta).has_value(),
        "refusal delta must project");
  Check(!Projector.Accept(Refusal).has_value(), "refusal must project");
  const auto* Projected =
      std::get_if<Agent::AgentRefusalEvent>(&Collected.Events[1]);
  Check(Projected != nullptr &&
            Projected->Category == Providers::RefusalCategory::Bio,
        "typed refusal category must survive projection");
}

void ProjectsToolLifecycleAndHandoff() {
  Collector Collected;
  Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                       Collected.Sink());
  const std::vector<Providers::CanonicalNonTerminalEvent> Events = {
      Providers::ResponseStartedEvent{.Metadata = Meta(1),
                                      .ProviderResponse = std::nullopt},
      Providers::ToolCallStartedEvent{
          .Metadata = Meta(2),
          .Item = Providers::ItemId{"item-tool"},
          .Call = Providers::ToolCallId{"call-tool"},
          .ToolName = "lookup",
      },
      Providers::ToolArgumentDeltaEvent{
          .Metadata = Meta(3),
          .Item = Providers::ItemId{"item-tool"},
          .Call = Providers::ToolCallId{"call-tool"},
          .JsonTextDelta = "{\"query\":\"x\"}",
      },
      Providers::ToolCallReadyEvent{
          .Metadata = Meta(4),
          .Item = Providers::ItemId{"item-tool"},
          .Call = Providers::ToolCallId{"call-tool"},
          .ToolName = "lookup",
          .Arguments = Providers::JsonValue{"{\"query\":\"x\"}"},
      },
  };
  for (const auto& Event : Events) {
    Check(!Projector.Accept(Event).has_value(),
          "tool lifecycle event must project");
  }
  const Providers::CanonicalTerminalEvent Handoff =
      Providers::ResponseCompletedEvent{
          .Metadata = Meta(5),
          .Kind = Providers::CompletionKind::ToolHandoff,
          .StopSequenceCode = std::nullopt,
          .FinalUsage = std::nullopt,
          .StopDetails = std::nullopt,
      };
  Check(!Projector.Accept(Handoff).has_value(), "handoff must project");
  Check(Collected.Events.size() == 5U, "expected five tool events");
  const auto* Ready =
      std::get_if<Agent::AgentToolCallReadyEvent>(&Collected.Events[3]);
  Check(Ready != nullptr && Ready->Call.Value == "call-tool" &&
            Ready->Arguments.JsonText == "{\"query\":\"x\"}",
        "ready tool call must preserve call id and arguments");
  Check(std::holds_alternative<Agent::AgentToolHandoffEvent>(
            Collected.Events[4]),
        "tool handoff terminal must remain typed");
}

void RejectsWrongCorrelation() {
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalNonTerminalEvent WrongRequest =
        Providers::ResponseStartedEvent{
            .Metadata = Meta(1, "other-request"),
            .ProviderResponse = std::nullopt,
        };
    Check(ErrorOf(Projector.Accept(WrongRequest)) ==
              Agent::AgentProjectionErrorCode::WrongRequest,
          "wrong request must fail closed");
    Check(Collected.Events.empty() && !Projector.HasTerminal(),
          "failed correlation must emit nothing");
  }
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalNonTerminalEvent WrongTurn =
        Providers::ResponseStartedEvent{
            .Metadata = Meta(1, ExpectedRequest().Value, "other-turn"),
            .ProviderResponse = std::nullopt,
        };
    Check(ErrorOf(Projector.Accept(WrongTurn)) ==
              Agent::AgentProjectionErrorCode::WrongTurn,
          "wrong turn must fail closed");
    Check(Collected.Events.empty() && !Projector.HasTerminal(),
          "failed turn correlation must emit nothing");
  }
}

void RejectsSequenceGapsAndRegressions() {
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalNonTerminalEvent First =
        Providers::ResponseStartedEvent{.Metadata = Meta(1),
                                        .ProviderResponse = std::nullopt};
    const Providers::CanonicalNonTerminalEvent Gap =
        Providers::TextDeltaEvent{
            .Metadata = Meta(3),
            .Item = Providers::ItemId{"item-text"},
            .Utf8Delta = "gap",
        };
    Check(!Projector.Accept(First).has_value(), "first event must project");
    Check(ErrorOf(Projector.Accept(Gap)) ==
              Agent::AgentProjectionErrorCode::NonMonotonicSequence,
          "sequence gaps must fail closed");
    Check(Projector.NextExpectedSequence() == 2U,
          "rejected gap must not advance the sequence");
  }
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                  Providers::ResponseStartedEvent{
                      .Metadata = Meta(1),
                      .ProviderResponse = std::nullopt}})
               .has_value(),
          "first event must project");
    Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                  Providers::TextDeltaEvent{
                      .Metadata = Meta(2),
                      .Item = Providers::ItemId{"item-text"},
                      .Utf8Delta = "two"}})
               .has_value(),
          "second event must project");
    const Providers::CanonicalNonTerminalEvent Regression =
        Providers::TextDeltaEvent{
            .Metadata = Meta(1),
            .Item = Providers::ItemId{"item-text"},
            .Utf8Delta = "regressed",
        };
    Check(ErrorOf(Projector.Accept(Regression)) ==
              Agent::AgentProjectionErrorCode::NonMonotonicSequence,
          "sequence regressions must fail closed");
  }
}

void IgnoresIdenticalDuplicatesAndRejectsConflicts() {
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalNonTerminalEvent Started =
        Providers::ResponseStartedEvent{.Metadata = Meta(1),
                                        .ProviderResponse = std::nullopt};
    Check(!Projector.Accept(Started).has_value(), "started must project");
    Check(!Projector.Accept(Started).has_value(),
          "identical duplicate delivery must be ignored");
    Check(Collected.Events.size() == 1U &&
              Projector.NextExpectedSequence() == 2U,
          "ignored duplicates must not re-emit or advance");
    Check(!Projector.Accept(FinalAnswer(2)).has_value(),
          "terminal after a duplicate must still project");
  }
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalNonTerminalEvent First =
        Providers::TextDeltaEvent{
            .Metadata = Meta(1),
            .Item = Providers::ItemId{"item-text"},
            .Utf8Delta = "a",
        };
    const Providers::CanonicalNonTerminalEvent Conflict =
        Providers::TextDeltaEvent{
            .Metadata = Meta(1),
            .Item = Providers::ItemId{"item-text"},
            .Utf8Delta = "b",
        };
    Check(!Projector.Accept(First).has_value(), "first text must project");
    Check(ErrorOf(Projector.Accept(Conflict)) ==
              Agent::AgentProjectionErrorCode::DuplicateConflict,
          "conflicting duplicate sequence must fail closed");
  }
}

void RejectsEventsAfterTerminalAndSecondTerminals() {
  Collector Collected;
  Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                       Collected.Sink());
  Check(!Projector.Accept(FinalAnswer(1)).has_value(),
        "terminal must project");
  const Providers::CanonicalNonTerminalEvent Late =
      Providers::TextDeltaEvent{
          .Metadata = Meta(2),
          .Item = Providers::ItemId{"item-text"},
          .Utf8Delta = "late",
      };
  Check(ErrorOf(Projector.Accept(Late)) ==
            Agent::AgentProjectionErrorCode::EventAfterTerminal,
        "nonterminal events after a terminal must fail closed");
  Check(ErrorOf(Projector.Accept(FinalAnswer(2))) ==
            Agent::AgentProjectionErrorCode::SecondTerminal,
        "second terminals must fail closed");
  Check(ErrorOf(Projector.Accept(FinalAnswer(1))) ==
            Agent::AgentProjectionErrorCode::SecondTerminal,
        "duplicate terminal deliveries remain second terminals");
  Check(Collected.Events.size() == 1U,
        "rejected late events must not emit");
}

void RejectsTerminalCorrelationAndSequenceErrors() {
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                  Providers::ResponseStartedEvent{
                      .Metadata = Meta(1),
                      .ProviderResponse = std::nullopt}})
               .has_value(),
          "started must project");
    const Providers::CanonicalTerminalEvent WrongRequest =
        Providers::ResponseCompletedEvent{
            .Metadata = Meta(2, "other-request"),
            .Kind = Providers::CompletionKind::FinalAnswer,
            .StopSequenceCode = std::nullopt,
            .FinalUsage = std::nullopt,
            .StopDetails = std::nullopt,
        };
    Check(ErrorOf(Projector.Accept(WrongRequest)) ==
              Agent::AgentProjectionErrorCode::WrongRequest,
          "terminal from another request must fail closed");
    Check(!Projector.HasTerminal(), "rejected terminal must not close");
    Check(!Projector.Accept(FinalAnswer(2)).has_value(),
          "a valid terminal may still be accepted afterwards");
  }
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                  Providers::ResponseStartedEvent{
                      .Metadata = Meta(1),
                      .ProviderResponse = std::nullopt}})
               .has_value(),
          "started must project");
    Check(ErrorOf(Projector.Accept(FinalAnswer(3))) ==
              Agent::AgentProjectionErrorCode::NonMonotonicSequence,
          "terminal sequence gaps must fail closed");
    Check(!Projector.HasTerminal(), "gapped terminal must not close");
  }
}

void OutputLimitedMarksPartialTextUnsendable() {
  Collector Collected;
  Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                       Collected.Sink());
  Check(!Projector.Accept(Providers::CanonicalNonTerminalEvent{
                Providers::TextDeltaEvent{
                    .Metadata = Meta(1),
                    .Item = Providers::ItemId{"item-text"},
                    .Utf8Delta = "partial"}})
             .has_value(),
        "partial text must project");
  const Providers::CanonicalTerminalEvent Limited =
      Providers::ResponseOutputLimitedEvent{
          .Metadata = Meta(2),
          .Kind = Providers::LimitKind::ContextWindow,
          .Usage = Providers::UsageSnapshot{
              .InputTokens = 5,
              .OutputTokens = 6,
              .TotalTokens = 11,
              .CachedInputReadTokens = std::nullopt,
              .CachedInputWriteTokens = std::nullopt,
              .ReasoningOutputTokens = std::nullopt,
              .Completeness = Providers::UsageCompleteness::Partial,
          },
          .SafeGeneratedText = std::string{"secret partial bytes"},
      };
  Check(!Projector.Accept(Limited).has_value(), "output limit must project");
  const auto* Projected =
      std::get_if<Agent::AgentOutputLimitedEvent>(&Collected.Events[1]);
  Check(Projected != nullptr && Projected->Kind ==
                                    Providers::LimitKind::ContextWindow &&
            Projected->HasUnsendablePartialText,
        "output limit must mark partial output as unsendable");
  Check(Projector.HasTerminal(), "output limit is terminal");
}

void ProjectsFailureCancelAndUnknownSafely() {
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalTerminalEvent Failed =
        Providers::ResponseFailedEvent{
            .Metadata = Meta(1),
            .Failure = Providers::ProviderError{
                Providers::FailureCode::RateLimited, true,
                "sanitized sentinel SECRET_SHOULD_NOT_PROJECT"},
        };
    Check(!Projector.Accept(Failed).has_value(), "failure must project");
    const auto* Projected =
        std::get_if<Agent::AgentFailedEvent>(&Collected.Events[0]);
    Check(Projected != nullptr &&
              Projected->Code == Providers::FailureCode::RateLimited &&
              Projected->Retryable,
          "failure code and retryability must survive projection");
  }
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalTerminalEvent Cancelled =
        Providers::ResponseCancelledEvent{
            .Metadata = Meta(1),
            .Reason = Providers::CancelReasonCode{
                Providers::StableReasonCode{"cancelled-before-send"}},
        };
    Check(!Projector.Accept(Cancelled).has_value(), "cancel must project");
    const auto* Projected =
        std::get_if<Agent::AgentCancelledEvent>(&Collected.Events[0]);
    Check(Projected != nullptr &&
              Projected->ReasonCode == "cancelled-before-send",
          "cancel reason code must survive projection");
  }
  {
    Collector Collected;
    Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(),
                                         Collected.Sink());
    const Providers::CanonicalTerminalEvent Unknown =
        Providers::ResponseOutcomeUnknownEvent{
            .Metadata = Meta(1),
            .Reason = Providers::OutcomeUnknownReasonCode{
                Providers::StableReasonCode{"stream-lost"}},
        };
    Check(!Projector.Accept(Unknown).has_value(), "unknown must project");
    const auto* Projected =
        std::get_if<Agent::AgentOutcomeUnknownEvent>(&Collected.Events[0]);
    Check(Projected != nullptr && Projected->ReasonCode == "stream-lost",
          "outcome-unknown reason code must survive projection");
  }
}

void EmptySinkStillTracksState() {
  Agent::AgentEventProjector Projector(ExpectedRequest(), ExpectedTurn(), {});
  const Providers::CanonicalNonTerminalEvent Started =
      Providers::ResponseStartedEvent{.Metadata = Meta(1),
                                      .ProviderResponse = std::nullopt};
  Check(!Projector.Accept(Started).has_value(),
        "projector must work without a sink");
  Check(Projector.EmittedCount() == 1U &&
            Projector.NextExpectedSequence() == 2U,
        "state must advance even without a sink");
}

} // namespace

int main() {
  ProjectsStartedTextUsageAndFinal();
  ProjectsRefusalWithCategory();
  ProjectsToolLifecycleAndHandoff();
  RejectsWrongCorrelation();
  RejectsSequenceGapsAndRegressions();
  IgnoresIdenticalDuplicatesAndRejectsConflicts();
  RejectsEventsAfterTerminalAndSecondTerminals();
  RejectsTerminalCorrelationAndSequenceErrors();
  OutputLimitedMarksPartialTextUnsendable();
  ProjectsFailureCancelAndUnknownSafely();
  EmptySinkStillTracksState();
  std::cout << "AgentEventProjector contract tests passed\n";
  return 0;
}
