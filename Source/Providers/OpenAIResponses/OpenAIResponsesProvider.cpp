#include "Source/Providers/OpenAIResponses/OpenAIResponsesProvider.hpp"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EmulativeDialog::Providers::OpenAIResponses {
namespace {

namespace Json = boost::json;
namespace Net = Platform::Network;

constexpr std::size_t MaximumSseFrameBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumDescriptionBytes = 240U;

ProviderError InvalidProfileError() {
    return ProviderError{
        .Code = FailureCode::InvalidProfile,
        .Retryable = false,
        .SanitizedDescription = "Invalid OpenAI Responses profile."};
}

CanonicalTerminalEvent FailedTerminal(
    const ProviderEventMetadata& Metadata, FailureCode Code, bool Retryable,
    std::string Description) {
    return ResponseFailedEvent{
        .Metadata = Metadata,
        .Failure =
            ProviderError{.Code = Code,
                          .Retryable = Retryable,
                          .SanitizedDescription = std::move(Description)}};
}

std::string SanitizeDescription(std::string_view Text) {
    std::string Result;
    Result.reserve(std::min(Text.size(), MaximumDescriptionBytes));
    for (const char Value : Text) {
        const auto Byte = static_cast<unsigned char>(Value);
        if (Byte < 0x20U && Value != ' ') {
            continue;
        }
        Result.push_back(Value);
        if (Result.size() >= MaximumDescriptionBytes) {
            break;
        }
    }
    return Result.empty() ? std::string("Provider request failed.")
                          : Result;
}

std::optional<std::string_view> StringField(const Json::object& Object,
                                            std::string_view Key) {
    const Json::value* Value = Object.if_contains(Key);
    if (Value == nullptr || !Value->is_string()) {
        return std::nullopt;
    }
    const Json::string& Text = Value->as_string();
    return std::string_view(Text.data(), Text.size());
}

std::optional<std::int64_t> IntField(const Json::object& Object,
                                     std::string_view Key) {
    const Json::value* Value = Object.if_contains(Key);
    if (Value == nullptr || !Value->is_int64()) {
        return std::nullopt;
    }
    return Value->as_int64();
}

const Json::object* ObjectField(const Json::object& Object,
                                std::string_view Key) {
    const Json::value* Value = Object.if_contains(Key);
    if (Value == nullptr || !Value->is_object()) {
        return nullptr;
    }
    return &Value->as_object();
}

const Json::array* ArrayField(const Json::object& Object,
                              std::string_view Key) {
    const Json::value* Value = Object.if_contains(Key);
    if (Value == nullptr || !Value->is_array()) {
        return nullptr;
    }
    return &Value->as_array();
}

FailureCode MapProviderFailureCode(std::string_view Code) {
    if (Code == "invalid_api_key" || Code == "authentication_error" ||
        Code == "unauthorized" || Code == "permission_denied") {
        return FailureCode::AuthenticationFailure;
    }
    if (Code == "rate_limit_exceeded" || Code == "rate_limit_error") {
        return FailureCode::RateLimited;
    }
    if (Code == "content_filter" || Code == "content_policy_violation") {
        return FailureCode::ContentFiltered;
    }
    if (Code == "invalid_request_error") {
        return FailureCode::InvalidRequest;
    }
    return FailureCode::ProviderRejected;
}

ProviderError ProviderErrorFromJson(const Json::object& ErrorObject) {
    std::string Code;
    if (const auto Value = StringField(ErrorObject, "code"); Value) {
        Code = std::string(*Value);
    }
    if (Code.empty()) {
        if (const auto Value = StringField(ErrorObject, "type"); Value) {
            Code = std::string(*Value);
        }
    }
    const FailureCode Failure = MapProviderFailureCode(Code);
    std::string Message = "Provider request failed.";
    if (const auto Value = StringField(ErrorObject, "message"); Value) {
        Message = SanitizeDescription(*Value);
    }
    return ProviderError{
        .Code = Failure,
        .Retryable = Failure == FailureCode::RateLimited,
        .SanitizedDescription = std::move(Message)};
}

ProviderError ProviderErrorFromBody(std::string_view Body) {
    boost::system::error_code ParseError;
    Json::value Parsed = Json::parse(Body, ParseError);
    if (!ParseError && Parsed.is_object()) {
        const Json::object& Root = Parsed.as_object();
        if (const Json::object* ErrorObject = ObjectField(Root, "error");
            ErrorObject != nullptr) {
            return ProviderErrorFromJson(*ErrorObject);
        }
        if (StringField(Root, "message").has_value() ||
            StringField(Root, "code").has_value()) {
            return ProviderErrorFromJson(Root);
        }
    }
    return ProviderError{.Code = FailureCode::ProviderRejected,
                         .Retryable = false,
                         .SanitizedDescription =
                             "Provider returned an HTTP error."};
}

std::optional<UsageSnapshot> UsageFromJson(const Json::value* Value) {
    if (Value == nullptr || !Value->is_object()) {
        return std::nullopt;
    }
    const Json::object& Usage = Value->as_object();
    UsageSnapshot Snapshot;
    if (const auto Input = IntField(Usage, "input_tokens"); Input) {
        Snapshot.InputTokens = static_cast<std::uint64_t>(*Input);
    }
    if (const auto Output = IntField(Usage, "output_tokens"); Output) {
        Snapshot.OutputTokens = static_cast<std::uint64_t>(*Output);
    }
    if (const auto Total = IntField(Usage, "total_tokens"); Total) {
        Snapshot.TotalTokens = static_cast<std::uint64_t>(*Total);
    }
    if (const Json::object* InputDetails =
            ObjectField(Usage, "input_tokens_details");
        InputDetails != nullptr) {
        if (const auto Cached = IntField(*InputDetails, "cached_tokens");
            Cached) {
            Snapshot.CachedInputReadTokens =
                static_cast<std::uint64_t>(*Cached);
        }
    }
    if (const Json::object* OutputDetails =
            ObjectField(Usage, "output_tokens_details");
        OutputDetails != nullptr) {
        if (const auto Reasoning =
                IntField(*OutputDetails, "reasoning_tokens");
            Reasoning) {
            Snapshot.ReasoningOutputTokens =
                static_cast<std::uint64_t>(*Reasoning);
        }
    }
    Snapshot.Completeness = UsageCompleteness::Final;
    return Snapshot;
}

ProviderResult<Json::object> BuildWireRequest(
    const ModelProviderProfile& Profile, const CanonicalModelRequest& Request) {
    Json::object Root;

    const std::string Model =
        Request.ModelName.empty() ? Profile.ModelName : Request.ModelName;
    if (Model.empty()) {
        return ProviderError{.Code = FailureCode::InvalidRequest,
                             .Retryable = false,
                             .SanitizedDescription =
                                 "Model name is required."};
    }
    Root["model"] = Model;

    std::string Instructions = Request.SystemInstructions;
    if (!Request.DeveloperInstructions.empty()) {
        if (!Instructions.empty()) {
            Instructions.append("\n\n");
        }
        Instructions.append(Request.DeveloperInstructions);
    }
    if (!Instructions.empty()) {
        Root["instructions"] = Instructions;
    }

    Json::array Input;
    const auto AppendTextContent =
        [](Json::array& Content, const CanonicalMessage& Message,
           std::string& Error) -> bool {
        const char* Type =
            Message.Role == MessageRole::Assistant ? "output_text"
                                                   : "input_text";
        for (const ContentPart& Part : Message.ContentParts) {
            if (const auto* Text = std::get_if<TextContentPart>(&Part);
                Text != nullptr) {
                Content.emplace_back(Json::object{
                    {"type", Type}, {"text", Text->Text}});
                continue;
            }
            Error = "Image content is not supported by the Responses adapter.";
            return false;
        }
        return true;
    };

    std::string ContentError;
    for (const CanonicalMessage& Message : Request.Messages) {
        if (Message.Role == MessageRole::System ||
            Message.Role == MessageRole::Developer) {
            return ProviderError{
                .Code = FailureCode::InvalidRequest,
                .Retryable = false,
                .SanitizedDescription =
                    "System and developer text must use instructions."};
        }
        if (Message.Role == MessageRole::Tool) {
            if (!Message.ToolCallResultId.has_value()) {
                return ProviderError{
                    .Code = FailureCode::InvalidRequest,
                    .Retryable = false,
                    .SanitizedDescription =
                        "Tool result requires a tool call id."};
            }
            std::string Output;
            for (const ContentPart& Part : Message.ContentParts) {
                if (const auto* Text = std::get_if<TextContentPart>(&Part);
                    Text != nullptr) {
                    Output.append(Text->Text);
                }
            }
            Input.emplace_back(Json::object{
                {"type", "function_call_output"},
                {"call_id", Message.ToolCallResultId->Value},
                {"output", Output}});
            continue;
        }

        Json::array Content;
        if (!AppendTextContent(Content, Message, ContentError)) {
            return ProviderError{.Code = FailureCode::UnsupportedCapability,
                                 .Retryable = false,
                                 .SanitizedDescription = std::move(ContentError)};
        }
        if (!Content.empty()) {
            Input.emplace_back(Json::object{
                {"role", Message.Role == MessageRole::Assistant ? "assistant"
                                                                : "user"},
                {"content", std::move(Content)}});
        }
        for (const FunctionCall& Call : Message.FunctionCalls) {
            boost::system::error_code ParseError;
            static_cast<void>(
                Json::parse(Call.Arguments.JsonText, ParseError));
            if (ParseError) {
                return ProviderError{
                    .Code = FailureCode::InvalidRequest,
                    .Retryable = false,
                    .SanitizedDescription =
                        "Function call arguments are not valid JSON."};
            }
            Input.emplace_back(Json::object{
                {"type", "function_call"},
                {"call_id", Call.Id.Value},
                {"name", Call.Name},
                {"arguments", Call.Arguments.JsonText}});
        }
    }
    if (!Request.ContentParts.empty()) {
        Json::array Content;
        for (const ContentPart& Part : Request.ContentParts) {
            if (const auto* Text = std::get_if<TextContentPart>(&Part);
                Text != nullptr) {
                Content.emplace_back(Json::object{
                    {"type", "input_text"}, {"text", Text->Text}});
                continue;
            }
            return ProviderError{
                .Code = FailureCode::UnsupportedCapability,
                .Retryable = false,
                .SanitizedDescription =
                    "Image content is not supported by the Responses adapter."};
        }
        if (!Content.empty()) {
            Input.emplace_back(Json::object{
                {"role", "user"}, {"content", std::move(Content)}});
        }
    }
    Root["input"] = std::move(Input);

    if (!Request.ToolDefinitions.empty()) {
        Json::array Tools;
        for (const ToolDefinition& Tool : Request.ToolDefinitions) {
            boost::system::error_code ParseError;
            Json::value Parameters =
                Json::parse(Tool.Parameters.JsonText, ParseError);
            if (ParseError || !Parameters.is_object()) {
                return ProviderError{
                    .Code = FailureCode::InvalidRequest,
                    .Retryable = false,
                    .SanitizedDescription =
                        "Tool parameters must be a JSON object schema."};
            }
            Json::object Entry{{"type", "function"}, {"name", Tool.Name}};
            if (!Tool.Description.empty()) {
                Entry["description"] = Tool.Description;
            }
            Entry["parameters"] = std::move(Parameters);
            Entry["strict"] = Tool.Strict;
            Tools.emplace_back(std::move(Entry));
        }
        Root["tools"] = std::move(Tools);
    }

    const std::optional<std::uint32_t> Limit =
        Request.Limit.MaximumOutputTokens.has_value()
            ? Request.Limit.MaximumOutputTokens
            : Profile.Limit.MaximumOutputTokens;
    if (Limit.has_value()) {
        Root["max_output_tokens"] = *Limit;
    }
    if (Request.Sampling.TopK.has_value()) {
        return ProviderError{
            .Code = FailureCode::UnsupportedCapability,
            .Retryable = false,
            .SanitizedDescription =
                "Responses does not support the top_k sampling option."};
    }
    if (Request.Sampling.Temperature.has_value()) {
        Root["temperature"] = *Request.Sampling.Temperature;
    }
    if (Request.Sampling.TopP.has_value()) {
        Root["top_p"] = *Request.Sampling.TopP;
    }
    if (!Request.ProviderOptions.empty()) {
        return ProviderError{
            .Code = FailureCode::UnsupportedCapability,
            .Retryable = false,
            .SanitizedDescription =
                "Unregistered provider options are not supported."};
    }
    Root["stream"] = true;
    return Root;
}

class EventEmitter final {
public:
    EventEmitter(RequestId Request, AgentTurnId Turn, const EventSink& Sink)
        : RequestIdValue(std::move(Request)),
          TurnIdValue(std::move(Turn)), SinkRef(Sink) {}

