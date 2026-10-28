#include "Source/Providers/AnthropicMessages/AnthropicMessagesProvider.hpp"

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace EmulativeDialog::Providers {
namespace {

namespace json = boost::json;
using Platform::Network::CancelledTransport;
using Platform::Network::CompletedTransport;
using Platform::Network::FailedTransport;
using Platform::Network::HttpsDestination;
using Platform::Network::HttpTransportPort;
using Platform::Network::RedirectDeniedTransport;
using Platform::Network::ResponseHeadMetadata;
using Platform::Network::TimedOutTransport;
using Platform::Network::TransportFailureCode;
using Platform::Network::TransportHeader;
using Platform::Network::TransportProxyMode;
using Platform::Network::TransportRequest;
using Platform::Network::TransportRequestLimits;
using Platform::Network::TransportTimeouts;
using Platform::Network::UnknownOutcomeTransport;

ProviderError MakeError(FailureCode Code, bool Retryable,
                        std::string Description) {
  return ProviderError{.Code = Code,
                       .Retryable = Retryable,
                       .SanitizedDescription = std::move(Description)};
}

ProviderEventMetadata MakeMetadata(const RequestId& Request,
                                   const AgentTurnId& Turn,
                                   std::uint64_t Sequence) {
  return ProviderEventMetadata{.Request = Request,
                               .TurnId = Turn,
                               .SequenceNumber = Sequence};
}

CanonicalTerminalEvent FailedTerminal(const RequestId& Request,
                                      const AgentTurnId& Turn,
                                      std::uint64_t Sequence,
                                      ProviderError Error) {
  return ResponseFailedEvent{
      .Metadata = MakeMetadata(Request, Turn, Sequence),
      .Failure = std::move(Error),
  };
}

bool IsValidBinding(const AnthropicEndpointBinding& Binding) {
  return !Binding.Grant.Value.empty() && !Binding.Destination.Host.empty() &&
         Binding.Destination.Port != 0U &&
         !Binding.Destination.Path.empty() &&
         Binding.Destination.Path.front() == '/';
}

const json::object* AsObject(const json::value& Value) {
  return Value.is_object() ? &Value.get_object() : nullptr;
}

const json::string* AsString(const json::value& Value) {
  return Value.is_string() ? &Value.get_string() : nullptr;
}

std::optional<std::int64_t> AsInt64(const json::value& Value) {
  if (Value.is_int64()) {
    return Value.get_int64();
  }
  if (Value.is_uint64()) {
    const auto Unsigned = Value.get_uint64();
    if (Unsigned <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(Unsigned);
    }
  }
  return std::nullopt;
}

const json::value* FindValue(const json::object& Object, std::string_view Name) {
  return Object.if_contains(Name);
}

std::optional<json::value> ParseJsonText(std::string_view Text) {
  try {
    return json::parse(Text);
  } catch (...) {
    return std::nullopt;
  }
}

ProviderError ParseInBandError(std::string_view Data) {
  std::string Sanitized =
      "Anthropic returned an in-band stream error after partial output.";
  FailureCode Code = FailureCode::ProviderRejected;
  bool Retryable = false;
  const auto Parsed = ParseJsonText(Data);
  if (Parsed.has_value()) {
    if (const auto* Root = AsObject(*Parsed)) {
      if (const auto* Error = FindValue(*Root, "error");
          Error != nullptr && Error->is_object()) {
        const auto& ErrorObject = Error->get_object();
        if (const auto* Type = FindValue(ErrorObject, "type");
            Type != nullptr && Type->is_string() &&
            Type->get_string().c_str() == std::string("overloaded_error")) {
          Code = FailureCode::RateLimited;
          Retryable = true;
        }
        if (const auto* Message = FindValue(ErrorObject, "message");
            Message != nullptr && Message->is_string()) {
          Sanitized = std::string(Message->get_string().c_str());
        }
      }
    }
  }
  return MakeError(Code, Retryable, std::move(Sanitized));
}

json::object MakeTextBlock(std::string_view Text) {
  json::object Block;
  Block["type"] = "text";
  Block["text"] = Text;
  return Block;
}

ProviderResult<std::string> BuildWireBody(const CanonicalModelRequest& Request,
                                          const ModelProviderProfile& Profile) {
  if (!Request.ContentParts.empty()) {
    return MakeError(FailureCode::InvalidRequest, false,
                     "request-level content parts are not supported by the "
                     "Anthropic V1 message mapping.");
  }
  if (Request.Sampling.Temperature.has_value() ||
      Request.Sampling.TopP.has_value() ||
      Request.Sampling.TopK.has_value()) {
    return MakeError(FailureCode::UnsupportedCapability, false,
                     "Anthropic V1 rejects non-default sampling parameters.");
  }
  const auto MaximumOutputTokens = Request.Limit.MaximumOutputTokens.has_value()
                                       ? Request.Limit.MaximumOutputTokens
                                       : Profile.Limit.MaximumOutputTokens;
  if (!MaximumOutputTokens.has_value() || *MaximumOutputTokens == 0U) {
    return MakeError(FailureCode::InvalidRequest, false,
                     "Anthropic requests require a positive max_tokens.");
  }

  json::object Body;
  Body["model"] =
      Request.ModelName.empty() ? Profile.ModelName : Request.ModelName;
  Body["max_tokens"] =
      static_cast<std::int64_t>(*MaximumOutputTokens);
  Body["stream"] = true;
  std::string System = Request.SystemInstructions;
  if (!Request.DeveloperInstructions.empty()) {
    if (!System.empty()) {
      System.append("\n\n");
    }
    System.append(Request.DeveloperInstructions);
  }
  if (!System.empty()) {
    Body["system"] = System;
  }

  json::array Messages;
  std::optional<json::object> PendingUser;
  json::array PendingContent;
  bool PendingHasToolResult = false;
  const auto FlushPending = [&Messages, &PendingUser, &PendingContent,
                             &PendingHasToolResult]() {
    if (!PendingUser.has_value()) {
      return;
    }
    (*PendingUser)["content"] = std::move(PendingContent);
    Messages.push_back(std::move(*PendingUser));
    PendingUser.reset();
    PendingContent = json::array();
    PendingHasToolResult = false;
  };
  const auto EnsurePendingUser = [&PendingUser, &Messages, &PendingContent,
                                  &PendingHasToolResult]() {
    if (PendingUser.has_value()) {
      return;
    }
    static_cast<void>(Messages);
    PendingUser = json::object{};
    (*PendingUser)["role"] = "user";
    PendingContent = json::array();
    PendingHasToolResult = false;
  };

  for (const CanonicalMessage& Message : Request.Messages) {
    switch (Message.Role) {
    case MessageRole::System:
    case MessageRole::Developer:
      return MakeError(FailureCode::InvalidRequest, false,
                       "system and developer roles belong in the system "
                       "field.");
    case MessageRole::Tool: {
      if (!Message.ToolCallResultId.has_value()) {
        return MakeError(FailureCode::InvalidRequest, false,
                         "tool results require a tool call id.");
      }
      if (PendingUser.has_value() && !PendingHasToolResult) {
        FlushPending();
      }
      EnsurePendingUser();
      std::string ToolText;
      for (const ContentPart& Part : Message.ContentParts) {
        const auto* Text = std::get_if<TextContentPart>(&Part);
        if (Text == nullptr) {
          return MakeError(FailureCode::UnsupportedCapability, false,
                           "Anthropic image input is not enabled.");
        }
        ToolText.append(Text->Text);
      }
      if (ToolText.empty()) {
        return MakeError(FailureCode::InvalidRequest, false,
                         "tool result content is required.");
      }
      json::object Block;
      Block["type"] = "tool_result";
      Block["tool_use_id"] = Message.ToolCallResultId->Value;
      Block["content"] = ToolText;
      PendingContent.push_back(std::move(Block));
      PendingHasToolResult = true;
      break;
    }
    case MessageRole::Assistant: {
      FlushPending();
      json::object WireMessage;
      WireMessage["role"] = "assistant";
      json::array Content;
      for (const ContentPart& Part : Message.ContentParts) {
        const auto* Text = std::get_if<TextContentPart>(&Part);
        if (Text == nullptr) {
          return MakeError(FailureCode::UnsupportedCapability, false,
                           "Anthropic image input is not enabled.");
        }
        Content.push_back(MakeTextBlock(Text->Text));
      }
      for (const FunctionCall& Call : Message.FunctionCalls) {
        const auto Input = ParseJsonText(Call.Arguments.JsonText);
        if (!Input.has_value()) {
          return MakeError(FailureCode::InvalidRequest, false,
                           "tool call arguments must be valid JSON.");
        }
        json::object Block;
        Block["type"] = "tool_use";
        Block["id"] = Call.Id.Value;
        Block["name"] = Call.Name;
        Block["input"] = *Input;
        Content.push_back(std::move(Block));
      }
      WireMessage["content"] = std::move(Content);
      Messages.push_back(std::move(WireMessage));
      break;
    }
    case MessageRole::User: {
      EnsurePendingUser();
      if (PendingHasToolResult) {
        for (const ContentPart& Part : Message.ContentParts) {
          const auto* Text = std::get_if<TextContentPart>(&Part);
          if (Text == nullptr) {
            return MakeError(FailureCode::UnsupportedCapability, false,
                             "Anthropic image input is not enabled.");
          }
          PendingContent.push_back(MakeTextBlock(Text->Text));
        }
        FlushPending();
      } else {
        for (const ContentPart& Part : Message.ContentParts) {
          const auto* Text = std::get_if<TextContentPart>(&Part);
          if (Text == nullptr) {
            return MakeError(FailureCode::UnsupportedCapability, false,
                             "Anthropic image input is not enabled.");
          }
          PendingContent.push_back(MakeTextBlock(Text->Text));
        }
        FlushPending();
      }
      break;
    }
    }
  }
  FlushPending();
  Body["messages"] = std::move(Messages);

  if (!Request.ToolDefinitions.empty()) {
    json::array Tools;
    for (const ToolDefinition& Definition : Request.ToolDefinitions) {
      const auto Schema = ParseJsonText(Definition.Parameters.JsonText);
      if (!Schema.has_value()) {
        return MakeError(FailureCode::InvalidRequest, false,
                         "tool schema must be valid JSON.");
      }
      json::object Tool;
      Tool["name"] = Definition.Name;
      Tool["description"] = Definition.Description;
      Tool["input_schema"] = *Schema;
      Tools.push_back(std::move(Tool));
    }
    Body["tools"] = std::move(Tools);
  }

  for (const RegisteredProviderOption& Option : Request.ProviderOptions) {
    if (Option.Id.Value == "cache_control") {
      return MakeError(FailureCode::UnsupportedCapability, false,
                       "Anthropic cache_control requires block-level support "
                       "that is not available in V1.");
    }
    if (Option.Id.Value != "thinking") {
      return MakeError(FailureCode::UnsupportedCapability, false,
                       "unrecognized provider option.");
    }
    const auto* Text = std::get_if<std::string>(&Option.Value);
    if (Text == nullptr) {
      return MakeError(FailureCode::InvalidProfile, false,
                       "provider option must be a JSON text value.");
    }
    const auto Parsed = ParseJsonText(*Text);
    if (!Parsed.has_value()) {
      return MakeError(FailureCode::InvalidProfile, false,
                       "provider option JSON is invalid.");
    }
    Body[Option.Id.Value] = *Parsed;
  }

  return json::serialize(Body);
}

struct UsageParse final {
  UsageSnapshot Usage;
  bool Present{false};
};

UsageParse ParseUsage(const json::object& Wire,
                      UsageCompleteness Completeness) {
  UsageParse Result;
  if (const auto* Input = FindValue(Wire, "input_tokens")) {
    if (const auto Value = AsInt64(*Input); Value.has_value() && *Value >= 0) {
      Result.Usage.InputTokens = static_cast<std::uint64_t>(*Value);
      Result.Present = true;
    }
  }
  if (const auto* Output = FindValue(Wire, "output_tokens")) {
    if (const auto Value = AsInt64(*Output);
        Value.has_value() && *Value >= 0) {
      Result.Usage.OutputTokens = static_cast<std::uint64_t>(*Value);
      Result.Present = true;
    }
  }
  if (const auto* CacheWrite = FindValue(Wire, "cache_creation_input_tokens")) {
    if (const auto Value = AsInt64(*CacheWrite);
        Value.has_value() && *Value >= 0) {
      Result.Usage.CachedInputWriteTokens =
          static_cast<std::uint64_t>(*Value);
      Result.Present = true;
    }
  }
  if (const auto* CacheRead = FindValue(Wire, "cache_read_input_tokens")) {
    if (const auto Value = AsInt64(*CacheRead);
        Value.has_value() && *Value >= 0) {
      Result.Usage.CachedInputReadTokens = static_cast<std::uint64_t>(*Value);
      Result.Present = true;
    }
  }
  Result.Usage.Completeness =
      Result.Present ? Completeness : UsageCompleteness::Unavailable;
  return Result;
}

struct BlockState final {
  enum class Kind { Text, Tool, Thinking };
  Kind Type{Kind::Text};
  ToolCallId Call;
  std::string Name;
  std::string JsonBuffer;
  bool Closed{false};
  bool JsonInvalid{false};
};

class StreamState final {
public:
  StreamState(const RequestId& Request, const AgentTurnId& Turn,
              const EventSink& Sink)
      : Request_(Request), Turn_(Turn), Sink_(Sink) {}

  [[nodiscard]] std::uint64_t NextSequence() const noexcept {
    return NextSequence_;
  }

  std::optional<CanonicalTerminalEvent> Feed(std::string_view EventName,
                                             std::string_view Data) {
    if (Failed_) {
      return std::nullopt;
    }
    if (Terminal_.has_value()) {
      return Fail(FailureCode::InvalidStream,
                  "duplicate terminal event received.");
    }
    json::value Parsed;
    try {
      Parsed = json::parse(Data);
    } catch (...) {
      return Fail(FailureCode::InvalidStream,
                  "Anthropic SSE data is not valid JSON.");
    }
    const auto* Object = AsObject(Parsed);
    if (Object == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "Anthropic SSE data must be an object.");
    }
    const auto* TypeValue = FindValue(*Object, "type");
    const auto* Type = TypeValue != nullptr ? AsString(*TypeValue) : nullptr;
    if (Type == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "Anthropic SSE data is missing a type.");
    }
    const std::string_view Name = Type->c_str();
    if (!EventName.empty() && Name != EventName) {
      return Fail(FailureCode::InvalidStream,
                  "SSE event name and JSON type disagree.");
    }
    if (Name == "ping") {
      return std::nullopt;
    }
    if (Name == "message_start") {
      return OnMessageStart(*Object);
    }
    if (Name == "content_block_start") {
      return OnContentBlockStart(*Object);
    }
    if (Name == "content_block_delta") {
      return OnContentBlockDelta(*Object);
    }
    if (Name == "content_block_stop") {
      return OnContentBlockStop(*Object);
    }
    if (Name == "message_delta") {
      return OnMessageDelta(*Object);
    }
    if (Name == "message_stop") {
      return OnMessageStop();
    }
    if (Name == "error") {
      return FailWith(ParseInBandError(Data));
    }
    return Fail(FailureCode::InvalidStream,
                "Unrecognized Anthropic SSE event type.");
  }

  [[nodiscard]] bool HasTerminal() const noexcept {
    return Terminal_.has_value();
  }

  [[nodiscard]] std::optional<CanonicalTerminalEvent> Terminal() const {
    return Terminal_;
  }

  std::optional<CanonicalTerminalEvent> ForceInvalidStream(
      std::string Description) {
    return Fail(FailureCode::InvalidStream, std::move(Description));
  }

private:
  std::optional<CanonicalTerminalEvent> FailWith(ProviderError Error) {
    Failed_ = true;
    Terminal_ = ResponseFailedEvent{
        .Metadata = MakeMetadata(Request_, Turn_, NextSequence_),
        .Failure = std::move(Error),
    };
    return Terminal_;
  }

  std::optional<CanonicalTerminalEvent> Fail(FailureCode Code,
                                             std::string Description) {
    Failed_ = true;
    Terminal_ = ResponseFailedEvent{
        .Metadata = MakeMetadata(Request_, Turn_, NextSequence_),
        .Failure = MakeError(Code, false, std::move(Description)),
    };
    return Terminal_;
  }

  template <typename Event>
  void Emit(Event&& Value) {
    Sink_(CanonicalNonTerminalEvent{std::forward<Event>(Value)});
    ++NextSequence_;
  }

  std::optional<CanonicalTerminalEvent> OnMessageStart(
      const json::object& Object) {
    if (Started_) {
      return Fail(FailureCode::InvalidStream,
                  "duplicate message_start event.");
    }
    Started_ = true;
    const auto* Message = FindValue(Object, "message");
    const auto* MessageObject =
        Message != nullptr ? AsObject(*Message) : nullptr;
    if (MessageObject == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "message_start is missing the message object.");
    }
    ResponseStartedEvent Started;
    Started.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
    if (const auto* Id = FindValue(*MessageObject, "id");
        Id != nullptr && Id->is_string()) {
      Started.ProviderResponse =
          ProviderResponseId{.Value = std::string(Id->get_string().c_str())};
    }
    Emit(Started);
    if (const auto* Usage = FindValue(*MessageObject, "usage");
        Usage != nullptr && Usage->is_object()) {
      auto Snapshot =
          ParseUsage(Usage->get_object(), UsageCompleteness::Partial);
      if (Snapshot.Present) {
        LastUsage_ = Snapshot.Usage;
        UsageUpdatedEvent Updated;
        Updated.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
        Updated.Usage = Snapshot.Usage;
        Emit(Updated);
      }
    }
    return std::nullopt;
  }

  std::optional<CanonicalTerminalEvent> OnContentBlockStart(
      const json::object& Object) {
    if (!Started_) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_start before message_start.");
    }
    const auto* Index = FindValue(Object, "index");
    const auto* BlockValue = FindValue(Object, "content_block");
    const auto* BlockObject =
        BlockValue != nullptr ? AsObject(*BlockValue) : nullptr;
    if (Index == nullptr || BlockObject == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_start is malformed.");
    }
    const auto IndexValue = AsInt64(*Index);
    if (!IndexValue.has_value() || *IndexValue < 0) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_start index is invalid.");
    }
    const auto* Type = FindValue(*BlockObject, "type");
    const auto* TypeText = Type != nullptr ? AsString(*Type) : nullptr;
    if (TypeText == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_start is missing a block type.");
    }
    BlockState Block;
    const std::string_view BlockType = TypeText->c_str();
    if (BlockType == "text") {
      Block.Type = BlockState::Kind::Text;
    } else if (BlockType == "thinking" || BlockType == "redacted_thinking") {
      Block.Type = BlockState::Kind::Thinking;
    } else if (BlockType == "tool_use") {
      Block.Type = BlockState::Kind::Tool;
      const auto* Id = FindValue(*BlockObject, "id");
      const auto* Name = FindValue(*BlockObject, "name");
      const auto* IdText = Id != nullptr ? AsString(*Id) : nullptr;
      const auto* NameText = Name != nullptr ? AsString(*Name) : nullptr;
      if (IdText == nullptr || NameText == nullptr) {
        return Fail(FailureCode::InvalidStream,
                    "tool_use block is missing its identity.");
      }
      Block.Call = ToolCallId{.Value = std::string(IdText->c_str())};
      Block.Name = std::string(NameText->c_str());
      ToolCallStartedEvent ToolStarted;
      ToolStarted.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
      ToolStarted.Item = ItemId{
          .Value = "anthropic:block:" + std::to_string(*IndexValue)};
      ToolStarted.Call = Block.Call;
      ToolStarted.ToolName = Block.Name;
      Emit(ToolStarted);
    } else if (BlockType == "server_tool_use" ||
               BlockType == "server_tool_result") {
      return Fail(FailureCode::UnsupportedCapability,
                  "Anthropic server-side tools are outside the V1 profile.");
    } else {
      return Fail(FailureCode::UnsupportedCapability,
                  "Unrecognized Anthropic content block.");
    }
    Blocks_[static_cast<int>(*IndexValue)] = std::move(Block);
    return std::nullopt;
  }

  std::optional<CanonicalTerminalEvent> OnContentBlockDelta(
      const json::object& Object) {
    const auto* Index = FindValue(Object, "index");
    const auto* Delta = FindValue(Object, "delta");
    const auto* DeltaObject = Delta != nullptr ? AsObject(*Delta) : nullptr;
    if (Index == nullptr || DeltaObject == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_delta is malformed.");
    }
    const auto IndexValue = AsInt64(*Index);
    if (!IndexValue.has_value()) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_delta index is invalid.");
    }
    auto Block = Blocks_.find(static_cast<int>(*IndexValue));
    if (Block == Blocks_.end()) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_delta references an unknown block.");
    }
    const auto* Type = FindValue(*DeltaObject, "type");
    const auto* TypeText = Type != nullptr ? AsString(*Type) : nullptr;
    if (TypeText == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_delta is missing a delta type.");
    }
    const std::string_view DeltaType = TypeText->c_str();
    const std::string Item = "anthropic:block:" +
                             std::to_string(*IndexValue);
    if (DeltaType == "text_delta" &&
        Block->second.Type == BlockState::Kind::Text) {
      const auto* TextValue = FindValue(*DeltaObject, "text");
      const auto* Text = TextValue != nullptr ? AsString(*TextValue) : nullptr;
      if (Text == nullptr) {
        return Fail(FailureCode::InvalidStream,
                    "text_delta is missing text.");
      }
      TextDeltaEvent DeltaEvent;
      DeltaEvent.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
      DeltaEvent.Item = ItemId{.Value = Item};
      DeltaEvent.Utf8Delta = std::string(Text->c_str());
      Emit(DeltaEvent);
      return std::nullopt;
    }
    if (DeltaType == "input_json_delta" &&
        Block->second.Type == BlockState::Kind::Tool) {
      const auto* TextValue = FindValue(*DeltaObject, "partial_json");
      const auto* Text = TextValue != nullptr ? AsString(*TextValue) : nullptr;
      if (Text == nullptr) {
        return Fail(FailureCode::InvalidStream,
                    "input_json_delta is missing partial_json.");
      }
      Block->second.JsonBuffer.append(Text->c_str());
      ToolArgumentDeltaEvent DeltaEvent;
      DeltaEvent.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
      DeltaEvent.Item = ItemId{.Value = Item};
      DeltaEvent.Call = Block->second.Call;
      DeltaEvent.JsonTextDelta = std::string(Text->c_str());
      Emit(DeltaEvent);
      return std::nullopt;
    }
    if ((DeltaType == "thinking_delta" ||
         DeltaType == "signature_delta") &&
        Block->second.Type == BlockState::Kind::Thinking) {
      return std::nullopt;
    }
    return Fail(FailureCode::InvalidStream,
                "content block delta type does not match the block.");
  }

  std::optional<CanonicalTerminalEvent> OnContentBlockStop(
      const json::object& Object) {
    const auto* Index = FindValue(Object, "index");
    const auto IndexValue =
        Index != nullptr ? AsInt64(*Index) : std::nullopt;
    if (!IndexValue.has_value()) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_stop index is invalid.");
    }
    auto Block = Blocks_.find(static_cast<int>(*IndexValue));
    if (Block == Blocks_.end() || Block->second.Closed) {
      return Fail(FailureCode::InvalidStream,
                  "content_block_stop references an unknown block.");
    }
    Block->second.Closed = true;
    if (Block->second.Type == BlockState::Kind::Tool) {
      if (Block->second.JsonBuffer.empty()) {
        Block->second.JsonInvalid = true;
        return std::nullopt;
      }
      try {
        static_cast<void>(json::parse(Block->second.JsonBuffer));
      } catch (...) {
        Block->second.JsonInvalid = true;
        return std::nullopt;
      }
      ToolCallReadyEvent Ready;
      Ready.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
      Ready.Item = ItemId{
          .Value = "anthropic:block:" + std::to_string(*IndexValue)};
      Ready.Call = Block->second.Call;
      Ready.ToolName = Block->second.Name;
      Ready.Arguments = JsonValue{.JsonText = Block->second.JsonBuffer};
      Emit(Ready);
    }
    return std::nullopt;
  }

  std::optional<CanonicalTerminalEvent> OnMessageDelta(
      const json::object& Object) {
    if (!Started_) {
      return Fail(FailureCode::InvalidStream,
                  "message_delta before message_start.");
    }
    const auto* Delta = FindValue(Object, "delta");
    const auto* DeltaObject = Delta != nullptr ? AsObject(*Delta) : nullptr;
    if (DeltaObject == nullptr) {
      return Fail(FailureCode::InvalidStream,
                  "message_delta is missing the delta object.");
    }
    const auto* StopReasonValue = FindValue(*DeltaObject, "stop_reason");
    const auto* StopReason =
        StopReasonValue != nullptr && StopReasonValue->is_string()
            ? &StopReasonValue->get_string()
            : nullptr;
    const auto* UsageValue = FindValue(Object, "usage");
    const auto* UsageObject =
        UsageValue != nullptr ? AsObject(*UsageValue) : nullptr;
    if (StopReason == nullptr) {
      if (UsageObject != nullptr) {
        auto Snapshot = ParseUsage(*UsageObject, UsageCompleteness::Partial);
        if (Snapshot.Present) {
          LastUsage_ = Snapshot.Usage;
          UsageUpdatedEvent Updated;
          Updated.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
          Updated.Usage = Snapshot.Usage;
          Emit(Updated);
        }
      }
      return std::nullopt;
    }

    const std::string_view Reason = StopReason->c_str();
    if (Reason != "end_turn" && Reason != "tool_use" && Reason != "max_tokens" &&
        Reason != "model_context_window_exceeded" &&
        Reason != "stop_sequence" && Reason != "refusal" &&
        Reason != "pause_turn") {
      return Fail(FailureCode::InvalidStream,
                  "Unrecognized Anthropic stop_reason.");
    }
    StopReason_ = std::string(Reason);
    if (const auto* StopSequence = FindValue(*DeltaObject, "stop_sequence");
        StopSequence != nullptr && StopSequence->is_string()) {
      StopSequence_ = std::string(StopSequence->get_string().c_str());
    }
    if (UsageObject != nullptr) {
      auto Snapshot = ParseUsage(*UsageObject, UsageCompleteness::Final);
      if (Snapshot.Present) {
        FinalUsage_ = Snapshot.Usage;
      }
    }
    if (Reason == "pause_turn") {
      return Fail(FailureCode::UnsupportedCapability,
                  "Anthropic server-tool continuation is unsupported in V1.");
    }
    if (Reason == "refusal") {
      const auto ParsedDetails =
          ParseRefusalDetails(FindValue(Object, "stop_details"));
      if (!ParsedDetails.has_value()) {
        return Fail(FailureCode::InvalidStream,
                    "Anthropic refusal is missing valid stop_details.");
      }
      RefusalDetails_ = *ParsedDetails;
      return std::nullopt;
    }
    if ((Reason == "end_turn" || Reason == "tool_use") &&
        FinalUsage_.has_value()) {
      UsageUpdatedEvent Updated;
      Updated.Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
      Updated.Usage = *FinalUsage_;
      Emit(Updated);
    }
    return std::nullopt;
  }

  std::optional<CanonicalTerminalEvent> OnMessageStop() {
    if (!Started_ || !StopReason_.has_value()) {
      return Fail(FailureCode::InvalidStream,
                  "message_stop arrived without a stop_reason.");
    }
    for (const auto& Entry : Blocks_) {
      if (Entry.second.Type == BlockState::Kind::Tool &&
          !Entry.second.Closed) {
        return Fail(FailureCode::InvalidStream,
                    "message_stop arrived before a tool_use block closed.");
      }
    }
    const UsageSnapshot Usage =
        FinalUsage_.value_or(LastUsage_.value_or(UsageSnapshot{}));
    const auto Metadata = MakeMetadata(Request_, Turn_, NextSequence_);
    if (*StopReason_ == "end_turn") {
      Terminal_ = ResponseCompletedEvent{
          .Metadata = Metadata,
          .Kind = CompletionKind::FinalAnswer,
          .StopSequenceCode = std::nullopt,
          .FinalUsage = FinalUsage_,
          .StopDetails = std::nullopt,
      };
    } else if (*StopReason_ == "tool_use") {
      for (const auto& Entry : Blocks_) {
        if (Entry.second.Type == BlockState::Kind::Tool &&
            Entry.second.JsonInvalid) {
          return Fail(FailureCode::InvalidStream,
                      "tool_use completed without valid JSON arguments.");
        }
      }
      Terminal_ = ResponseCompletedEvent{
          .Metadata = Metadata,
          .Kind = CompletionKind::ToolHandoff,
          .StopSequenceCode = std::nullopt,
          .FinalUsage = FinalUsage_,
          .StopDetails = std::nullopt,
      };
    } else if (*StopReason_ == "stop_sequence") {
      Terminal_ = ResponseCompletedEvent{
          .Metadata = Metadata,
          .Kind = CompletionKind::FinalAnswer,
          .StopSequenceCode =
              StopSequence_.has_value()
                  ? std::optional<StableReasonCode>(
                        StableReasonCode{.Value = *StopSequence_})
                  : std::nullopt,
          .FinalUsage = FinalUsage_,
          .StopDetails = std::nullopt,
      };
    } else if (*StopReason_ == "max_tokens") {
      Terminal_ = ResponseOutputLimitedEvent{
          .Metadata = Metadata,
          .Kind = LimitKind::OutputTokens,
          .Usage = Usage,
          .SafeGeneratedText = std::nullopt,
      };
    } else if (*StopReason_ == "model_context_window_exceeded") {
      Terminal_ = ResponseOutputLimitedEvent{
          .Metadata = Metadata,
          .Kind = LimitKind::ContextWindow,
          .Usage = Usage,
          .SafeGeneratedText = std::nullopt,
      };
    } else if (*StopReason_ == "refusal") {
      if (!RefusalDetails_.has_value()) {
        return Fail(FailureCode::InvalidStream,
                    "Anthropic refusal lacks stop details.");
      }
      Terminal_ = ResponseCompletedEvent{
          .Metadata = Metadata,
          .Kind = CompletionKind::Refusal,
          .StopSequenceCode = std::nullopt,
          .FinalUsage = FinalUsage_,
          .StopDetails = RefusalDetails_,
      };
    } else {
      return Fail(FailureCode::InvalidStream,
                  "Unrecognized Anthropic stop_reason.");
    }
    return Terminal_;
  }

  static std::optional<RefusalStopDetails> ParseRefusalDetails(
      const json::value* Details) {
    if (Details == nullptr) {
      return std::nullopt;
    }
    const auto* Object = AsObject(*Details);
    if (Object == nullptr) {
      return std::nullopt;
    }
    for (const auto& Entry : *Object) {
      const std::string_view Key(Entry.key().data(), Entry.key().size());
      if (Key != "type" && Key != "category" && Key != "explanation") {
        return std::nullopt;
      }
    }
    const auto* Type = FindValue(*Object, "type");
    const auto* TypeText = Type != nullptr ? AsString(*Type) : nullptr;
    if (TypeText == nullptr || TypeText->c_str() != std::string("refusal")) {
      return std::nullopt;
    }
    const auto* Explanation = FindValue(*Object, "explanation");
    if (Explanation != nullptr && !Explanation->is_string()) {
      return std::nullopt;
    }
    RefusalStopDetails Result;
    Result.Type = RefusalStopDetailsType::Refusal;
    Result.Category = RefusalCategory::Unclassified;
    const auto* Category = FindValue(*Object, "category");
    if (Category != nullptr && Category->is_string()) {
      const std::string_view Name = Category->get_string().c_str();
      if (Name == "general_harms") {
        Result.Category = RefusalCategory::GeneralHarms;
      } else if (Name == "cyber") {
        Result.Category = RefusalCategory::Cyber;
      } else if (Name == "bio") {
        Result.Category = RefusalCategory::Bio;
      } else if (Name == "frontier_llm") {
        Result.Category = RefusalCategory::FrontierLlm;
      } else if (Name == "reasoning_extraction") {
        Result.Category = RefusalCategory::ReasoningExtraction;
      }
    } else if (Category != nullptr && !Category->is_null()) {
      return std::nullopt;
    }
    return Result;
  }

  RequestId Request_;
  AgentTurnId Turn_;
  EventSink Sink_;
  std::uint64_t NextSequence_{1};
  bool Started_{false};
  bool Failed_{false};
  std::map<int, BlockState> Blocks_;
  std::optional<std::string> StopReason_;
  std::optional<std::string> StopSequence_;
  std::optional<UsageSnapshot> LastUsage_;
  std::optional<UsageSnapshot> FinalUsage_;
  std::optional<RefusalStopDetails> RefusalDetails_;
  std::optional<CanonicalTerminalEvent> Terminal_;
};

