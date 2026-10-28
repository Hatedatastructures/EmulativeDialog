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

void AppendOptional(std::string& Target, const auto& Value) {
  if (Value.has_value()) {
    Target.append(std::to_string(*Value));
  } else {
    Target.push_back('-');
  }
  Target.push_back('|');
}

void AppendUsage(std::string& Target, const Providers::UsageSnapshot& Usage) {
  AppendOptional(Target, Usage.InputTokens);
  AppendOptional(Target, Usage.OutputTokens);
  AppendOptional(Target, Usage.TotalTokens);
  AppendOptional(Target, Usage.CachedInputReadTokens);
  AppendOptional(Target, Usage.CachedInputWriteTokens);
  AppendOptional(Target, Usage.ReasoningOutputTokens);
  Target.append(std::to_string(static_cast<unsigned>(Usage.Completeness)));
}

std::string Fingerprint(const AgentLifecycleEvent& Event) {
  return std::visit(
      [](const auto& Value) -> std::string {
        using EventType = std::decay_t<decltype(Value)>;
        std::string Result;
        Result.reserve(64);
        Result.append(std::to_string(Value.Metadata.SequenceNumber));
        Result.push_back('|');

        if constexpr (std::is_same_v<EventType, AgentStartedEvent>) {
          Result.push_back('S');
          Result.append(Value.Response.has_value() ? Value.Response->Value
                                                   : std::string{});
        } else if constexpr (std::is_same_v<EventType,
                                            AgentTextDeltaEvent>) {
          Result.push_back('T');
          Result.append(Value.Item.Value);
          Result.push_back('|');
          Result.append(Value.Utf8Delta);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentRefusalDeltaEvent>) {
          Result.push_back('R');
          Result.append(Value.Item.Value);
          Result.push_back('|');
          Result.append(Value.Utf8Delta);
          Result.push_back('|');
          if (Value.Category.has_value()) {
            Result.append(
                std::to_string(static_cast<unsigned>(*Value.Category)));
          }
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolCallStartedEvent>) {
          Result.append("TS|");
          Result.append(Value.Item.Value);
          Result.push_back('|');
          Result.append(Value.Call.Value);
          Result.push_back('|');
          Result.append(Value.ToolName);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolArgumentDeltaEvent>) {
          Result.append("TA|");
          Result.append(Value.Item.Value);
          Result.push_back('|');
          Result.append(Value.Call.Value);
          Result.push_back('|');
          Result.append(Value.JsonTextDelta);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolCallReadyEvent>) {
          Result.append("TR|");
          Result.append(Value.Item.Value);
          Result.push_back('|');
          Result.append(Value.Call.Value);
          Result.push_back('|');
          Result.append(Value.ToolName);
          Result.push_back('|');
          Result.append(Value.Arguments.JsonText);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentUsageUpdatedEvent>) {
          Result.push_back('U');
          AppendUsage(Result, Value.Usage);
        } else if constexpr (std::is_same_v<EventType, AgentFinalEvent>) {
          Result.append("F");
        } else if constexpr (std::is_same_v<EventType, AgentRefusalEvent>) {
          Result.append("RF|");
          if (Value.Category.has_value()) {
            Result.append(
                std::to_string(static_cast<unsigned>(*Value.Category)));
          }
        } else if constexpr (std::is_same_v<EventType,
                                            AgentToolHandoffEvent>) {
          Result.append("TH");
        } else if constexpr (std::is_same_v<EventType,
                                            AgentOutputLimitedEvent>) {
          Result.append("OL|");
          Result.append(std::to_string(static_cast<unsigned>(Value.Kind)));
          Result.push_back('|');
          AppendUsage(Result, Value.Usage);
          Result.push_back(Value.HasUnsendablePartialText ? '1' : '0');
        } else if constexpr (std::is_same_v<EventType, AgentFailedEvent>) {
          Result.append("X|");
          Result.append(std::to_string(static_cast<unsigned>(Value.Code)));
          Result.push_back(Value.Retryable ? '1' : '0');
        } else if constexpr (std::is_same_v<EventType,
                                            AgentCancelledEvent>) {
          Result.append("C|");
          Result.append(Value.ReasonCode);
        } else if constexpr (std::is_same_v<EventType,
                                            AgentOutcomeUnknownEvent>) {
          Result.append("OU|");
          Result.append(Value.ReasonCode);
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