    [[nodiscard]] ProviderEventMetadata NextMetadata() {
        return ProviderEventMetadata{.Request = RequestIdValue,
                                     .TurnId = TurnIdValue,
                                     .SequenceNumber = NextSequence++};
    }

    void Emit(const CanonicalNonTerminalEvent& Event) {
        if (SinkRef) {
            SinkRef(Event);
        }
    }

private:
    RequestId RequestIdValue;
    AgentTurnId TurnIdValue;
    const EventSink& SinkRef;
    std::uint64_t NextSequence{1};
};

class StreamSession final {
public:
    StreamSession(EventEmitter& InEmitter) : Emitter(InEmitter) {}

    [[nodiscard]] bool Done() const { return TerminalEvent.has_value(); }

    [[nodiscard]] const std::optional<CanonicalTerminalEvent>& Terminal() const {
        return TerminalEvent;
    }

    void HandleFrame(std::string_view Frame) {
        if (Done()) {
            return;
        }
        boost::system::error_code ParseError;
        const Json::value Parsed = Json::parse(Frame, ParseError);
        if (ParseError || !Parsed.is_object()) {
            FailInvalidStream("Provider SSE data is not a JSON object.");
            return;
        }
        const Json::object& Event = Parsed.as_object();
        const auto Type = StringField(Event, "type");
        if (!Type.has_value()) {
            FailInvalidStream("Provider SSE data has no event type.");
            return;
        }
        const std::optional<std::int64_t> Sequence = IntField(Event, "sequence_number");
        if (Sequence.has_value()) {
            const auto Decision = AcceptSequence(
                static_cast<std::uint64_t>(*Sequence), Frame);
            if (Decision == SequenceDecision::Duplicate) {
                return;
            }
            if (Decision == SequenceDecision::Conflict) {
                FailInvalidStream(
                    "Provider event sequence is non-contiguous or conflicts.");
                return;
            }
        }
        Dispatch(*Type, Event);
    }

