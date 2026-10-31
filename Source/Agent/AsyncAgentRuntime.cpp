#include "Source/Agent/AsyncAgentRuntime.hpp"

#include "Source/Agent/OutputBudgetController.hpp"
#include "Source/Agent/AsyncToolCallCoordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

std::atomic<std::uint64_t> NextAsyncRequestSequence{1};

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

std::optional<std::uint64_t> ReserveRequestSequence() noexcept {
  std::uint64_t Current =
      NextAsyncRequestSequence.load(std::memory_order_relaxed);
  for (;;) {
    if (Current == 0 || Current == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    if (NextAsyncRequestSequence.compare_exchange_weak(
            Current, Current + 1, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return Current;
    }
  }
}

} // namespace

boost::asio::awaitable<Core::AgentRunResult>
AsyncAgentRuntime::RunTurnAsync(
    const Core::AuthorizedTurnContext& Context,
    Providers::AsyncModelProviderPort& Provider,
    const Providers::ModelProviderProfile& Profile,
    const Providers::CredentialLease& Lease,
    std::stop_token CancellationToken) {
  if (CancellationToken.stop_requested()) {
    co_return AgentRunResult::Cancelled();
  }
  if (Context.AgentTurn.Value.empty()) {
    co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidRequest);
  }

  const MappedInput Input = MapInboundMessage(Context.Message);
  if (Input.Result == MappedInput::Status::Invalid) {
    co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidRequest);
  }
  if (Input.Result == MappedInput::Status::Unsupported) {
    co_return AgentRunResult::Unsupported();
  }

  try {
    const Providers::ProviderResult<std::monostate> ProfileValidation =
        co_await Provider.ValidateProfileAsync(Profile);
    if (const auto* Error =
            std::get_if<Providers::ProviderError>(&ProfileValidation)) {
      co_return MapProviderError(*Error);
    }

    const Providers::ProviderResult<Providers::ProviderCapabilities>
        CapabilityResult = co_await Provider.GetCapabilitiesAsync(Profile.Id);
    if (const auto* Error =
            std::get_if<Providers::ProviderError>(&CapabilityResult)) {
      co_return MapProviderError(*Error);
    }
    const auto& Capabilities =
        std::get<Providers::ProviderCapabilities>(CapabilityResult);
    if (!Capabilities.Streaming ||
        (Input.RequiresVision && !Capabilities.Vision) ||
        (Profile.Limit.MaximumOutputTokens.has_value() &&
         !Capabilities.OutputLimit)) {
      co_return AgentRunResult::Unsupported();
    }

    const std::optional<std::uint64_t> RequestSequence =
        ReserveRequestSequence();
    if (!RequestSequence.has_value()) {
      co_return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
    }

    const std::string RequestValue =
        "async-agent-runtime:" + std::to_string(Context.AgentTurn.Value.size()) +
        ":" + Context.AgentTurn.Value + ":" +
        std::to_string(*RequestSequence);
    const Providers::RequestId RequestId{RequestValue};
    OutputBudgetController& Budget = OutputBudgetController::Shared();
    if (Budget.BeginRequest(Context.AgentTurn, RequestId) !=
        OutputBudgetRequestResult::Accepted) {
      co_return AgentRunResult::Failed(AgentRunFailureCode::OutputLimitReached);
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

    std::optional<Providers::CanonicalTerminalEvent> Terminal;
    try {
      Terminal.emplace(co_await Provider.StreamTurnAsync(
          Profile, Lease, Request, CancellationToken, Sink));
    } catch (...) {
      static_cast<void>(Budget.CompleteRequest(Context.AgentTurn, RequestId,
                                               std::nullopt, false, false));
      co_return AgentRunResult::UnknownOutcome();
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
        *Terminal);

    const auto& Metadata = GetTerminalMetadata(*Terminal);
    static_cast<void>(Accumulator.AcceptTerminal(Metadata, RequestId,
                                                 Context.AgentTurn));
    if (Accumulator.Invalid) {
      co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
    }
    if (!Budget.CompleteRequest(Context.AgentTurn, RequestId, TerminalUsage,
                                OutputLimited, ToolHandoff)) {
      co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
    }

    co_return std::visit(
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
              return AgentRunResult::ResponseReady(
                  Event.Kind, std::move(Accumulator.Text));
            }
            if (Event.Kind == Providers::CompletionKind::Refusal) {
              if (Profile.Protocol ==
                      Providers::ProviderProtocol::AnthropicMessages &&
                  !Event.StopDetails.has_value()) {
                return AgentRunResult::Failed(
                    AgentRunFailureCode::InvalidStream);
              }
              if (Profile.Protocol ==
                      Providers::ProviderProtocol::
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
        *Terminal);
  } catch (...) {
    co_return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
  }
}

boost::asio::awaitable<Core::AgentRunResult>
AsyncAgentRuntime::RunTurnWithToolsAsync(
    const Core::AuthorizedTurnContext& Context,
    Providers::AsyncModelProviderPort& Provider,
    const Providers::ModelProviderProfile& Profile,
    const Providers::CredentialLease& Lease,
    Core::AsyncBusinessActionDispatcherPort& Dispatcher,
    std::stop_token CancellationToken) {
  if (CancellationToken.stop_requested()) {
    co_return AgentRunResult::Cancelled();
  }
  if (Context.AgentTurn.Value.empty()) {
    co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidRequest);
  }

  const MappedInput Input = MapInboundMessage(Context.Message);
  if (Input.Result == MappedInput::Status::Invalid) {
    co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidRequest);
  }
  if (Input.Result == MappedInput::Status::Unsupported) {
    co_return AgentRunResult::Unsupported();
  }

  try {
    const Providers::ProviderResult<std::monostate> ProfileValidation =
        co_await Provider.ValidateProfileAsync(Profile);
    if (const auto* Error =
            std::get_if<Providers::ProviderError>(&ProfileValidation)) {
      co_return MapProviderError(*Error);
    }

    const Providers::ProviderResult<Providers::ProviderCapabilities>
        CapabilityResult = co_await Provider.GetCapabilitiesAsync(Profile.Id);
    if (const auto* Error =
            std::get_if<Providers::ProviderError>(&CapabilityResult)) {
      co_return MapProviderError(*Error);
    }
    const Providers::ProviderCapabilities Capabilities =
        std::get<Providers::ProviderCapabilities>(CapabilityResult);
    if (!Capabilities.Streaming ||
        (Input.RequiresVision && !Capabilities.Vision) ||
        (Profile.Limit.MaximumOutputTokens.has_value() &&
         !Capabilities.OutputLimit)) {
      co_return AgentRunResult::Unsupported();
    }

    const std::vector<Providers::ToolDefinition> ToolDefinitions =
        Dispatcher.GetToolDefinitions(Context);
    if (!ToolDefinitions.empty() && !Capabilities.ToolCalls) {
      co_return AgentRunResult::Unsupported();
    }

    std::vector<Providers::CanonicalMessage> Transcript{Input.Message};
    std::uint32_t HandoffCycles = 0;
    std::uint32_t ProposedCalls = 0;
    std::uint64_t CumulativeOutputTokens = 0;
    Agent::OutputBudgetController& Budget = OutputBudgetController::Shared();

    for (;;) {
      if (CancellationToken.stop_requested()) {
        co_return AgentRunResult::Cancelled();
      }
      const std::optional<std::uint64_t> RequestSequence =
          ReserveRequestSequence();
      if (!RequestSequence.has_value()) {
        co_return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
      }
      const std::string RequestValue =
          "async-agent-runtime:" +
          std::to_string(Context.AgentTurn.Value.size()) + ":" +
          Context.AgentTurn.Value + ":" +
          std::to_string(*RequestSequence);
      const Providers::RequestId RequestId{RequestValue};
      if (Budget.BeginRequest(Context.AgentTurn, RequestId) !=
          OutputBudgetRequestResult::Accepted) {
        co_return AgentRunResult::Failed(
            AgentRunFailureCode::OutputLimitReached);
      }

      Providers::CanonicalModelRequest Request;
      Request.Id = RequestId;
      Request.TurnId = Context.AgentTurn;
      Request.CreatedAt = std::chrono::system_clock::now();
      Request.Messages = Transcript;
      Request.ModelName = Profile.ModelName;
      Request.Limit = Profile.Limit;
      Request.Sampling = Profile.Sampling;
      Request.CapabilitySnapshot = Capabilities;
      Request.ToolDefinitions = ToolDefinitions;

      StreamAccumulator Accumulator;
      AsyncToolCallCoordinator ToolCoordinator{RequestId, Context.AgentTurn};
      const Providers::EventSink Sink =
          [&Accumulator, &ToolCoordinator, &RequestId, &Context, &Budget](
              const Providers::CanonicalNonTerminalEvent& Event) {
            const bool StreamAccepted =
                Accumulator.Accept(Event, RequestId, Context.AgentTurn);
            const bool ToolAccepted = ToolCoordinator.Accept(Event);
            if (!StreamAccepted || !ToolAccepted) {
              Accumulator.Invalid = true;
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

      std::optional<Providers::CanonicalTerminalEvent> Terminal;
      try {
        Terminal.emplace(co_await Provider.StreamTurnAsync(
            Profile, Lease, Request, CancellationToken, Sink));
      } catch (...) {
        static_cast<void>(Budget.CompleteRequest(
            Context.AgentTurn, RequestId, std::nullopt, false, false));
        co_return AgentRunResult::UnknownOutcome();
      }

      std::optional<Providers::UsageSnapshot> TerminalUsage;
      bool OutputLimited = false;
      bool ProviderToolHandoff = false;
      std::visit(
          [&TerminalUsage, &OutputLimited,
           &ProviderToolHandoff](const auto& Event) {
            using TerminalType = std::decay_t<decltype(Event)>;
            if constexpr (std::is_same_v<
                              TerminalType,
                              Providers::ResponseCompletedEvent>) {
              TerminalUsage = Event.FinalUsage;
              ProviderToolHandoff =
                  Event.Kind == Providers::CompletionKind::ToolHandoff;
            } else if constexpr (std::is_same_v<
                                     TerminalType,
                                     Providers::ResponseOutputLimitedEvent>) {
              TerminalUsage = Event.Usage;
              OutputLimited = true;
            }
          },
          *Terminal);

      const Providers::ProviderEventMetadata& TerminalMetadata =
          GetTerminalMetadata(*Terminal);
      static_cast<void>(Accumulator.AcceptTerminal(
          TerminalMetadata, RequestId, Context.AgentTurn));
      if (Accumulator.Invalid) {
        static_cast<void>(ToolCoordinator.Complete(*Terminal, false));
        static_cast<void>(Budget.CompleteRequest(
            Context.AgentTurn, RequestId, TerminalUsage, OutputLimited,
            ProviderToolHandoff));
        co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
      }
      if (!Budget.CompleteRequest(Context.AgentTurn, RequestId, TerminalUsage,
                                  OutputLimited, ProviderToolHandoff)) {
        co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
      }

      const bool BudgetAllowsDispatch =
          Budget.CanDispatchTool(Context.AgentTurn, RequestId);
      const AsyncToolHandoffDecision Decision =
          ToolCoordinator.Complete(*Terminal, BudgetAllowsDispatch);
      if (Decision == AsyncToolHandoffDecision::Invalid) {
        co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
      }

      if (Decision != AsyncToolHandoffDecision::Dispatch) {
        if (Decision == AsyncToolHandoffDecision::BudgetRejected) {
          co_return AgentRunResult::Failed(
              AgentRunFailureCode::OutputLimitReached);
        }
        co_return std::visit(
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
                  return AgentRunResult::Failed(
                      AgentRunFailureCode::InvalidStream);
                }
                if (Event.Kind == Providers::CompletionKind::FinalAnswer) {
                  if (Accumulator.HasRefusalDelta ||
                      Event.StopDetails.has_value() || Accumulator.Text.empty()) {
                    return AgentRunResult::Failed(
                        AgentRunFailureCode::InvalidStream);
                  }
                  return AgentRunResult::ResponseReady(
                      Event.Kind, std::move(Accumulator.Text));
                }
                if (Event.Kind == Providers::CompletionKind::Refusal) {
                  if ((Profile.Protocol ==
                           Providers::ProviderProtocol::AnthropicMessages &&
                       !Event.StopDetails.has_value()) ||
                      (Profile.Protocol ==
                           Providers::ProviderProtocol::
                               OpenAICompatibleChatCompletions &&
                       Accumulator.HasTextDelta) ||
                      Accumulator.RefusalText.empty()) {
                    return AgentRunResult::Failed(
                        AgentRunFailureCode::InvalidStream);
                  }
                  return AgentRunResult::ResponseReady(
                      Event.Kind, std::move(Accumulator.RefusalText));
                }
                return AgentRunResult::Failed(
                    AgentRunFailureCode::InvalidStream);
              } else if constexpr (std::is_same_v<
                                       TerminalType,
                                       Providers::ResponseOutputLimitedEvent>) {
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
            *Terminal);
      }

      if (CancellationToken.stop_requested()) {
        co_return AgentRunResult::Cancelled();
      }
      std::vector<Providers::FunctionCall> Calls =
          ToolCoordinator.TakePending();
      if (!ProviderToolHandoff || Calls.empty()) {
        co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
      }
      if (!TerminalUsage.has_value() ||
          TerminalUsage->Completeness != Providers::UsageCompleteness::Final ||
          !TerminalUsage->OutputTokens.has_value()) {
        co_return AgentRunResult::Failed(
            AgentRunFailureCode::OutputLimitReached);
      }
      const std::uint64_t CurrentOutputTokens =
          *TerminalUsage->OutputTokens;
      if (CumulativeOutputTokens >
              std::numeric_limits<std::uint64_t>::max() -
                  CurrentOutputTokens ||
          (Profile.Limit.MaximumOutputTokens.has_value() &&
           CumulativeOutputTokens + CurrentOutputTokens >=
               *Profile.Limit.MaximumOutputTokens)) {
        co_return AgentRunResult::Failed(
            AgentRunFailureCode::OutputLimitReached);
      }
      if (HandoffCycles >= 4U || Calls.size() > 8U - ProposedCalls) {
        co_return AgentRunResult::Failed(
            AgentRunFailureCode::OutputLimitReached);
      }
      CumulativeOutputTokens += CurrentOutputTokens;
      ++HandoffCycles;
      ProposedCalls += static_cast<std::uint32_t>(Calls.size());

      std::vector<Core::CoreActionReceipt> Receipts;
      try {
        Receipts = co_await Dispatcher.DispatchAsync(
            Context, RequestId, Calls, CancellationToken);
      } catch (...) {
        co_return CancellationToken.stop_requested()
                      ? AgentRunResult::Cancelled()
                      : AgentRunResult::Failed(
                            AgentRunFailureCode::InternalError);
      }
      if (CancellationToken.stop_requested()) {
        co_return AgentRunResult::Cancelled();
      }
      if (Receipts.size() != Calls.size()) {
        co_return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
      }

      Providers::CanonicalMessage AssistantCalls;
      AssistantCalls.Role = MessageRole::Assistant;
      AssistantCalls.FunctionCalls = Calls;
      Transcript.push_back(std::move(AssistantCalls));
      for (std::size_t Index = 0; Index < Calls.size(); ++Index) {
        const Core::CoreActionReceipt& Receipt = Receipts[Index];
        const Providers::FunctionCall& Call = Calls[Index];
        const bool ValidCode =
            !Receipt.SanitizedCode.empty() &&
            Receipt.SanitizedCode.size() <= 64U &&
            std::all_of(Receipt.SanitizedCode.begin(),
                        Receipt.SanitizedCode.end(), [](char Character) {
                          const auto Byte =
                              static_cast<unsigned char>(Character);
                          return (Byte >= 'A' && Byte <= 'Z') ||
                                 (Byte >= 'a' && Byte <= 'z') ||
                                 (Byte >= '0' && Byte <= '9') ||
                                 Character == '_' || Character == '-';
                        });
        const bool CorrelatedActionName =
            Receipt.ActionName == Call.Name ||
            (Receipt.Status == Core::CoreActionReceiptStatus::Rejected &&
             Receipt.ActionName == "UnknownAction");
        if (Receipt.ToolCall.Value != Call.Id.Value ||
            Receipt.Request.Value != RequestId.Value ||
            Receipt.Turn.Value != Context.AgentTurn.Value ||
            !CorrelatedActionName || !ValidCode) {
          co_return AgentRunResult::Failed(AgentRunFailureCode::InvalidStream);
        }
        Providers::CanonicalMessage ToolResult;
        ToolResult.Role = MessageRole::Tool;
        ToolResult.ToolCallResultId = Receipt.ToolCall;
        ToolResult.ContentParts.emplace_back(
            TextContentPart{.Text = Receipt.SanitizedCode});
        Transcript.push_back(std::move(ToolResult));
      }

      Budget.EndTurn(Context.AgentTurn);
      if (!Budget.BeginTurn(Context.AgentTurn,
                            Profile.Limit.MaximumOutputTokens)) {
        co_return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
      }
    }
  } catch (...) {
    co_return AgentRunResult::Failed(AgentRunFailureCode::InternalError);
  }
}

} // namespace EmulativeDialog::Agent
