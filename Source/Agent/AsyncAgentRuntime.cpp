#include "Source/Agent/AsyncAgentRuntime.hpp"

#include "Source/Agent/OutputBudgetController.hpp"
#include "Source/Agent/AsyncToolCallCoordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
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

enum class StreamAdmission : std::uint8_t {
  Accepted,
  Invalid,
  ResourceLimit,
  Ignored,
};

constexpr std::uint64_t MaximumCanonicalEvents = 1024U;
constexpr std::uint64_t MaximumDynamicStringBytes = 256U * 1024U;
constexpr std::uint64_t MaximumTextBytes = 64U * 1024U;
constexpr std::uint64_t MaximumToolArgumentBytes = 64U * 1024U;
constexpr std::size_t MaximumPendingCalls = 8U;

bool FitsRemaining(std::uint64_t Used, std::size_t Delta,
                   std::uint64_t Maximum) noexcept {
  if (Used > Maximum) {
    return false;
  }
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (Delta > std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
  }
  return static_cast<std::uint64_t>(Delta) <= Maximum - Used;
}

std::optional<std::size_t> CompactJsonByteCount(
    std::string_view Json) noexcept {
  std::size_t Count = 0;
  bool InString = false;
  bool Escaped = false;
  for (const char Character : Json) {
    if (InString) {
      if (Count == std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
      }
      ++Count;
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
    } else if (Character == ' ' || Character == '\t' ||
               Character == '\n' || Character == '\r') {
      continue;
    }
    if (Count == std::numeric_limits<std::size_t>::max()) {
      return std::nullopt;
    }
    ++Count;
  }
  if (InString || Escaped) {
    return std::nullopt;
  }
  return Count;
}

class StreamAccumulator final {
public:
  StreamAdmission Accept(const Providers::CanonicalNonTerminalEvent& Event,
                         const Providers::RequestId& ExpectedRequest,
                         const Providers::AgentTurnId& ExpectedTurn) {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (ResourceLimitLatched) {
      return StreamAdmission::Ignored;
    }
    if (Invalid) {
      return StreamAdmission::Invalid;
    }
    if (!FitsRemaining(EventCount, 1U, MaximumCanonicalEvents)) {
      ResourceLimitLatched = true;
      return StreamAdmission::ResourceLimit;
    }

    std::size_t DynamicDelta = 0;
    std::size_t TextDelta = 0;
    std::size_t ArgumentDelta = 0;
    std::string_view PendingId;
    bool AddsPendingCall = false;
    const bool PayloadMeasured = std::visit(
        [&](const auto& Value) {
          using EventType = std::decay_t<decltype(Value)>;
          if constexpr (std::is_same_v<EventType,
                                       Providers::TextDeltaEvent>) {
            DynamicDelta = Value.Utf8Delta.size();
            TextDelta = Value.Utf8Delta.size();
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::RefusalDeltaEvent>) {
            DynamicDelta = Value.Utf8Delta.size();
            TextDelta = Value.Utf8Delta.size();
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ToolCallStartedEvent>) {
            DynamicDelta = Value.Call.Value.size();
            PendingId = Value.Call.Value;
            AddsPendingCall = !PendingCalls.contains(Value.Call.Value);
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ToolArgumentDeltaEvent>) {
            if (Value.Call.Value.size() >
                std::numeric_limits<std::size_t>::max() -
                    Value.JsonTextDelta.size()) {
              return false;
            }
            DynamicDelta = Value.Call.Value.size() + Value.JsonTextDelta.size();
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ToolCallReadyEvent>) {
            if (Value.Call.Value.size() >
                    std::numeric_limits<std::size_t>::max() -
                        Value.ToolName.size() ||
                Value.Call.Value.size() + Value.ToolName.size() >
                    std::numeric_limits<std::size_t>::max() -
                        Value.Arguments.JsonText.size()) {
              return false;
            }
            DynamicDelta = Value.Call.Value.size() + Value.ToolName.size() +
                           Value.Arguments.JsonText.size();
            const std::optional<std::size_t> CompactBytes =
                CompactJsonByteCount(Value.Arguments.JsonText);
            if (!CompactBytes.has_value()) {
              return false;
            }
            ArgumentDelta = *CompactBytes;
            PendingId = Value.Call.Value;
            AddsPendingCall = !PendingCalls.contains(Value.Call.Value);
          }
          return true;
        },
        Event);
    if (!PayloadMeasured) {
      Invalid = true;
      return StreamAdmission::Invalid;
    }
    if (!FitsRemaining(DynamicStringBytes, DynamicDelta,
                       MaximumDynamicStringBytes) ||
        !FitsRemaining(TextBytes, TextDelta, MaximumTextBytes) ||
        !FitsRemaining(ToolArgumentBytes, ArgumentDelta,
                       MaximumToolArgumentBytes) ||
        (AddsPendingCall && PendingCalls.size() >= MaximumPendingCalls)) {
      ResourceLimitLatched = true;
      return StreamAdmission::ResourceLimit;
    }
    if (!std::visit(
            [this, &ExpectedRequest, &ExpectedTurn](const auto& Value) {
              return AcceptMetadata(Value.Metadata, ExpectedRequest,
                                    ExpectedTurn);
            },
            Event)) {
      Invalid = true;
      return StreamAdmission::Invalid;
    }

    ++EventCount;
    DynamicStringBytes += static_cast<std::uint64_t>(DynamicDelta);
    TextBytes += static_cast<std::uint64_t>(TextDelta);
    ToolArgumentBytes += static_cast<std::uint64_t>(ArgumentDelta);
    if (AddsPendingCall && !PendingId.empty()) {
      PendingCalls.emplace(PendingId);
    }
    return std::visit(
        [this](const auto& Value) {
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
          return StreamAdmission::Accepted;
        },
        Event);
  }