    std::string PartialText;

private:
    enum class SequenceDecision {
        Accepted,
        Duplicate,
        Conflict
    };

    SequenceDecision AcceptSequence(std::uint64_t Sequence,
                                    std::string_view Raw) {
        if (!LastSequence.has_value()) {
            LastSequence = Sequence;
            LastRawFrame = std::string(Raw);
            return SequenceDecision::Accepted;
        }
        if (Sequence == *LastSequence) {
            return Raw == LastRawFrame ? SequenceDecision::Duplicate
                                       : SequenceDecision::Conflict;
        }
        if (Sequence != *LastSequence + 1U) {
            return SequenceDecision::Conflict;
        }
        LastSequence = Sequence;
        LastRawFrame = std::string(Raw);
        return SequenceDecision::Accepted;
    }

    struct ToolState final {
        std::string CallId;
        std::string Name;
        std::string Arguments;
        bool Ready{false};
    };

    void FailInvalidStream(std::string Description) {
        if (Done()) {
            return;
        }
        TerminalEvent = FailedTerminal(
            Emitter.NextMetadata(), FailureCode::InvalidStream, false,
            std::move(Description));
    }

    void Dispatch(std::string_view Type, const Json::object& Event) {
        if (Type == "response.created") {
            const Json::object* Response = ObjectField(Event, "response");
            std::optional<ProviderResponseId> Id;
            if (Response != nullptr) {
                if (const auto Value = StringField(*Response, "id"); Value) {
                    Id = ProviderResponseId{std::string(*Value)};
                }
            }
            ResponseStartedEvent Started{.Metadata = Emitter.NextMetadata(),
                                         .ProviderResponse = std::move(Id)};
            Emitter.Emit(Started);
            return;
        }
        if (Type == "response.output_item.added") {
            const Json::object* Item = ObjectField(Event, "item");
            if (Item == nullptr) {
                return;
            }
            const auto ItemType = StringField(*Item, "type");
            if (ItemType != "function_call") {
                return; // reasoning and message items carry no action here.
            }
            const auto Id = StringField(*Item, "id");
            const auto CallId = StringField(*Item, "call_id");
            const auto Name = StringField(*Item, "name");
            if (!Id || !CallId || !Name) {
                FailInvalidStream("Function call item is incomplete.");
                return;
            }
            ToolState State;
            State.CallId = std::string(*CallId);
            State.Name = std::string(*Name);
            Tools[std::string(*Id)] = std::move(State);
            ToolCallStartedEvent Started{
                .Metadata = Emitter.NextMetadata(),
                .Item = ItemId{std::string(*Id)},
                .Call = ToolCallId{std::string(*CallId)},
                .ToolName = std::string(*Name)};
            Emitter.Emit(Started);
            return;
        }
        if (Type == "response.output_text.delta") {
            const auto Item = StringField(Event, "item_id");
            const auto Delta = StringField(Event, "delta");
            if (!Item || !Delta) {
                FailInvalidStream("Text delta is incomplete.");
                return;
            }
            PartialText.append(*Delta);
            TextDeltaEvent Text{.Metadata = Emitter.NextMetadata(),
                                .Item = ItemId{std::string(*Item)},
                                .Utf8Delta = std::string(*Delta)};
            Emitter.Emit(Text);
            return;
        }
        if (Type == "response.refusal.delta") {
            const auto Item = StringField(Event, "item_id");
            const auto Delta = StringField(Event, "delta");
            if (!Item || !Delta) {
                FailInvalidStream("Refusal delta is incomplete.");
                return;
            }
            SawRefusal = true;
            RefusalDeltaEvent Refusal{
                .Metadata = Emitter.NextMetadata(),
                .Item = ItemId{std::string(*Item)},
                .Utf8Delta = std::string(*Delta),
                .Category = std::nullopt};
            Emitter.Emit(Refusal);
            return;
        }
        if (Type == "response.function_call_arguments.delta") {
            const auto Item = StringField(Event, "item_id");
            const auto Delta = StringField(Event, "delta");
            if (!Item || !Delta) {
                FailInvalidStream("Tool argument delta is incomplete.");
                return;
            }
            auto Iterator = Tools.find(std::string(*Item));
            if (Iterator == Tools.end() || Iterator->second.Ready) {
                FailInvalidStream("Tool argument delta has no open call.");
                return;
            }
            Iterator->second.Arguments.append(*Delta);
            ToolArgumentDeltaEvent Arguments{
                .Metadata = Emitter.NextMetadata(),
                .Item = ItemId{std::string(*Item)},
                .Call = ToolCallId{Iterator->second.CallId},
                .JsonTextDelta = std::string(*Delta)};
            Emitter.Emit(Arguments);
            return;
        }
        if (Type == "response.function_call_arguments.done") {
            const auto Item = StringField(Event, "item_id");
            const auto Arguments = StringField(Event, "arguments");
            if (!Item || !Arguments) {
                FailInvalidStream("Tool argument completion is incomplete.");
                return;
            }
            CompleteTool(std::string(*Item), std::string(*Arguments));
            return;
        }
        if (Type == "response.output_item.done") {
            const Json::object* Item = ObjectField(Event, "item");
            if (Item == nullptr ||
                StringField(*Item, "type") != "function_call") {
                return;
            }
            const auto Id = StringField(*Item, "id");
            const auto Arguments = StringField(*Item, "arguments");
            if (!Id || !Arguments) {
                FailInvalidStream("Completed function call is incomplete.");
                return;
            }
            auto Iterator = Tools.find(std::string(*Id));
            if (Iterator == Tools.end() || !Iterator->second.Ready) {
                CompleteTool(std::string(*Id), std::string(*Arguments));
            }
            return;
        }
        if (Type == "response.completed") {
            CompleteResponse(Event);
            return;
        }
        if (Type == "response.incomplete") {
            IncompleteResponse(Event);
            return;
        }
        if (Type == "response.failed") {
            const Json::object* Response = ObjectField(Event, "response");
            ProviderError Error{.Code = FailureCode::ProviderRejected,
                                .Retryable = false,
                                .SanitizedDescription =
                                    "Provider response failed."};
            if (Response != nullptr) {
                if (const Json::object* ErrorObject =
                        ObjectField(*Response, "error");
                    ErrorObject != nullptr) {
                    Error = ProviderErrorFromJson(*ErrorObject);
                }
            }
            TerminalEvent = ResponseFailedEvent{.Metadata =
                                                    Emitter.NextMetadata(),
                                                .Failure = std::move(Error)};
            return;
        }
        if (Type == "error") {
            const ProviderError Error = ProviderErrorFromJson(Event);
            TerminalEvent = ResponseFailedEvent{
                .Metadata = Emitter.NextMetadata(), .Failure = Error};
            return;
        }
        // response.content_part.*, response.output_text.done,
        // response.refusal.done, reasoning events and unknown future events
        // are filtered while their sequence numbers still participate in the
        // continuity check above.
    }