class SseParser final {
public:
  explicit SseParser(StreamState& State) : State_(State) {}

  void Push(std::span<const std::byte> Chunk) {
    TotalBytes_ += Chunk.size();
    if (TotalBytes_ > MaximumStreamBytes) {
      Terminal_ = State_.ForceInvalidStream(
          "Anthropic SSE stream exceeded the V1 stream limit.");
      Buffer_.clear();
      return;
    }
    for (const std::byte Byte : Chunk) {
      Buffer_.push_back(static_cast<char>(Byte));
    }
    while (true) {
      const auto Newline = Buffer_.find('\n');
      if (Newline == std::string::npos) {
        break;
      }
      std::string Line = Buffer_.substr(0, Newline);
      Buffer_.erase(0, Newline + 1);
      if (!Line.empty() && Line.back() == '\r') {
        Line.pop_back();
      }
      if (Line.empty()) {
        Dispatch();
        continue;
      }
      if (Line.size() > MaximumLineBytes) {
        Terminal_ = State_.ForceInvalidStream(
            "Anthropic SSE line exceeded the V1 line limit.");
        Buffer_.clear();
        return;
      }
      if (Line.rfind("event:", 0) == 0) {
        EventName_ = Trim(Line.substr(6));
      } else if (Line.rfind("data:", 0) == 0) {
        if (!Data_.empty()) {
          Data_.push_back('\n');
        }
        Data_.append(Trim(Line.substr(5)));
      }
    }
  }