  StreamAdmission AcceptTerminal(
      const Providers::CanonicalTerminalEvent& Event,
      const Providers::RequestId& ExpectedRequest,
      const Providers::AgentTurnId& ExpectedTurn) {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (ResourceLimitLatched) {
      return StreamAdmission::ResourceLimit;
    }
    if (Invalid) {
      return StreamAdmission::Invalid;
    }
    if (!FitsRemaining(EventCount, 1U, MaximumCanonicalEvents)) {
      ResourceLimitLatched = true;
      return StreamAdmission::ResourceLimit;
    }
    std::size_t DynamicDelta = 0;
    std::size_t SafeTextDelta = 0;
    const bool Measured = std::visit(
        [&DynamicDelta, &SafeTextDelta](const auto& Value) {
          using EventType = std::decay_t<decltype(Value)>;
          if constexpr (std::is_same_v<
                            EventType,
                            Providers::ResponseOutputLimitedEvent>) {
            if (Value.SafeGeneratedText.has_value()) {
              DynamicDelta = Value.SafeGeneratedText->size();
              SafeTextDelta = Value.SafeGeneratedText->size();
            }
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ResponseCompletedEvent>) {
            if (Value.StopSequenceCode.has_value()) {
              DynamicDelta = Value.StopSequenceCode->Value.size();
            }
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ResponseFailedEvent>) {
            DynamicDelta = Value.Failure.SanitizedDescription.size();
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ResponseCancelledEvent>) {
            DynamicDelta = Value.Reason.Code.Value.size();
          } else if constexpr (std::is_same_v<
                                   EventType,
                                   Providers::ResponseOutcomeUnknownEvent>) {
            DynamicDelta = Value.Reason.Code.Value.size();
          }
          return true;
        },
        Event);
    if (!Measured ||
        !FitsRemaining(DynamicStringBytes, DynamicDelta,
                       MaximumDynamicStringBytes) ||
        !FitsRemaining(TextBytes, SafeTextDelta, MaximumTextBytes)) {
      ResourceLimitLatched = true;
      return StreamAdmission::ResourceLimit;
    }
    const bool MetadataAccepted = std::visit(
        [this, &ExpectedRequest, &ExpectedTurn](const auto& Value) {
          return AcceptMetadata(Value.Metadata, ExpectedRequest,
                                ExpectedTurn);
        },
        Event);
    if (!MetadataAccepted) {
      Invalid = true;
      return StreamAdmission::Invalid;
    }
    ++EventCount;
    DynamicStringBytes += static_cast<std::uint64_t>(DynamicDelta);
    TextBytes += static_cast<std::uint64_t>(SafeTextDelta);
    return StreamAdmission::Accepted;
  }

  void MarkInvalid() noexcept {
    std::lock_guard<std::mutex> Lock(Mutex);
    Invalid = true;
  }

  [[nodiscard]] bool IsResourceLimitLatched() const noexcept {
    std::lock_guard<std::mutex> Lock(Mutex);
    return ResourceLimitLatched;
  }