    void CompleteTool(const std::string& Item, std::string Arguments) {
        auto Iterator = Tools.find(Item);
        if (Iterator == Tools.end()) {
            FailInvalidStream("Tool completion has no matching call.");
            return;
        }
        boost::system::error_code ParseError;
        static_cast<void>(Json::parse(Arguments, ParseError));
        if (ParseError) {
            FailInvalidStream("Function arguments are not valid JSON.");
            return;
        }
        if (Iterator->second.Ready) {
            return;
        }
        Iterator->second.Ready = true;
        ToolCallReadyEvent Ready{
            .Metadata = Emitter.NextMetadata(),
            .Item = ItemId{Item},
            .Call = ToolCallId{Iterator->second.CallId},
            .ToolName = Iterator->second.Name,
            .Arguments = JsonValue{std::move(Arguments)}};
        Emitter.Emit(Ready);
        SawToolCall = true;
    }

    void EmitFinalUsage(const Json::object& Response) {
        const Json::value* UsageValue = Response.if_contains("usage");
        const std::optional<UsageSnapshot> Usage = UsageFromJson(UsageValue);
        if (Usage.has_value()) {
            UsageUpdatedEvent Updated{.Metadata = Emitter.NextMetadata(),
                                      .Usage = *Usage};
            Emitter.Emit(Updated);
        }
    }

