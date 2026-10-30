#include "Source/Agent/AgentRuntime.h"
#include "Source/Agent/OutputBudgetController.hpp"

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace EmulativeDialog::Agent {
namespace {

using Core::AgentRunFailureCode;
using Core::AgentRunResult;
using Channels::MediaReferenceKind;
using Providers::CanonicalMessage;
using Providers::ContentPart;
using Providers::ImageContentReference;
using Providers::MessageRole;
using Providers::TextContentPart;

struct MappedInput final {
  enum class Status {
    Ready,
    Invalid,
    Unsupported,
  };

  Status Result{Status::Invalid};
  bool RequiresVision{false};
  CanonicalMessage Message;
};

MappedInput MapInboundMessage(const Channels::CanonicalMessage& Input) {
  MappedInput Mapped;
  Mapped.Message.Role = MessageRole::User;
  Mapped.Result = MappedInput::Status::Ready;

  for (const Channels::MessagePart& Part : Input.MessageParts) {
    std::visit(
        [&Mapped](const auto& Value) {
          using PartType = std::decay_t<decltype(Value)>;
          if constexpr (std::is_same_v<PartType, Channels::TextPart>) {
            Mapped.Message.ContentParts.emplace_back(
                TextContentPart{.Text = Value.Utf8Text});
          } else if constexpr (
              std::is_same_v<PartType, Channels::MediaReferencePart>) {
            if (Value.Reference.Value.empty()) {
              Mapped.Result = MappedInput::Status::Invalid;
              return;
            }
            if (Value.Kind != MediaReferenceKind::Image) {
              Mapped.Result = MappedInput::Status::Unsupported;
              return;
            }
            Mapped.RequiresVision = true;
            Mapped.Message.ContentParts.emplace_back(
                ImageContentReference{
                    .OpaqueReference = Value.Reference.Value});
          }
        },
        Part);
    if (Mapped.Result != MappedInput::Status::Ready) {
      return Mapped;
    }
  }

  if (Mapped.Message.ContentParts.empty()) {
    Mapped.Result = MappedInput::Status::Invalid;
  }
  return Mapped;
}

AgentRunResult MapProviderError(const Providers::ProviderError& Error) {
  const std::optional<AgentRunFailureCode> Failure =
      Core::MapProviderFailureCode(Error.Code);
  if (!Failure.has_value()) {
    return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
  }
  return AgentRunResult::Failed(*Failure);
}

class StreamAccumulator final {
public:
  bool Accept(const Providers::CanonicalNonTerminalEvent& Event,
              const Providers::RequestId& ExpectedRequest,
              const Providers::AgentTurnId& ExpectedTurn) {
    return std::visit(
        [this, &ExpectedRequest, &ExpectedTurn](const auto& Value) {
          if (!AcceptMetadata(Value.Metadata, ExpectedRequest, ExpectedTurn)) {
            Invalid = true;
            return false;
          }

          using EventType = std::decay_t<decltype(Value)>;
          if constexpr (std::is_same_v<EventType,
                                      Providers::TextDeltaEvent>) {
            if (!Value.Utf8Delta.empty()) {
              HasTextDelta = true;
              Text.append(Value.Utf8Delta);
            }
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::RefusalDeltaEvent>) {
            if (!Value.Utf8Delta.empty()) {
              HasRefusalDelta = true;
              RefusalText.append(Value.Utf8Delta);
            }
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ToolCallStartedEvent> ||
                               std::is_same_v<
                                   EventType,
                                   Providers::ToolArgumentDeltaEvent> ||
                               std::is_same_v<
                                   EventType,
                                   Providers::ToolCallReadyEvent>) {
            HasToolCall = true;
          }
          return true;
        },
        Event);
  }

  bool AcceptTerminal(const Providers::ProviderEventMetadata& Metadata,
                      const Providers::RequestId& ExpectedRequest,
                      const Providers::AgentTurnId& ExpectedTurn) {
    const bool Accepted =
        AcceptMetadata(Metadata, ExpectedRequest, ExpectedTurn);
    Invalid = Invalid || !Accepted;
    return Accepted;
  }

  bool Invalid{false};
  bool HasTextDelta{false};
  bool HasRefusalDelta{false};
  bool HasToolCall{false};
  std::string Text;
  std::string RefusalText;

private:
  bool AcceptMetadata(const Providers::ProviderEventMetadata& Metadata,
                      const Providers::RequestId& ExpectedRequest,
                      const Providers::AgentTurnId& ExpectedTurn) {
    if (Metadata.Request.Value != ExpectedRequest.Value ||
        Metadata.TurnId.Value != ExpectedTurn.Value ||
        Metadata.SequenceNumber != NextSequence ||
        NextSequence == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
    ++NextSequence;
    return true;
  }

  std::uint64_t NextSequence{1};
};

const Providers::ProviderEventMetadata& GetTerminalMetadata(
    const Providers::CanonicalTerminalEvent& Event) {
  return std::visit(
      [](const auto& Value) -> const Providers::ProviderEventMetadata& {
        return Value.Metadata;
      },
      Event);
}

} // namespace

Core::AgentRunResult AgentRuntime::RunTurn(
    const Core::AuthorizedTurnContext& Context,
    Providers::ModelProviderPort& Provider,
    const Providers::ModelProviderProfile& Profile,
    const Providers::CredentialLease& Lease,
    std::stop_token CancellationToken) {
  if (CancellationToken.stop_requested()) {
    return AgentRunResult::Cancelled();
  }
  if (Context.AgentTurn.Value.empty()) {
    return AgentRunResult::Failed(AgentRunFailureCode::InvalidRequest);
  }

  const MappedInput Input = MapInboundMessage(Context.Message);
  if (Input.Result == MappedInput::Status::Invalid) {
    return AgentRunResult::Failed(AgentRunFailureCode::InvalidRequest);
  }
  if (Input.Result == MappedInput::Status::Unsupported) {
    return AgentRunResult::Unsupported();
  }

  try {
    const Providers::ProviderResult<std::monostate> ProfileValidation =
        Provider.ValidateProfile(Profile);
    if (const auto* Error =
            std::get_if<Providers::ProviderError>(&ProfileValidation)) {
      return MapProviderError(*Error);
    }

    const Providers::ProviderResult<Providers::ProviderCapabilities>
        CapabilityResult = Provider.GetCapabilities(Profile.Id);
    if (const auto* Error =
            std::get_if<Providers::ProviderError>(&CapabilityResult)) {
      return MapProviderError(*Error);
    }
    const auto& Capabilities =
        std::get<Providers::ProviderCapabilities>(CapabilityResult);
    if (!Capabilities.Streaming ||
        (Input.RequiresVision && !Capabilities.Vision) ||
        (Profile.Limit.MaximumOutputTokens.has_value() &&
         !Capabilities.OutputLimit)) {
      return AgentRunResult::Unsupported();
    }

    const std::uint64_t RequestSequence =
        NextRequestSequence.fetch_add(1, std::memory_order_relaxed);
    if (RequestSequence == 0) {
      return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
    }

    // RequestId remains local correlation data and is not a channel identity.
    const std::string RequestValue =
        "agent-runtime:" + std::to_string(Context.AgentTurn.Value.size()) +
        ":" + Context.AgentTurn.Value + ":" +
        std::to_string(RequestSequence);
    const Providers::RequestId RequestId{RequestValue};
    OutputBudgetController& Budget = OutputBudgetController::Shared();
    if (Budget.BeginRequest(Context.AgentTurn, RequestId) !=
        OutputBudgetRequestResult::Accepted) {
      return AgentRunResult::Failed(AgentRunFailureCode::OutputLimitReached);
    }

    Providers::CanonicalModelRequest Request;
    Request.Id = RequestId;
    Request.TurnId = Context.AgentTurn;
    Request.CreatedAt = std::chrono::system_clock::now();
    Request.Messages.push_back(Input.Message);
    Request.ModelName = Profile.ModelName;
    Request.Limit = Profile.Limit;
    Request.Sampling = Profile.Sampling;
    Request.CapabilitySnapshot = Capabilities;

    StreamAccumulator Accumulator;
    const Providers::EventSink Sink =
        [&Accumulator, &RequestId, &Context, &Budget](
            const Providers::CanonicalNonTerminalEvent& Event) {
          if (!Accumulator.Accept(Event, RequestId, Context.AgentTurn)) {
            return;
          }
          if (const auto* Usage =
                  std::get_if<Providers::UsageUpdatedEvent>(&Event)) {
            if (!Budget.ObserveUsage(Context.AgentTurn, RequestId,
                                     Usage->Usage)) {
              Accumulator.Invalid = true;
            }
          }
        };

    Providers::CanonicalTerminalEvent Terminal;
    try {
      Terminal = Provider.StreamTurn(Profile, Lease, Request,
                                     CancellationToken, Sink);
    } catch (...) {
      static_cast<void>(Budget.CompleteRequest(
          Context.AgentTurn, RequestId, std::nullopt, false, false));
      return AgentRunResult::UnknownOutcome();
    }

    std::optional<Providers::UsageSnapshot> TerminalUsage;
    bool OutputLimited = false;
    bool ToolHandoff = false;
    std::visit(
        [&TerminalUsage, &OutputLimited, &ToolHandoff](const auto& Event) {
          using TerminalType = std::decay_t<decltype(Event)>;
          if constexpr (std::is_same_v<
                            TerminalType,
                            Providers::ResponseCompletedEvent>) {
            TerminalUsage = Event.FinalUsage;
            ToolHandoff = Event.Kind == Providers::CompletionKind::ToolHandoff;
          } else if constexpr (std::is_same_v<
                                   TerminalType,
                                   Providers::ResponseOutputLimitedEvent>) {
            TerminalUsage = Event.Usage;
            OutputLimited = true;
          }
        },
        Terminal);
    const auto& Metadata = GetTerminalMetadata(Terminal);
    static_cast<void>(Accumulator.AcceptTerminal(Metadata, RequestId,
                                                 Context.AgentTurn));
    if (Accumulator.Invalid) {
      return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
    }
    if (!Budget.CompleteRequest(Context.AgentTurn, RequestId, TerminalUsage,
                                OutputLimited, ToolHandoff)) {
      return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
    }

    return std::visit(
        [&Accumulator, &Profile](const auto& Event) -> AgentRunResult {
          using TerminalType = std::decay_t<decltype(Event)>;
          if constexpr (std::is_same_v<
                            TerminalType,
                            Providers::ResponseCompletedEvent>) {
            if (Event.StopDetails.has_value() &&
                (Event.Kind != Providers::CompletionKind::Refusal ||
                 Event.StopDetails->Type !=
                     Providers::RefusalStopDetailsType::Refusal)) {
              return AgentRunResult::Failed(
                  AgentRunFailureCode::InvalidStream);
            }
            if (Accumulator.HasToolCall ||
                Event.Kind == Providers::CompletionKind::ToolHandoff) {
              return AgentRunResult::Unsupported();
            }
            if (Event.Kind == Providers::CompletionKind::FinalAnswer) {
              if (Accumulator.HasRefusalDelta ||
                  Event.StopDetails.has_value() ||
                  Accumulator.Text.empty()) {
                return AgentRunResult::Failed(
                    AgentRunFailureCode::InvalidStream);
              }
              return AgentRunResult::ResponseReady(Event.Kind,
                                                   std::move(Accumulator.Text));
            }
            if (Event.Kind == Providers::CompletionKind::Refusal) {
              if (Profile.Protocol ==
                      Providers::ProviderProtocol::AnthropicMessages &&
                  !Event.StopDetails.has_value()) {
                return AgentRunResult::Failed(
                    AgentRunFailureCode::InvalidStream);
              }
              if (Profile.Protocol == Providers::ProviderProtocol::
                                       OpenAICompatibleChatCompletions &&
                  Accumulator.HasTextDelta) {
                return AgentRunResult::Failed(
                    AgentRunFailureCode::InvalidStream);
              }
              if (Accumulator.RefusalText.empty()) {
                return AgentRunResult::Failed(
                    AgentRunFailureCode::InvalidStream);
              }
              return AgentRunResult::ResponseReady(
                  Event.Kind, std::move(Accumulator.RefusalText));
            }
            return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
          } else if constexpr (std::is_same_v<
                                   TerminalType,
                                   Providers::ResponseOutputLimitedEvent>) {
            // SafeGeneratedText is partial output and is never sendable.
            return AgentRunResult::Failed(
                AgentRunFailureCode::OutputLimitReached);
          } else if constexpr (std::is_same_v<
                                   TerminalType,
                                   Providers::ResponseFailedEvent>) {
            return MapProviderError(Event.Failure);
          } else if constexpr (std::is_same_v<
                                   TerminalType,
                                   Providers::ResponseCancelledEvent>) {
            return AgentRunResult::Cancelled();
          } else {
            return AgentRunResult::UnknownOutcome();
          }
        },
        Terminal);
  } catch (...) {
    return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
  }
}

} // namespace EmulativeDialog::Agent