  [[nodiscard]] std::optional<CanonicalTerminalEvent> Terminal() const {
    return Terminal_;
  }

private:
  static std::string_view Trim(std::string_view Text) {
    while (!Text.empty() && Text.front() == ' ') {
      Text.remove_prefix(1);
    }
    while (!Text.empty() && Text.back() == '\r') {
      Text.remove_suffix(1);
    }
    return Text;
  }

  void Dispatch() {
    if (Data_.empty() && EventName_.empty()) {
      return;
    }
    if (const auto NewTerminal = State_.Feed(EventName_, Data_);
        NewTerminal.has_value()) {
      Terminal_ = NewTerminal;
    }
    EventName_.clear();
    Data_.clear();
  }

  StreamState& State_;
  std::string Buffer_;
  std::string EventName_;
  std::string Data_;
  std::optional<CanonicalTerminalEvent> Terminal_;
  std::size_t TotalBytes_{0};
  static constexpr std::size_t MaximumLineBytes = 64U * 1024U;
  static constexpr std::size_t MaximumStreamBytes = 8U * 1024U * 1024U;
};

ProviderError MapHttpFailure(std::uint16_t Status, std::string_view Body) {
  std::string SanitizedMessage;
  FailureCode Code = FailureCode::ProviderRejected;
  bool Retryable = false;
  const auto Parsed = ParseJsonText(Body);
  if (Parsed.has_value()) {
    if (const auto* Root = AsObject(*Parsed)) {
      if (const auto* Error = FindValue(*Root, "error");
          Error != nullptr && Error->is_object()) {
        const auto& ErrorObject = Error->get_object();
        if (const auto* Type = FindValue(ErrorObject, "type");
            Type != nullptr && Type->is_string() &&
            Type->get_string().c_str() == std::string("overloaded_error")) {
          Code = FailureCode::RateLimited;
          Retryable = true;
        }
        if (const auto* Message = FindValue(ErrorObject, "message");
            Message != nullptr && Message->is_string()) {
          SanitizedMessage = std::string(Message->get_string().c_str());
        }
      }
    }
  }
  if (Code == FailureCode::ProviderRejected) {
    switch (Status) {
    case 401:
    case 403:
      Code = FailureCode::AuthenticationFailure;
      break;
    case 429:
      Code = FailureCode::RateLimited;
      Retryable = true;
      break;
    case 500:
      Code = FailureCode::TransportFailure;
      Retryable = true;
      break;
    case 529:
      Code = FailureCode::RateLimited;
      Retryable = true;
      break;
    default:
      break;
    }
  }
  if (SanitizedMessage.empty()) {
    SanitizedMessage = "Anthropic request failed with HTTP status " +
                       std::to_string(Status) + ".";
  }
  return MakeError(Code, Retryable, std::move(SanitizedMessage));
}

} // namespace