    bool ResponseHasToolCall(const Json::object& Response) {
        const Json::array* Output = ArrayField(Response, "output");
        if (Output == nullptr) {
            return false;
        }
        for (const Json::value& Item : *Output) {
            if (!Item.is_object()) {
                continue;
            }
            if (StringField(Item.as_object(), "type") == "function_call") {
                return true;
            }
        }
        return false;
    }

    bool ResponseHasRefusal(const Json::object& Response) {
        const Json::array* Output = ArrayField(Response, "output");
        if (Output == nullptr) {
            return false;
        }
        for (const Json::value& Item : *Output) {
            if (!Item.is_object()) {
                continue;
            }
            const Json::array* Content =
                ArrayField(Item.as_object(), "content");
            if (Content == nullptr) {
                continue;
            }
            for (const Json::value& Part : *Content) {
                if (Part.is_object() &&
                    StringField(Part.as_object(), "type") == "refusal") {
                    return true;
                }
            }
        }
        return false;
    }

    void CompleteResponse(const Json::object& Event) {
        const Json::object* Response = ObjectField(Event, "response");
        if (Response == nullptr) {
            FailInvalidStream("response.completed has no response object.");
            return;
        }
        EmitFinalUsage(*Response);

        CompletionKind Kind = CompletionKind::FinalAnswer;
        if (SawToolCall || ResponseHasToolCall(*Response)) {
            Kind = CompletionKind::ToolHandoff;
        } else if (SawRefusal || ResponseHasRefusal(*Response)) {
            Kind = CompletionKind::Refusal;
        }
        TerminalEvent = ResponseCompletedEvent{
            .Metadata = Emitter.NextMetadata(),
            .Kind = Kind,
            .StopSequenceCode = std::nullopt,
            .FinalUsage = UsageFromJson(Response->if_contains("usage")),
            .StopDetails = std::nullopt};
    }

