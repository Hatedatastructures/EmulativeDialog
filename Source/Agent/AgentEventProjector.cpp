#include "Source/Agent/AgentEventProjector.hpp"

#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace EmulativeDialog::Agent {
namespace {

using Providers::AgentTurnId;
using Providers::CanonicalNonTerminalEvent;
using Providers::CanonicalTerminalEvent;
using Providers::ProviderEventMetadata;
using Providers::RequestId;

AgentProjectionError Failure(AgentProjectionErrorCode Code) noexcept {
  return AgentProjectionError{.Code = Code};
}

AgentEventMetadata ToAgentMetadata(
    const ProviderEventMetadata& Metadata) noexcept {
  return AgentEventMetadata{
      .Request = Metadata.Request,
      .Turn = Metadata.TurnId,
      .SequenceNumber = Metadata.SequenceNumber,
  };
}

const AgentEventMetadata& GetMetadata(
    const AgentLifecycleEvent& Event) noexcept {
  return std::visit(
      [](const auto& Value) -> const AgentEventMetadata& {
        return Value.Metadata;
      },
      Event);
}

// Length-prefixed field encoding. Each field is "<length>:<bytes>;" so field
// boundaries can never collide, even when values contain the separator bytes.
void AppendField(std::string& Target, std::string_view Value) {
  Target.append(std::to_string(Value.size()));
  Target.push_back(':');
  Target.append(Value.data(), Value.size());
  Target.push_back(';');
}

template <typename ValueType>
std::string NumberText(const ValueType& Value) {
  if constexpr (std::is_enum_v<ValueType>) {
    return std::to_string(static_cast<unsigned>(Value));
  } else {
    return std::to_string(Value);
  }
}

void AppendOptionalNumber(std::string& Target, const auto& Value) {
  if (!Value.has_value()) {
    Target.push_back('0');
    return;
  }
  Target.push_back('1');
  AppendField(Target, NumberText(*Value));
}

void AppendUsage(std::string& Target, const Providers::UsageSnapshot& Usage) {
  AppendOptionalNumber(Target, Usage.InputTokens);
  AppendOptionalNumber(Target, Usage.OutputTokens);
  AppendOptionalNumber(Target, Usage.TotalTokens);
  AppendOptionalNumber(Target, Usage.CachedInputReadTokens);
  AppendOptionalNumber(Target, Usage.CachedInputWriteTokens);
  AppendOptionalNumber(Target, Usage.ReasoningOutputTokens);
  AppendField(Target,
              std::to_string(static_cast<unsigned>(Usage.Completeness)));
}

std::string Fingerprint(const AgentLifecycleEvent& Event) {
  return std::visit(
      [](const auto& Value) -> std::string {
        using EventType = std::decay_t<decltype(Value)>;
        std::string Result;
        Result.reserve(64);
        AppendField(Result,
                    std::to_string(Value.Metadata.SequenceNumber));

        if constexpr (std::is_same_v<EventType, AgentStartedEvent>) {
          AppendField(Result, "S");
          if (Value.Response.has_value()) {
            Result.push_back('1');
            AppendField(Result, Value.Response->Value);
          } else {
            Result.push_back('0');
          }
        } else if constexpr (std::is_same_v<EventType,
                                            AgentTextDeltaEvent>) {
          AppendField(Result, "T");
          AppendField(Result, Value.Item.Value);
          AppendField(Result, Value.Utf8Delta);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentRefusalDeltaEvent>) {
          AppendField(Result, "R");
          AppendField(Result, Value.Item.Value);
          AppendField(Result, Value.Utf8Delta);
          AppendOptionalNumber(Result, Value.Category);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolCallStartedEvent>) {
          AppendField(Result, "TS");
          AppendField(Result, Value.Item.Value);
          AppendField(Result, Value.Call.Value);
          AppendField(Result, Value.ToolName);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolArgumentDeltaEvent>) {
          AppendField(Result, "TA");
          AppendField(Result, Value.Item.Value);
          AppendField(Result, Value.Call.Value);
          AppendField(Result, Value.JsonTextDelta);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolCallReadyEvent>) {
          AppendField(Result, "TR");
          AppendField(Result, Value.Item.Value);
          AppendField(Result, Value.Call.Value);
          AppendField(Result, Value.ToolName);
          AppendField(Result, Value.Arguments.JsonText);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentUsageUpdatedEvent>) {
          AppendField(Result, "U");
          AppendUsage(Result, Value.Usage);
        } else if constexpr (std::is_same_v<EventType, AgentFinalEvent>) {
          AppendField(Result, "F");
        } else if constexpr (std::is_same_v<EventType, AgentRefusalEvent>) {
          AppendField(Result, "RF");
          AppendOptionalNumber(Result, Value.Category);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolHandoffEvent>) {
          AppendField(Result, "TH");
        } else if constexpr (std::is_same_v<EventType,
                                            AgentOutputLimitedEvent>) {
          AppendField(Result, "OL");
          AppendField(
              Result, std::to_string(static_cast<unsigned>(Value.Kind)));
          AppendUsage(Result, Value.Usage);
          AppendField(Result,
                      Value.HasUnsendablePartialText ? "1" : "0");
        } else if constexpr (std::is_same_v<EventType, AgentFailedEvent>) {
          AppendField(Result, "X");
          AppendField(
              Result, std::to_string(static_cast<unsigned>(Value.Code)));
          AppendField(Result, Value.Retryable ? "1" : "0");
        } else if constexpr (std::is_same_v<EventType,
                                            AgentCancelledEvent>) {
          AppendField(Result, "C");
          AppendField(Result, Value.ReasonCode);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentOutcomeUnknownEvent>) {
          AppendField(Result, "OU");
          AppendField(Result, Value.ReasonCode);
        }
        return Result;
      },
      Event);
}

AgentLifecycleEvent Project(const Providers::ResponseStartedEvent& Event) {
  return AgentStartedEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Response = Event.ProviderResponse,
  };
}