AnthropicMessagesProvider::AnthropicMessagesProvider(
    HttpTransportPort& Transport, AnthropicEndpointBinding Binding) noexcept
    : Transport_(Transport),
      Binding_(std::move(Binding)),
      BindingValid_(IsValidBinding(Binding_)) {}

ProviderResult<ProviderCapabilities>
AnthropicMessagesProvider::GetCapabilities(
    const ProviderProfileId& ProfileId) const {
  if (ProfileId.Value.empty()) {
    return MakeError(FailureCode::InvalidProfile, false,
                     "provider profile id is required.");
  }
  return ProviderCapabilities{.Streaming = true,
                              .ToolCalls = true,
                              .Vision = false,
                              .Usage = true,
                              .Cancellation = true,
                              .OutputLimit = true};
}

ProviderResult<std::monostate> AnthropicMessagesProvider::ValidateProfile(
    const ModelProviderProfile& Profile) const {
  if (Profile.Protocol != ProviderProtocol::AnthropicMessages) {
    return MakeError(FailureCode::InvalidProfile, false,
                     "profile protocol is not AnthropicMessages.");
  }
  if (Profile.Id.Value.empty() || Profile.ModelName.empty() ||
      Profile.ProtocolVersion.empty()) {
    return MakeError(FailureCode::InvalidProfile, false,
                     "profile id, model, and Anthropic version are required.");
  }
  if (Profile.Authentication != AuthenticationScheme::XApiKeyHeader) {
    return MakeError(FailureCode::InvalidProfile, false,
                     "Anthropic profiles require x-api-key authentication.");
  }
  if (Profile.Sampling.Temperature.has_value() ||
      Profile.Sampling.TopP.has_value() ||
      Profile.Sampling.TopK.has_value()) {
    return MakeError(FailureCode::UnsupportedCapability, false,
                     "Anthropic V1 rejects non-default sampling parameters.");
  }
  return std::monostate{};
}