    void IncompleteResponse(const Json::object& Event) {
        const Json::object* Response = ObjectField(Event, "response");
        if (Response == nullptr) {
            FailInvalidStream("response.incomplete has no response object.");
            return;
        }
        const Json::object* Details =
            ObjectField(*Response, "incomplete_details");
        const auto Reason =
            Details != nullptr
                ? StringField(*Details, "reason")
                : std::optional<std::string_view>{};
        UsageSnapshot Usage;
        const std::optional<UsageSnapshot> ParsedUsage =
            UsageFromJson(Response->if_contains("usage"));
        if (ParsedUsage.has_value()) {
            Usage = *ParsedUsage;
        } else {
            Usage.Completeness = UsageCompleteness::Unavailable;
        }
        if (Reason == "max_output_tokens") {
            std::optional<std::string> SafeText;
            if (!PartialText.empty()) {
                SafeText = PartialText;
            }
            TerminalEvent = ResponseOutputLimitedEvent{
                .Metadata = Emitter.NextMetadata(),
                .Kind = LimitKind::OutputTokens,
                .Usage = Usage,
                .SafeGeneratedText = std::move(SafeText)};
            return;
        }
        if (Reason == "content_filter") {
            TerminalEvent = FailedTerminal(
                Emitter.NextMetadata(), FailureCode::ContentFiltered, false,
                "Provider reported content_filter.");
            return;
        }
        TerminalEvent = FailedTerminal(
            Emitter.NextMetadata(), FailureCode::InvalidStream, false,
            "Unsupported incomplete reason.");
    }

    EventEmitter& Emitter;
    std::optional<std::uint64_t> LastSequence;
    std::string LastRawFrame;
    std::map<std::string, ToolState> Tools;
    bool SawRefusal{false};
    bool SawToolCall{false};
    std::optional<CanonicalTerminalEvent> TerminalEvent;
};

class SseDecoder final {
public:
    void Append(std::string_view Bytes, StreamSession& Session) {
        if (Overflow) {
            return;
        }
        Buffer.append(Bytes);
        while (!Session.Done()) {
            const std::size_t Delimiter = FindFrameDelimiter();
            if (Delimiter == std::string::npos) {
                if (Buffer.size() > MaximumSseFrameBytes) {
                    Overflow = true;
                    Buffer.clear();
                }
                return;
            }
            const std::string_view Frame(Buffer.data(), Delimiter);
            std::string Data = ExtractData(Frame);
            Buffer.erase(0, FrameEnd(Buffer, Delimiter));
            if (!Data.empty()) {
                Session.HandleFrame(Data);
            }
        }
    }

    [[nodiscard]] bool HasPendingPartialFrame() const {
        return !Buffer.empty();
    }
    [[nodiscard]] bool Failed() const { return Overflow; }

private:
    [[nodiscard]] std::size_t FindFrameDelimiter() const {
        const std::size_t Lf = Buffer.find("\n\n");
        const std::size_t Crlf = Buffer.find("\r\n\r\n");
        return std::min(Lf, Crlf);
    }

    static std::size_t FrameEnd(const std::string& Buffer,
                                std::size_t Delimiter) {
        if (Delimiter + 1 < Buffer.size() &&
            Buffer[Delimiter] == '\r') {
            return Delimiter + 4;
        }
        return Delimiter + 2;
    }

    static std::string ExtractData(std::string_view Frame) {
        std::string Data;
        while (!Frame.empty()) {
            const std::size_t LineEnd = Frame.find('\n');
            std::string_view Line =
                Frame.substr(0, LineEnd == std::string_view::npos
                                    ? Frame.size()
                                    : LineEnd);
            if (!Line.empty() && Line.back() == '\r') {
                Line.remove_suffix(1);
            }
            if (Line.starts_with("data:")) {
                Line.remove_prefix(5);
                if (!Line.empty() && Line.front() == ' ') {
                    Line.remove_prefix(1);
                }
                if (!Data.empty()) {
                    Data.push_back('\n');
                }
                Data.append(Line);
            }
            if (LineEnd == std::string_view::npos) {
                break;
            }
            Frame.remove_prefix(LineEnd + 1);
        }
        return Data;
    }