AgentLifecycleEvent Project(const Providers::TextDeltaEvent& Event) {
  return AgentTextDeltaEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Item = Event.Item,
      .Utf8Delta = Event.Utf8Delta,
  };
}

AgentLifecycleEvent Project(const Providers::RefusalDeltaEvent& Event) {
  return AgentRefusalDeltaEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Item = Event.Item,
      .Utf8Delta = Event.Utf8Delta,
      .Category = Event.Category,
  };
}

AgentLifecycleEvent Project(const Providers::ToolCallStartedEvent& Event) {
  return AgentToolCallStartedEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Item = Event.Item,
      .Call = Event.Call,
      .ToolName = Event.ToolName,
  };
}

AgentLifecycleEvent Project(
    const Providers::ToolArgumentDeltaEvent& Event) {
  return AgentToolArgumentDeltaEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Item = Event.Item,
      .Call = Event.Call,
      .JsonTextDelta = Event.JsonTextDelta,
  };
}

AgentLifecycleEvent Project(const Providers::ToolCallReadyEvent& Event) {
  return AgentToolCallReadyEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Item = Event.Item,
      .Call = Event.Call,
      .ToolName = Event.ToolName,
      .Arguments = Event.Arguments,
  };
}

AgentLifecycleEvent Project(const Providers::UsageUpdatedEvent& Event) {
  return AgentUsageUpdatedEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Usage = Event.Usage,
  };
}

AgentLifecycleEvent Project(const Providers::ResponseCompletedEvent& Event) {
  switch (Event.Kind) {
  case Providers::CompletionKind::FinalAnswer:
    return AgentFinalEvent{
        .Metadata = ToAgentMetadata(Event.Metadata),
    };
  case Providers::CompletionKind::ToolHandoff:
    return AgentToolHandoffEvent{
        .Metadata = ToAgentMetadata(Event.Metadata),
    };
  case Providers::CompletionKind::Refusal:
    return AgentRefusalEvent{
        .Metadata = ToAgentMetadata(Event.Metadata),
        .Category = Event.StopDetails.has_value()
                        ? std::optional<Providers::RefusalCategory>{
                              Event.StopDetails->Category}
                        : std::nullopt,
    };
  }
  return AgentFinalEvent{.Metadata = ToAgentMetadata(Event.Metadata)};
}

AgentLifecycleEvent Project(
    const Providers::ResponseOutputLimitedEvent& Event) {
  return AgentOutputLimitedEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Kind = Event.Kind,
      .Usage = Event.Usage,
      .HasUnsendablePartialText = Event.SafeGeneratedText.has_value(),
  };
}