CanonicalTerminalEvent AnthropicMessagesProvider::StreamTurn(
    const ModelProviderProfile& Profile, const CredentialLease& Lease,
    const CanonicalModelRequest& Request, std::stop_token CancellationToken,
    const EventSink& Sink) {
  if (CancellationToken.stop_requested()) {
    return ResponseCancelledEvent{
        .Metadata = MakeMetadata(Request.Id, Request.TurnId, 1),
        .Reason = CancelReasonCode{.Code = StableReasonCode{
                                       .Value = "CancelledBeforeSend"}},
    };
  }
  if (const auto Validation = ValidateProfile(Profile);
      std::holds_alternative<ProviderError>(Validation)) {
    return FailedTerminal(Request.Id, Request.TurnId, 1,
                          std::get<ProviderError>(Validation));
  }
  if (!BindingValid_) {
    return FailedTerminal(
        Request.Id, Request.TurnId, 1,
        MakeError(FailureCode::TransportFailure, false,
                  "EndpointNotGranted: Core endpoint binding is missing or "
                  "invalid."));
  }
  auto Body = BuildWireBody(Request, Profile);
  if (const auto* Error = std::get_if<ProviderError>(&Body)) {
    return FailedTerminal(Request.Id, Request.TurnId, 1, *Error);
  }

  const std::string Secret(Lease.GetAuthenticationSecret());
  std::vector<TransportHeader> Headers{
      TransportHeader{.Name = "content-type", .Value = "application/json"},
      TransportHeader{.Name = "accept", .Value = "text/event-stream"},
      TransportHeader{.Name = "anthropic-version",
                      .Value = Profile.ProtocolVersion},
      TransportHeader{.Name = "x-api-key", .Value = Secret},
  };
  TransportRequest WireRequest{
      .Id = Platform::Network::TransportRequestId{.Value = Request.Id.Value},
      .Grant = Binding_.Grant,
      .Destination = Binding_.Destination,
      .Headers = std::move(Headers),
      .BodyUtf8 = std::get<std::string>(std::move(Body)),
      .Limits = TransportRequestLimits{},
      .Timeouts = TransportTimeouts{},
      .Proxy = TransportProxyMode::System,
  };

  StreamState State(Request.Id, Request.TurnId, Sink);
  SseParser Parser(State);
  std::string CollectedBody;
  const auto OnHead = [](const ResponseHeadMetadata&) {};
  const auto OnChunk = [&Parser, &CollectedBody](
                           std::span<const std::byte> Chunk) {
    CollectedBody.append(reinterpret_cast<const char*>(Chunk.data()),
                         Chunk.size());
    Parser.Push(Chunk);
  };
  const auto TransportResult =
      Transport_.Stream(WireRequest, OnHead, OnChunk, CancellationToken);

  if (const auto Terminal = Parser.Terminal(); Terminal.has_value()) {
    return *Terminal;
  }
  if (std::holds_alternative<CancelledTransport>(TransportResult)) {
    const auto& Cancelled = std::get<CancelledTransport>(TransportResult);
    if (!Cancelled.RequestMayHaveReachedEndpoint) {
      return ResponseCancelledEvent{
          .Metadata = MakeMetadata(Request.Id, Request.TurnId, 1),
          .Reason = CancelReasonCode{.Code = StableReasonCode{
                                         .Value = "CancelledBeforeSend"}},
      };
    }
    return ResponseOutcomeUnknownEvent{
        .Metadata =
            MakeMetadata(Request.Id, Request.TurnId, State.NextSequence()),
        .Reason = OutcomeUnknownReasonCode{
            .Code = StableReasonCode{.Value = "LocalCancellationUnconfirmed"}},
    };
  }
  if (std::holds_alternative<UnknownOutcomeTransport>(TransportResult)) {
    return ResponseOutcomeUnknownEvent{
        .Metadata =
            MakeMetadata(Request.Id, Request.TurnId, State.NextSequence()),
        .Reason = OutcomeUnknownReasonCode{
            .Code = StableReasonCode{.Value = "StreamEndedBeforeTerminal"}},
    };
  }
  if (const auto* Failed =
          std::get_if<FailedTransport>(&TransportResult)) {
    const bool Retryable =
        Failed->Code == TransportFailureCode::DnsFailure ||
        Failed->Code == TransportFailureCode::ConnectFailure ||
        Failed->Code == TransportFailureCode::ResponseReadFailure;
    return FailedTerminal(Request.Id, Request.TurnId, 1,
                          MakeError(FailureCode::TransportFailure, Retryable,
                                    "Anthropic transport failed."));
  }
  if (std::holds_alternative<TimedOutTransport>(TransportResult)) {
    return FailedTerminal(Request.Id, Request.TurnId, 1,
                          MakeError(FailureCode::TransportFailure, true,
                                    "Anthropic transport timed out."));
  }
  if (std::holds_alternative<RedirectDeniedTransport>(TransportResult)) {
    return FailedTerminal(Request.Id, Request.TurnId, 1,
                          MakeError(FailureCode::TransportFailure, false,
                                    "Anthropic redirects are not permitted."));
  }
  if (const auto* Completed =
          std::get_if<CompletedTransport>(&TransportResult)) {
    if (Completed->StatusCode != 200) {
      return FailedTerminal(Request.Id, Request.TurnId, 1,
                            MapHttpFailure(Completed->StatusCode,
                                           CollectedBody));
    }
  }
  if (const auto Terminal = Parser.Terminal(); Terminal.has_value()) {
    return *Terminal;
  }
  return ResponseOutcomeUnknownEvent{
      .Metadata =
          MakeMetadata(Request.Id, Request.TurnId, State.NextSequence()),
      .Reason = OutcomeUnknownReasonCode{
          .Code = StableReasonCode{.Value = "StreamEndedBeforeTerminal"}},
  };
}

ProviderResult<ProviderHealthStatus>
AnthropicMessagesProvider::HealthCheck(const ModelProviderProfile& Profile) {
  const auto Validation = ValidateProfile(Profile);
  if (std::holds_alternative<ProviderError>(Validation)) {
    return std::get<ProviderError>(Validation);
  }
  return ProviderHealthStatus{.State = ProviderHealthState::Available};
}

} // namespace EmulativeDialog::Providers