    std::string Buffer;
    bool Overflow{false};
};

CanonicalTerminalEvent TerminalForTransportTerminal(
    const Net::TransportTerminal& Terminal, int HttpStatus,
    std::string_view Body, EventEmitter& Emitter) {
    if (const auto* Completed =
            std::get_if<Net::CompletedTransport>(&Terminal)) {
        if (HttpStatus == 401 || HttpStatus == 403) {
            return FailedTerminal(Emitter.NextMetadata(),
                                  FailureCode::AuthenticationFailure, false,
                                  ProviderErrorFromBody(Body)
                                      .SanitizedDescription);
        }
        if (HttpStatus == 429) {
            return FailedTerminal(Emitter.NextMetadata(),
                                  FailureCode::RateLimited, true,
                                  ProviderErrorFromBody(Body)
                                      .SanitizedDescription);
        }
        if (Completed->StatusCode == 500 || Completed->StatusCode == 502 ||
            Completed->StatusCode == 503 ||
            Completed->StatusCode == 504) {
            ProviderError Error = ProviderErrorFromBody(Body);
            Error.Retryable = true;
            if (Error.Code != FailureCode::AuthenticationFailure &&
                Error.Code != FailureCode::RateLimited &&
                Error.Code != FailureCode::ContentFiltered) {
                Error.Code = FailureCode::ProviderRejected;
            }
            return ResponseFailedEvent{.Metadata = Emitter.NextMetadata(),
                                       .Failure = std::move(Error)};
        }
        if (Completed->StatusCode >= 400) {
            const ProviderError Error = ProviderErrorFromBody(Body);
            return ResponseFailedEvent{.Metadata = Emitter.NextMetadata(),
                                       .Failure = Error};
        }
        return ResponseOutcomeUnknownEvent{
            .Metadata = Emitter.NextMetadata(),
            .Reason = OutcomeUnknownReasonCode{
                .Code = StableReasonCode{.Value = "OutcomeUnknown"}}};
    }
    if (std::holds_alternative<Net::UnknownOutcomeTransport>(Terminal)) {
        return ResponseOutcomeUnknownEvent{
            .Metadata = Emitter.NextMetadata(),
            .Reason = OutcomeUnknownReasonCode{
                .Code = StableReasonCode{.Value = "OutcomeUnknown"}}};
    }
    if (const auto* Cancelled =
            std::get_if<Net::CancelledTransport>(&Terminal)) {
        if (!Cancelled->RequestMayHaveReachedEndpoint) {
            return ResponseCancelledEvent{
                .Metadata = Emitter.NextMetadata(),
                .Reason = CancelReasonCode{
                    .Code =
                        StableReasonCode{.Value = "CancelledBeforeSend"}}};
        }
        return ResponseOutcomeUnknownEvent{
            .Metadata = Emitter.NextMetadata(),
            .Reason = OutcomeUnknownReasonCode{
                .Code = StableReasonCode{.Value = "OutcomeUnknown"}}};
    }
    if (const auto* TimedOut =
            std::get_if<Net::TimedOutTransport>(&Terminal)) {
        if (TimedOut->Phase == Net::TransportTimeoutPhase::Connect) {
            return FailedTerminal(Emitter.NextMetadata(),
                                  FailureCode::TransportFailure, true,
                                  "Provider connection timed out.");
        }
        return ResponseOutcomeUnknownEvent{
            .Metadata = Emitter.NextMetadata(),
            .Reason = OutcomeUnknownReasonCode{
                .Code = StableReasonCode{.Value = "OutcomeUnknown"}}};
    }
    if (std::holds_alternative<Net::RedirectDeniedTransport>(Terminal)) {
        return FailedTerminal(Emitter.NextMetadata(),
                              FailureCode::ProviderRejected, false,
                              "Redirect denied by transport policy.");
    }
    if (const auto* Failed =
            std::get_if<Net::FailedTransport>(&Terminal)) {
        const FailureCode Code =
            Failed->Code == Net::TransportFailureCode::EndpointNotGranted
                ? FailureCode::InvalidProfile
                : FailureCode::TransportFailure;
        return FailedTerminal(Emitter.NextMetadata(), Code, false,
                              "Provider transport failed.");
    }
    return ResponseOutcomeUnknownEvent{
        .Metadata = Emitter.NextMetadata(),
        .Reason = OutcomeUnknownReasonCode{
            .Code = StableReasonCode{.Value = "OutcomeUnknown"}}};
}

} // namespace

OpenAIResponsesProvider::OpenAIResponsesProvider(
    ProviderProfileId Profile, Net::HttpTransportPort& Transport,
    Net::EndpointGrantId Grant, Net::HttpsDestination Destination)
    : BoundProfile(std::move(Profile)),
      Transport(Transport),
      Grant(std::move(Grant)),
      Destination(std::move(Destination)) {}

OpenAIResponsesProvider::~OpenAIResponsesProvider() = default;

ProviderResult<ProviderCapabilities> OpenAIResponsesProvider::GetCapabilities(
    const ProviderProfileId& ProfileId) const {
    if (ProfileId != BoundProfile) {
        return InvalidProfileError();
    }
    return ProviderCapabilities{
        .Streaming = true,
        .ToolCalls = true,
        .Vision = false,
        .Usage = true,
        .Cancellation = true,
        .OutputLimit = true};
}

ProviderResult<std::monostate> OpenAIResponsesProvider::ValidateProfile(
    const ModelProviderProfile& Profile) const {
    if (Profile.Id != BoundProfile ||
        Profile.Protocol != ProviderProtocol::OpenAIResponses ||
        Profile.Endpoint.empty() || Profile.ModelName.empty()) {
        return InvalidProfileError();
    }
    if (Profile.Authentication != AuthenticationScheme::Bearer &&
        Profile.Authentication != AuthenticationScheme::XApiKeyHeader) {
        return InvalidProfileError();
    }
    return std::monostate{};
}