  bool Invalid{false};
  bool ResourceLimitLatched{false};
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
  std::uint64_t EventCount{};
  std::uint64_t DynamicStringBytes{};
  std::uint64_t TextBytes{};
  std::uint64_t ToolArgumentBytes{};
  std::unordered_set<std::string> PendingCalls;
  mutable std::mutex Mutex;
};

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
    std::stop_source RequestStopSource;
    const auto ForwardCallerCancellation = [&RequestStopSource]() noexcept {
      static_cast<void>(RequestStopSource.request_stop());
    };
    std::stop_callback CallerCancellation{CancellationToken,
                                          ForwardCallerCancellation};
    static_cast<void>(CallerCancellation);
    const Providers::EventSink Sink =
        [&Accumulator, &RequestId, &Context, &Budget, &RequestStopSource](
            const Providers::CanonicalNonTerminalEvent& Event) {
          const StreamAdmission Admission =
              Accumulator.Accept(Event, RequestId, Context.AgentTurn);
          if (Admission == StreamAdmission::ResourceLimit) {
            static_cast<void>(RequestStopSource.request_stop());
            return;
          }
          if (Admission != StreamAdmission::Accepted) {
            return;
          }
          if (const auto* Usage =
                  std::get_if<Providers::UsageUpdatedEvent>(&Event)) {
            if (!Budget.ObserveUsage(Context.AgentTurn, RequestId,
                                     Usage->Usage)) {
              Accumulator.MarkInvalid();
            }
          }
        };

    std::optional<Providers::CanonicalTerminalEvent> Terminal;
    try {
      Terminal.emplace(co_await Provider.StreamTurnAsync(
          Profile, Lease, Request, RequestStopSource.get_token(), Sink));
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

    if (std::holds_alternative<Providers::ResponseOutcomeUnknownEvent>(
            *Terminal)) {
      static_cast<void>(Budget.CompleteRequest(Context.AgentTurn, RequestId,
                                               TerminalUsage, OutputLimited,
                                               ToolHandoff));
      co_return AgentRunResult::UnknownOutcome();
    }
    const StreamAdmission TerminalAdmission = Accumulator.AcceptTerminal(
        *Terminal, RequestId, Context.AgentTurn);
    if (TerminalAdmission == StreamAdmission::ResourceLimit ||
        Accumulator.IsResourceLimitLatched()) {
      static_cast<void>(Budget.CompleteRequest(Context.AgentTurn, RequestId,
                                               TerminalUsage, OutputLimited,
                                               ToolHandoff));
      co_return AgentRunResult::Failed(
          AgentRunFailureCode::OutputLimitReached);
    }
    if (TerminalAdmission != StreamAdmission::Accepted || Accumulator.Invalid) {
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
      std::stop_source RequestStopSource;
      const auto ForwardCallerCancellation = [&RequestStopSource]() noexcept {
        static_cast<void>(RequestStopSource.request_stop());
      };
      std::stop_callback CallerCancellation{CancellationToken,
                                            ForwardCallerCancellation};
      static_cast<void>(CallerCancellation);
      const Providers::EventSink Sink =
          [&Accumulator, &ToolCoordinator, &RequestId, &Context, &Budget,
           &RequestStopSource](
              const Providers::CanonicalNonTerminalEvent& Event) {
            const StreamAdmission StreamResult =
                Accumulator.Accept(Event, RequestId, Context.AgentTurn);
            if (StreamResult == StreamAdmission::ResourceLimit) {
              static_cast<void>(RequestStopSource.request_stop());
              return;
            }
            if (StreamResult == StreamAdmission::Ignored) {
              return;
            }
            if (StreamResult == StreamAdmission::Invalid) {
              return;
            }
            const bool ToolAccepted = ToolCoordinator.Accept(Event);
            if (!ToolAccepted) {
              Accumulator.MarkInvalid();
              return;
            }
            if (const auto* Usage =
                    std::get_if<Providers::UsageUpdatedEvent>(&Event)) {
              if (!Budget.ObserveUsage(Context.AgentTurn, RequestId,
                                       Usage->Usage)) {
                Accumulator.MarkInvalid();
              }
            }
          };

      std::optional<Providers::CanonicalTerminalEvent> Terminal;
      try {
        Terminal.emplace(co_await Provider.StreamTurnAsync(
            Profile, Lease, Request, RequestStopSource.get_token(), Sink));
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

      if (std::holds_alternative<Providers::ResponseOutcomeUnknownEvent>(
              *Terminal)) {
        static_cast<void>(ToolCoordinator.Complete(*Terminal, false));
        static_cast<void>(Budget.CompleteRequest(
            Context.AgentTurn, RequestId, TerminalUsage, OutputLimited,
            ProviderToolHandoff));
        co_return AgentRunResult::UnknownOutcome();
      }
      const StreamAdmission TerminalAdmission = Accumulator.AcceptTerminal(
          *Terminal, RequestId, Context.AgentTurn);
      if (TerminalAdmission == StreamAdmission::ResourceLimit ||
          Accumulator.IsResourceLimitLatched()) {
        static_cast<void>(ToolCoordinator.Complete(*Terminal, false));
        static_cast<void>(Budget.CompleteRequest(
            Context.AgentTurn, RequestId, TerminalUsage, OutputLimited,
            ProviderToolHandoff));
        co_return AgentRunResult::Failed(
            AgentRunFailureCode::OutputLimitReached);
      }
      if (TerminalAdmission != StreamAdmission::Accepted ||
          Accumulator.Invalid) {
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