AgentLifecycleEvent Project(const Providers::ResponseFailedEvent& Event) {
  return AgentFailedEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .Code = Event.Failure.Code,
      .Retryable = Event.Failure.Retryable,
  };
}

AgentLifecycleEvent Project(const Providers::ResponseCancelledEvent& Event) {
  return AgentCancelledEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .ReasonCode = Event.Reason.Code.Value,
  };
}

AgentLifecycleEvent Project(
    const Providers::ResponseOutcomeUnknownEvent& Event) {
  return AgentOutcomeUnknownEvent{
      .Metadata = ToAgentMetadata(Event.Metadata),
      .ReasonCode = Event.Reason.Code.Value,
  };
}

const ProviderEventMetadata& GetCanonicalMetadata(
    const CanonicalNonTerminalEvent& Event) noexcept {
  return std::visit(
      [](const auto& Value) -> const ProviderEventMetadata& {
        return Value.Metadata;
      },
      Event);
}

const ProviderEventMetadata& GetCanonicalMetadata(
    const CanonicalTerminalEvent& Event) noexcept {
  return std::visit(
      [](const auto& Value) -> const ProviderEventMetadata& {
        return Value.Metadata;
      },
      Event);
}

} // namespace

AgentEventProjector::AgentEventProjector(RequestId ExpectedRequestValue,
                                         AgentTurnId ExpectedTurnValue,
                                         Sink EventSinkValue)
    : ExpectedRequest(std::move(ExpectedRequestValue)),
      ExpectedTurn(std::move(ExpectedTurnValue)),
      EventSink(std::move(EventSinkValue)) {}

std::optional<AgentProjectionError> AgentEventProjector::ValidateMetadata(
    const ProviderEventMetadata& Metadata) const noexcept {
  if (Metadata.Request.Value != ExpectedRequest.Value) {
    return Failure(AgentProjectionErrorCode::WrongRequest);
  }
  if (Metadata.TurnId.Value != ExpectedTurn.Value) {
    return Failure(AgentProjectionErrorCode::WrongTurn);
  }
  return std::nullopt;
}

std::optional<AgentProjectionError> AgentEventProjector::Deliver(
    AgentLifecycleEvent Event) {
  if (SequenceExhausted) {
    return Failure(AgentProjectionErrorCode::NonMonotonicSequence);
  }

  const std::uint64_t Sequence = GetMetadata(Event).SequenceNumber;
  std::string EventFingerprint = Fingerprint(Event);

  if (Sequence == NextSequence) {
    if (EventSink) {
      EventSink(Event);
    }
    LastFingerprint = std::move(EventFingerprint);
    ++Emitted;
    if (NextSequence == std::numeric_limits<std::uint64_t>::max()) {
      SequenceExhausted = true;
    } else {
      ++NextSequence;
    }
    return std::nullopt;
  }

  const bool PreviousSequence =
      NextSequence > 0 && Sequence == NextSequence - 1;
  if (PreviousSequence) {
    if (EventFingerprint == LastFingerprint) {
      return std::nullopt;
    }
    return Failure(AgentProjectionErrorCode::DuplicateConflict);
  }
  return Failure(AgentProjectionErrorCode::NonMonotonicSequence);
}

std::optional<AgentProjectionError> AgentEventProjector::Accept(
    const CanonicalNonTerminalEvent& Event) {
  if (TerminalSeen) {
    return Failure(AgentProjectionErrorCode::EventAfterTerminal);
  }
  if (const auto Error = ValidateMetadata(GetCanonicalMetadata(Event));
      Error.has_value()) {
    return Error;
  }
  return Deliver(std::visit(
      [](const auto& Value) -> AgentLifecycleEvent { return Project(Value); },
      Event));
}

std::optional<AgentProjectionError> AgentEventProjector::Accept(
    const CanonicalTerminalEvent& Event) {
  if (TerminalSeen) {
    return Failure(AgentProjectionErrorCode::SecondTerminal);
  }
  if (const auto Error = ValidateMetadata(GetCanonicalMetadata(Event));
      Error.has_value()) {
    return Error;
  }
  if (const auto Error = Deliver(std::visit(
          [](const auto& Value) -> AgentLifecycleEvent {
            return Project(Value);
          },
          Event));
      Error.has_value()) {
    return Error;
  }
  TerminalSeen = true;
  return std::nullopt;
}

} // namespace EmulativeDialog::Agent