CanonicalTerminalEvent OpenAIResponsesProvider::StreamTurn(
    const ModelProviderProfile& Profile, const CredentialLease& Lease,
    const CanonicalModelRequest& Request, std::stop_token CancellationToken,
    const EventSink& Sink) {
    EventEmitter Emitter(Request.Id, Request.TurnId, Sink);
    if (CancellationToken.stop_requested()) {
        return ResponseCancelledEvent{
            .Metadata = Emitter.NextMetadata(),
            .Reason = CancelReasonCode{
                .Code = StableReasonCode{.Value = "CancelledBeforeSend"}}};
    }
    const ProviderResult<std::monostate> ProfileValidation =
        ValidateProfile(Profile);
    if (const auto* Error =
            std::get_if<ProviderError>(&ProfileValidation)) {
        return ResponseFailedEvent{.Metadata = Emitter.NextMetadata(),
                                   .Failure = *Error};
    }
    const ProviderResult<Json::object> WireRequest =
        BuildWireRequest(Profile, Request);
    if (const auto* Error = std::get_if<ProviderError>(&WireRequest)) {
        return ResponseFailedEvent{.Metadata = Emitter.NextMetadata(),
                                   .Failure = *Error};
    }
    const std::string Secret(Lease.GetAuthenticationSecret());
    if (Secret.empty()) {
        return FailedTerminal(Emitter.NextMetadata(),
                              FailureCode::AuthenticationFailure, false,
                              "Credential lease is empty.");
    }

    std::vector<Net::TransportHeader> Headers{
        Net::TransportHeader{.Name = "Content-Type",
                             .Value = "application/json"},
        Net::TransportHeader{.Name = "Accept",
                             .Value = "text/event-stream"}};
    if (Profile.Authentication == AuthenticationScheme::Bearer) {
        Headers.push_back(Net::TransportHeader{
            .Name = "Authorization", .Value = "Bearer " + Secret});
    } else {
        Headers.push_back(
            Net::TransportHeader{.Name = "api-key", .Value = Secret});
    }

    Net::TransportTimeouts Timeouts;
    if (Profile.Timeouts.ConnectionTimeout.count() > 0) {
        Timeouts.Connect = Profile.Timeouts.ConnectionTimeout;
    }
    if (Profile.Timeouts.RequestTimeout.count() > 0) {
        Timeouts.Response = Profile.Timeouts.RequestTimeout;
    }

    const Net::TransportRequest TransportRequest{
        .Id = Net::TransportRequestId{Request.Id.Value},
        .Grant = Grant,
        .Method = Net::TransportMethod::Post,
        .Destination = Destination,
        .Headers = std::move(Headers),
        .BodyUtf8 = Json::serialize(std::get<Json::object>(WireRequest)),
        .Limits = Net::TransportRequestLimits{},
        .Timeouts = Timeouts,
        .Proxy = Net::TransportProxyMode::System};

    StreamSession Session(Emitter);
    SseDecoder Decoder;
    std::string ResponseBody;
    int HttpStatus = 200;
    const Net::TransportTerminal Terminal = Transport.Stream(
        TransportRequest,
        [&HttpStatus](const Net::ResponseHeadMetadata& Head) {
            HttpStatus = Head.StatusCode;
        },
        [&Session, &Decoder, &ResponseBody](std::span<const std::byte> Chunk) {
            if (Session.Done()) {
                return;
            }
            const std::string_view Bytes(
                reinterpret_cast<const char*>(Chunk.data()), Chunk.size());
            if (ResponseBody.size() < 64U * 1024U) {
                ResponseBody.append(Bytes.substr(
                    0, std::min(Bytes.size(), 64U * 1024U - ResponseBody.size())));
            }
            Decoder.Append(Bytes, Session);
        },
        CancellationToken);

    if (Decoder.Failed()) {
        return FailedTerminal(Emitter.NextMetadata(),
                              FailureCode::InvalidStream, false,
                              "Provider SSE frame exceeded its bound.");
    }
    if (Session.Terminal().has_value()) {
        return *Session.Terminal();
    }
    if (HttpStatus >= 400) {
        return TerminalForTransportTerminal(Terminal, HttpStatus, ResponseBody,
                                            Emitter);
    }
    if (Decoder.HasPendingPartialFrame()) {
        return ResponseOutcomeUnknownEvent{
            .Metadata = Emitter.NextMetadata(),
            .Reason = OutcomeUnknownReasonCode{
                .Code = StableReasonCode{.Value = "OutcomeUnknown"}}};
    }
    return TerminalForTransportTerminal(Terminal, HttpStatus, ResponseBody,
                                        Emitter);
}

ProviderResult<ProviderHealthStatus> OpenAIResponsesProvider::HealthCheck(
    const ModelProviderProfile& Profile) {
    const ProviderResult<std::monostate> ProfileValidation =
        ValidateProfile(Profile);
    if (std::holds_alternative<ProviderError>(ProfileValidation)) {
        return ProviderHealthStatus{.State = ProviderHealthState::Unavailable};
    }
    return ProviderHealthStatus{.State = ProviderHealthState::Available};
}

} // namespace EmulativeDialog::Providers::OpenAIResponses
