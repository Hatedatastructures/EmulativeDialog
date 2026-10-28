#include "Source/Providers/OpenAIChatCompletions/ChatCompletionsProvider.hpp"

// Boost.JSON is consumed in header-only mode: exactly one product TU includes
// the compiled implementation so the pinned dependency adds no runtime DLL.
#include <boost/json/src.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EmulativeDialog::Providers::OpenAIChatCompletions {
namespace {

namespace json = boost::json;
using Platform::Network::CompletedTransport;
using Platform::Network::FailedTransport;
using Platform::Network::HttpsDestination;
using Platform::Network::RedirectDeniedTransport;
using Platform::Network::ResponseHeadMetadata;
using Platform::Network::TimedOutTransport;
using Platform::Network::TransportRequest;
using Platform::Network::TransportTerminal;
using Platform::Network::UnknownOutcomeTransport;
using Platform::Network::CancelledTransport;

ProviderError MakeError(FailureCode Code, bool Retryable,
                        std::string Description) {
    return ProviderError{Code, Retryable, std::move(Description)};
}

CanonicalTerminalEvent Failed(FailureCode Code, bool Retryable,
                              std::string Description, std::uint64_t Sequence) {
    return ResponseFailedEvent{
        ProviderEventMetadata{RequestId{}, AgentTurnId{}, Sequence},
        MakeError(Code, Retryable, std::move(Description))};
}

std::optional<std::uint16_t> ParsePort(std::string_view Text) {
    if (Text.empty()) {
        return std::nullopt;
    }
    std::uint32_t Value = 0;
    const auto Result =
        std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    if (Result.ec != std::errc{} ||
        Result.ptr != Text.data() + Text.size() ||
        Value == 0 || Value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(Value);
}

// Parses the Core-approved HTTPS endpoint into the transport destination
// shape. The transport still re-validates the exact destination against the
// Core-minted endpoint grant before any DNS or connect step.
std::optional<HttpsDestination> ParseHttpsEndpoint(std::string_view Endpoint) {
    constexpr std::string_view Scheme = "https://";
    if (Endpoint.substr(0, Scheme.size()) != Scheme) {
        return std::nullopt;
    }
    Endpoint.remove_prefix(Scheme.size());
    if (Endpoint.empty()) {
        return std::nullopt;
    }
    for (const char Character : Endpoint) {
        const auto Byte = static_cast<unsigned char>(Character);
        if (Byte <= 0x20U || Byte == 0x7FU) {
            return std::nullopt;
        }
    }
    if (Endpoint.find('@') != std::string_view::npos ||
        Endpoint.find('#') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view Authority = Endpoint;
    std::string_view Path = "/";
    if (const auto Slash = Endpoint.find('/'); Slash != std::string_view::npos) {
        Authority = Endpoint.substr(0, Slash);
        Path = Endpoint.substr(Slash);
    }
    if (Authority.empty()) {
        return std::nullopt;
    }

    HttpsDestination Destination;
    Destination.Path = std::string(Path);
    if (Authority.front() == '[') {
        const auto Close = Authority.find(']');
        if (Close == std::string_view::npos) {
            return std::nullopt;
        }
        Destination.Host = std::string(Authority.substr(0, Close + 1));
        const auto Remainder = Authority.substr(Close + 1);
        if (Remainder.empty()) {
            Destination.Port = 443;
        } else if (Remainder.front() == ':') {
            const auto Port = ParsePort(Remainder.substr(1));
            if (!Port.has_value()) {
                return std::nullopt;
            }
            Destination.Port = *Port;
        } else {
            return std::nullopt;
        }
    } else {
        const auto Colon = Authority.rfind(':');
        if (Colon == std::string_view::npos) {
            Destination.Host = std::string(Authority);
            Destination.Port = 443;
        } else {
            if (Authority.find(':') != Colon) {
                return std::nullopt;
            }
            Destination.Host = std::string(Authority.substr(0, Colon));
            const auto Port = ParsePort(Authority.substr(Colon + 1));
            if (!Port.has_value()) {
                return std::nullopt;
            }
            Destination.Port = *Port;
        }
    }
    if (Destination.Host.empty()) {
        return std::nullopt;
    }
    return Destination;
}

enum class FieldKind : std::uint8_t {
    Absent,
    Null,
    String,
    Other
};

FieldKind KindOf(const json::object& Object, std::string_view Name,
                 std::string_view* OutText = nullptr) {
    const json::value* Value = Object.if_contains(Name);
    if (Value == nullptr) {
        return FieldKind::Absent;
    }
    if (Value->is_null()) {
        return FieldKind::Null;
    }
    if (const json::string* Text = Value->if_string()) {
        if (OutText != nullptr) {
            OutText->operator=(std::string_view(Text->data(), Text->size()));
        }
        return FieldKind::String;
    }
    return FieldKind::Other;
}

std::optional<std::uint64_t> UnsignedField(const json::object& Object,
                                           std::string_view Name) {
    const json::value* Value = Object.if_contains(Name);
    if (Value == nullptr) {
        return std::nullopt;
    }
    if (Value->is_uint64()) {
        return Value->as_uint64();
    }
    if (Value->is_int64() && Value->as_int64() >= 0) {
        return static_cast<std::uint64_t>(Value->as_int64());
    }
    return std::nullopt;
}

std::string TextOf(const json::object& Object, std::string_view Name,
                   std::string Default = {}) {
    std::string_view Text;
    if (KindOf(Object, Name, &Text) != FieldKind::String) {
        return Default;
    }
    return std::string(Text);
}

bool ParseJsonText(std::string_view Text, json::value& Out) {
    boost::system::error_code Error;
    Out = json::parse(Text, Error);
    return !Error;
}

bool IsCompleteJson(std::string_view Text) {
    json::value Value;
    return ParseJsonText(Text, Value);
}

std::string JoinTextParts(const std::vector<ContentPart>& Parts) {
    std::string Result;
    for (const ContentPart& Part : Parts) {
        if (const auto* Text = std::get_if<TextContentPart>(&Part)) {
            Result.append(Text->Text);
        }
    }
    return Result;
}

struct RequestOptions final {
    bool IncludeUsage{false};
    std::optional<std::string> ToolChoice;
    std::optional<std::string> ServiceTier;
};

std::optional<ProviderError> ReadRequestOptions(
    const CanonicalModelRequest& Request,
    const ModelProviderProfile& Profile, RequestOptions& Options) {
    bool CompatibilityAllowed = Profile.CompatibilityProfile.has_value();
    for (const RegisteredProviderOption& Option : Request.ProviderOptions) {
        const std::string& Id = Option.Id.Value;
        if (Id == "N" || Id == "n") {
            const auto* Value = std::get_if<std::int64_t>(&Option.Value);
            if (Value == nullptr || *Value != 1) {
                return MakeError(FailureCode::InvalidRequest, false,
                                 "Chat Completions V1 fixes n=1.");
            }
            continue;
        }
        if (Id == "Stream" || Id == "stream") {
            const auto* Value = std::get_if<bool>(&Option.Value);
            if (Value == nullptr || !*Value) {
                return MakeError(FailureCode::InvalidRequest, false,
                                 "Chat Completions V1 requires streaming.");
            }
            continue;
        }
        if (Id == "IncludeUsage" || Id == "include_usage") {
            const auto* Value = std::get_if<bool>(&Option.Value);
            if (Value == nullptr) {
                return MakeError(FailureCode::InvalidRequest, false,
                                 "include_usage must be a boolean.");
            }
            Options.IncludeUsage = *Value;
            continue;
        }
        if (Id == "ToolChoice" || Id == "tool_choice") {
            const auto* Value = std::get_if<std::string>(&Option.Value);
            if (Value == nullptr) {
                return MakeError(FailureCode::InvalidRequest, false,
                                 "tool_choice must be a string in V1.");
            }
            Options.ToolChoice = *Value;
            continue;
        }
        if (Id == "ServiceTier" || Id == "service_tier" ||
            Id == "Compatibility.ServiceTier") {
            const auto* Value = std::get_if<std::string>(&Option.Value);
            if (Value == nullptr || !CompatibilityAllowed) {
                return MakeError(
                    FailureCode::InvalidRequest, false,
                    "Unregistered compatibility fields are rejected before "
                    "dispatch.");
            }
            Options.ServiceTier = *Value;
            continue;
        }
        if (Id.rfind("Compatibility.", 0) == 0) {
            return MakeError(
                FailureCode::InvalidRequest, false,
                "Unregistered compatibility fields are rejected before "
                "dispatch.");
        }
        return MakeError(FailureCode::InvalidRequest, false,
                         "Unregistered request option is rejected.");
    }
    return std::nullopt;
}

std::optional<UsageSnapshot> ParseUsage(const json::value& Value,
                                        bool CompatibilityAllowed,
                                        bool& Invalid) {
    Invalid = false;
    const json::object* Object = Value.if_object();
    if (Object == nullptr) {
        Invalid = true;
        return std::nullopt;
    }
    UsageSnapshot Snapshot;
    Snapshot.Completeness = UsageCompleteness::Final;
    Snapshot.InputTokens = UnsignedField(*Object, "prompt_tokens");
    Snapshot.OutputTokens = UnsignedField(*Object, "completion_tokens");
    Snapshot.TotalTokens = UnsignedField(*Object, "total_tokens");
    if (CompatibilityAllowed) {
        if (const json::value* Details =
                Object->if_contains("prompt_tokens_details");
            Details != nullptr) {
            if (const json::object* DetailObject = Details->if_object();
                DetailObject != nullptr) {
                Snapshot.CachedInputReadTokens =
                    UnsignedField(*DetailObject, "cached_tokens");
            }
        }
        if (const json::value* Details =
                Object->if_contains("completion_tokens_details");
            Details != nullptr) {
            if (const json::object* DetailObject = Details->if_object();
                DetailObject != nullptr) {
                Snapshot.ReasoningOutputTokens =
                    UnsignedField(*DetailObject, "reasoning_tokens");
            }
        }
    }
    return Snapshot;
}

class TurnProcessor final {
public:
    TurnProcessor(const ModelProviderProfile& Profile,
                  const CanonicalModelRequest& Request, const EventSink& Sink)
        : ProfileValue(Profile), RequestValue(Request), EventSinkValue(Sink) {}

    void Feed(std::span<const std::byte> Chunk) {
        for (const std::byte Byte : Chunk) {
            const char Character = static_cast<char>(Byte);
            if (Character == '\n') {
                DispatchLine(Line);
                Line.clear();
            } else {
                Line.push_back(Character);
            }
        }
    }

    void FinishStream() {
        if (!Line.empty()) {
            DispatchLine(Line);
            Line.clear();
        }
    }

    [[nodiscard]] bool SawResponseStarted() const noexcept { return Started; }
    [[nodiscard]] bool SawChunk() const noexcept { return ChunkSeen; }
    [[nodiscard]] std::optional<ProviderError> FatalError() const {
        return Fatal;
    }
    [[nodiscard]] std::optional<std::string> FinishReason() const {
        return FinishReasonValue;
    }
    [[nodiscard]] std::optional<UsageSnapshot> UsageValue() const {
        return LastUsage;
    }

    void SetHttpErrorCode(std::string Code) {
        HttpErrorCode = std::move(Code);
    }

    [[nodiscard]] const std::string& GetHttpErrorCode() const noexcept {
        return HttpErrorCode;
    }

    [[nodiscard]] CanonicalTerminalEvent TerminalForTransport(
        const TransportTerminal& Transport, std::uint16_t StatusCode) {
        if (Fatal.has_value()) {
            return MakeFailed(*Fatal);
        }
        if (const auto* Completed = std::get_if<CompletedTransport>(&Transport);
            Completed != nullptr) {
            if (StatusCode != 200) {
                return MapHttpStatus(StatusCode);
            }
            if (DataAfterDone) {
                return MakeFailed(MakeError(
                    FailureCode::InvalidStream, false,
                    "Chat Completions stream continued after [DONE]."));
            }
            if (!FinishReasonValue.has_value()) {
                return MakeOutcomeUnknown(
                    "StreamEndedBeforeCompleteTerminator");
            }
            return MapFinishReason(*FinishReasonValue);
        }
        if (const auto* FailedValue = std::get_if<FailedTransport>(&Transport);
            FailedValue != nullptr) {
            return MapTransportFailure(FailedValue->Code);
        }
        if (std::holds_alternative<RedirectDeniedTransport>(Transport)) {
            return MakeFailed(MakeError(
                FailureCode::TransportFailure, false,
                "Provider redirect was denied by the transport contract."));
        }
        if (const auto* TimedOut = std::get_if<TimedOutTransport>(&Transport);
            TimedOut != nullptr) {
            if (!Started && !ChunkSeen) {
                return MakeFailed(MakeError(
                    FailureCode::TransportFailure, true,
                    "Provider request timed out before a response arrived."));
            }
            return MakeOutcomeUnknown("TransportTimedOutAfterRequestSent");
        }
        if (const auto* Cancelled = std::get_if<CancelledTransport>(&Transport);
            Cancelled != nullptr) {
            if (Cancelled->RequestMayHaveReachedEndpoint) {
                return MakeOutcomeUnknown("CancellationUnconfirmed");
            }
            return CanonicalTerminalEvent{ResponseCancelledEvent{
                Metadata(), CancelReasonCode{StableReasonCode{
                                "CancelledBeforeSend"}}}};
        }
        if (std::holds_alternative<UnknownOutcomeTransport>(Transport)) {
            return MakeOutcomeUnknown("StreamInterruptedAfterRequestSent");
        }
        return MakeOutcomeUnknown("UnknownTransportTerminal");
    }

private:
    struct ToolState final {
        std::string Id;
        std::string Name;
        bool TypeValidated{false};
        bool Started{false};
        std::string Arguments;
        std::vector<std::string> PendingDeltas;
    };

    ProviderEventMetadata Metadata() {
        return ProviderEventMetadata{RequestValue.Id, RequestValue.TurnId,
                                     NextSequence++};
    }

    CanonicalTerminalEvent MakeFailed(const ProviderError& Error) {
        auto Copy = Error;
        return ResponseFailedEvent{Metadata(), std::move(Copy)};
    }

    CanonicalTerminalEvent MakeOutcomeUnknown(std::string Reason) {
        return ResponseOutcomeUnknownEvent{
            Metadata(), OutcomeUnknownReasonCode{StableReasonCode{
                            std::move(Reason)}}};
    }

    void SetFatal(ProviderError Error) {
        if (!Fatal.has_value()) {
            Fatal = std::move(Error);
        }
    }

    void EnsureStarted() {
        if (Started) {
            return;
        }
        Started = true;
        ResponseStartedEvent Event;
        Event.Metadata = Metadata();
        Event.ProviderResponse = ProviderResponseId{ResponseId};
        EventSinkValue(CanonicalNonTerminalEvent{std::move(Event)});
    }

    std::string TextItemId() const {
        std::string Source = ResponseId.empty() ? "response" : ResponseId;
        return "item_" + Source + "_0";
    }

    std::string ToolItemId(std::size_t Index) const {
        std::string Source = ResponseId.empty() ? "response" : ResponseId;
        return "item_" + Source + "_tool_" + std::to_string(Index);
    }

    void DispatchLine(std::string_view RawLine) {
        std::string_view LineValue = RawLine;
        if (!LineValue.empty() && LineValue.back() == '\r') {
            LineValue.remove_suffix(1);
        }
        if (LineValue.empty()) {
            DispatchEvent();
            return;
        }
        if (LineValue.front() == ':') {
            return;
        }
        constexpr std::string_view DataPrefix = "data:";
        if (LineValue.substr(0, DataPrefix.size()) != DataPrefix) {
            return;
        }
        std::string_view Data = LineValue.substr(DataPrefix.size());
        if (!Data.empty() && Data.front() == ' ') {
            Data.remove_prefix(1);
        }
        if (!PendingData.empty()) {
            PendingData.push_back('\n');
        }
        PendingData.append(Data);
    }

    void DispatchEvent() {
        if (PendingData.empty()) {
            return;
        }
        const std::string Data = std::move(PendingData);
        PendingData.clear();
        if (DoneSeen) {
            if (Data == "[DONE]") {
                return;
            }
            DataAfterDone = true;
            return;
        }
        if (Data == "[DONE]") {
            DoneSeen = true;
            return;
        }
        json::value Value;
        if (!ParseJsonText(Data, Value)) {
            if (!Started) {
                EnsureStarted();
            }
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Malformed Chat Completions SSE payload."));
            return;
        }
        const json::object* Object = Value.if_object();
        if (Object == nullptr) {
            if (!Started) {
                EnsureStarted();
            }
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions frame is not a JSON object."));
            return;
        }
        ProcessFrame(*Object);
    }

    static bool IsAllowedEnvelopeField(std::string_view Name) {
        static constexpr std::array<std::string_view, 6> Allowed{
            "id", "object", "created", "model", "choices", "usage"};
        return std::find(Allowed.begin(), Allowed.end(), Name) != Allowed.end();
    }

    void ProcessFrame(const json::object& Frame) {
        for (const auto& Member : Frame) {
            const std::string Key(Member.key().data(), Member.key().size());
            if (!IsAllowedEnvelopeField(Key)) {
                if (!Started) {
                    ResponseId = TextOf(Frame, "id");
                    EnsureStarted();
                }
                SetFatal(MakeError(
                    FailureCode::UnsupportedCapability, false,
                    "Response contains an unregistered vendor field."));
                return;
            }
        }
        ChunkSeen = true;
        if (!Started) {
            ResponseId = TextOf(Frame, "id");
            EnsureStarted();
        }
        if (Fatal.has_value()) {
            return;
        }

        std::optional<UsageSnapshot> FrameUsage;
        if (const json::value* Usage = Frame.if_contains("usage");
            Usage != nullptr && !Usage->is_null()) {
            bool Invalid = false;
            const auto Parsed =
                ParseUsage(*Usage, ProfileValue.CompatibilityProfile.has_value(),
                           Invalid);
            if (Invalid || !Parsed.has_value()) {
                SetFatal(MakeError(FailureCode::InvalidStream, false,
                                   "Malformed Chat Completions usage payload."));
                return;
            }
            FrameUsage = *Parsed;
        }

        const json::value* ChoicesValue = Frame.if_contains("choices");
        const json::array* Choices = nullptr;
        if (ChoicesValue != nullptr) {
            Choices = ChoicesValue->if_array();
        }
        if (Choices == nullptr) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions frame is missing choices."));
            return;
        }
        if (Choices->empty()) {
            EmitUsageIfPresent(FrameUsage);
            return;
        }
        if (Choices->size() != 1) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions V1 accepts exactly one choice."));
            return;
        }
        const json::object* Choice = (*Choices)[0].if_object();
        if (Choice == nullptr) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions choice is not a JSON object."));
            return;
        }
        const auto Index = UnsignedField(*Choice, "index");
        if (Index.has_value() && *Index != 0) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions V1 only maps choice index 0."));
            return;
        }
        if (FinishReasonValue.has_value()) {
            SetFatal(MakeError(
                FailureCode::InvalidStream, false,
                "Chat Completions stream continued after a finish_reason."));
            return;
        }

        if (const json::value* DeltaValue = Choice->if_contains("delta");
            DeltaValue != nullptr && DeltaValue->is_object()) {
            ProcessDelta(DeltaValue->as_object());
        } else if (const json::value* MessageValue =
                       Choice->if_contains("message");
                   MessageValue != nullptr && MessageValue->is_object()) {
            ProcessDelta(MessageValue->as_object());
        }

        std::string_view FinishReason;
        if (KindOf(*Choice, "finish_reason", &FinishReason) ==
            FieldKind::String) {
            FinishReasonValue = std::string(FinishReason);
            if (*FinishReasonValue == "tool_calls") {
                FinalizeToolCalls();
            }
        }
        EmitUsageIfPresent(FrameUsage);
    }

    void EmitUsageIfPresent(const std::optional<UsageSnapshot>& FrameUsage) {
        if (!FrameUsage.has_value()) {
            return;
        }
        LastUsage = *FrameUsage;
        UsageUpdatedEvent Event;
        Event.Metadata = Metadata();
        Event.Usage = *FrameUsage;
        EventSinkValue(CanonicalNonTerminalEvent{std::move(Event)});
    }

    void ProcessDelta(const json::object& Delta) {
        std::string_view Role;
        const FieldKind RoleKind = KindOf(Delta, "role", &Role);
        if (RoleKind == FieldKind::String && Role != "assistant") {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions streamed an unexpected role."));
            return;
        }
        if (RoleKind == FieldKind::Other) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions role is not a string."));
            return;
        }

        if (const json::value* FunctionCall = Delta.if_contains("function_call");
            FunctionCall != nullptr && !FunctionCall->is_null()) {
            SetFatal(MakeError(FailureCode::UnsupportedCapability, false,
                               "V1 does not support legacy function_call."));
            return;
        }

        std::string_view Refusal;
        const FieldKind RefusalKind = KindOf(Delta, "refusal", &Refusal);
        if (RefusalKind == FieldKind::String) {
            if (Refusal.empty()) {
                RefusalMarker = true;
            } else {
                if (SawContent) {
                    SetFatal(MakeError(
                        FailureCode::InvalidStream, false,
                        "Chat Completions refusal conflicts with text content."));
                    return;
                }
                RefusalMarker = true;
                RefusalDeltaEvent Event;
                Event.Metadata = Metadata();
                Event.Item = ItemId{TextItemId()};
                Event.Utf8Delta = std::string(Refusal);
                EventSinkValue(CanonicalNonTerminalEvent{std::move(Event)});
            }
        } else if (RefusalKind == FieldKind::Other) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions refusal must be a string."));
            return;
        }

        std::string_view Content;
        const FieldKind ContentKind = KindOf(Delta, "content", &Content);
        if (ContentKind == FieldKind::String) {
            if (!Content.empty()) {
                if (RefusalMarker) {
                    SetFatal(MakeError(
                        FailureCode::InvalidStream, false,
                        "Chat Completions text content conflicts with refusal."));
                    return;
                }
                SawContent = true;
                TextDeltaEvent Event;
                Event.Metadata = Metadata();
                Event.Item = ItemId{TextItemId()};
                Event.Utf8Delta = std::string(Content);
                EventSinkValue(CanonicalNonTerminalEvent{std::move(Event)});
            }
        } else if (ContentKind == FieldKind::Other) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions content is not a string."));
            return;
        }

        const json::value* ToolCalls = Delta.if_contains("tool_calls");
        if (ToolCalls == nullptr || ToolCalls->is_null()) {
            return;
        }
        const json::array* Calls = ToolCalls->if_array();
        if (Calls == nullptr) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions tool_calls is not an array."));
            return;
        }
        for (const json::value& CallValue : *Calls) {
            const json::object* Call = CallValue.if_object();
            if (Call == nullptr) {
                SetFatal(MakeError(FailureCode::InvalidStream, false,
                                   "Chat Completions tool call is not an object."));
                return;
            }
            const auto Index = UnsignedField(*Call, "index");
            if (!Index.has_value()) {
                SetFatal(MakeError(FailureCode::InvalidStream, false,
                                   "Chat Completions tool call has no index."));
                return;
            }
            ProcessToolCall(static_cast<std::size_t>(*Index), *Call);
            if (Fatal.has_value()) {
                return;
            }
        }
    }

    void ProcessToolCall(std::size_t Index, const json::object& Call) {
        if (RefusalMarker) {
            SetFatal(MakeError(
                FailureCode::InvalidStream, false,
                "Chat Completions tool call conflicts with refusal."));
            return;
        }
        ToolState& State = Tools[Index];
        std::string_view Type;
        const FieldKind TypeKind = KindOf(Call, "type", &Type);
        if (TypeKind == FieldKind::String) {
            if (Type != "function") {
                SetFatal(MakeError(FailureCode::UnsupportedCapability, false,
                                   "V1 only supports function tool calls."));
                return;
            }
            State.TypeValidated = true;
        } else if (TypeKind == FieldKind::Other) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions tool type is not a string."));
            return;
        }

        std::string_view CallId;
        if (KindOf(Call, "id", &CallId) == FieldKind::String &&
            !CallId.empty()) {
            if (!State.Id.empty() && State.Id != CallId) {
                SetFatal(MakeError(FailureCode::InvalidStream, false,
                                   "Tool call id changed within one index."));
                return;
            }
            State.Id = std::string(CallId);
        }

        const json::value* FunctionValue = Call.if_contains("function");
        if (FunctionValue == nullptr || !FunctionValue->is_object()) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Chat Completions tool call has no function."));
            return;
        }
        const json::object& Function = FunctionValue->as_object();
        std::string_view Name;
        if (KindOf(Function, "name", &Name) == FieldKind::String &&
            !Name.empty()) {
            if (!State.Name.empty() && State.Name != Name) {
                SetFatal(MakeError(FailureCode::InvalidStream, false,
                                   "Tool name changed within one index."));
                return;
            }
            State.Name = std::string(Name);
        }
        std::string_view Arguments;
        if (KindOf(Function, "arguments", &Arguments) == FieldKind::String) {
            if (!Arguments.empty()) {
                State.Arguments.append(Arguments);
                State.PendingDeltas.emplace_back(Arguments);
            }
        } else if (Function.if_contains("arguments") != nullptr) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "Tool arguments must be a string delta."));
            return;
        }

        if (!State.Started && !State.Id.empty() && !State.Name.empty()) {
            if (TypeKind == FieldKind::String && !State.TypeValidated) {
                return;
            }
            StartTool(Index, State);
        } else if (State.Started) {
            EmitPendingToolArguments(Index, State);
        }
    }

    void StartTool(std::size_t Index, ToolState& State) {
        State.Started = true;
        ToolCallStartedEvent Event;
        Event.Metadata = Metadata();
        Event.Item = ItemId{ToolItemId(Index)};
        Event.Call = ToolCallId{State.Id};
        Event.ToolName = State.Name;
        EventSinkValue(CanonicalNonTerminalEvent{std::move(Event)});
        EmitPendingToolArguments(Index, State);
    }

    void EmitPendingToolArguments(std::size_t Index, ToolState& State) {
        for (const std::string& Delta : State.PendingDeltas) {
            ToolArgumentDeltaEvent Event;
            Event.Metadata = Metadata();
            Event.Item = ItemId{ToolItemId(Index)};
            Event.Call = ToolCallId{State.Id};
            Event.JsonTextDelta = Delta;
            EventSinkValue(CanonicalNonTerminalEvent{std::move(Event)});
        }
        State.PendingDeltas.clear();
    }

    void FinalizeToolCalls() {
        if (Tools.empty()) {
            SetFatal(MakeError(FailureCode::InvalidStream, false,
                               "tool_calls finish_reason without tool calls."));
            return;
        }
        for (const auto& Entry : Tools) {
            const ToolState& State = Entry.second;
            if (!State.Started || !IsCompleteJson(State.Arguments)) {
                SetFatal(MakeError(
                    FailureCode::InvalidStream, false,
                    "Tool arguments are incomplete JSON at tool_calls terminal."));
                return;
            }
        }
        for (const auto& Entry : Tools) {
            const std::size_t Index = Entry.first;
            const ToolState& State = Entry.second;
            ToolCallReadyEvent Event;
            Event.Metadata = Metadata();
            Event.Item = ItemId{ToolItemId(Index)};
            Event.Call = ToolCallId{State.Id};
            Event.ToolName = State.Name;
            Event.Arguments = JsonValue{State.Arguments};
            EventSinkValue(CanonicalNonTerminalEvent{std::move(Event)});
        }
    }

    UsageSnapshot UsageOrUnavailable() const {
        if (LastUsage.has_value()) {
            return *LastUsage;
        }
        UsageSnapshot Unknown;
        Unknown.Completeness = UsageCompleteness::Unavailable;
        return Unknown;
    }

    CanonicalTerminalEvent MapFinishReason(const std::string& Reason) {
        if (Reason == "stop") {
            if (RefusalMarker) {
                if (SawContent || !Tools.empty()) {
                    return MakeFailed(MakeError(
                        FailureCode::InvalidStream, false,
                        "Chat Completions refusal conflicts with response "
                        "content or tool calls."));
                }
                ResponseCompletedEvent Event;
                Event.Metadata = Metadata();
                Event.Kind = CompletionKind::Refusal;
                Event.FinalUsage = UsageOrUnavailable();
                return Event;
            }
            ResponseCompletedEvent Event;
            Event.Metadata = Metadata();
            Event.Kind = CompletionKind::FinalAnswer;
            Event.FinalUsage = UsageOrUnavailable();
            return Event;
        }
        if (Reason == "length") {
            ResponseOutputLimitedEvent Event;
            Event.Metadata = Metadata();
            Event.Kind = LimitKind::OutputTokens;
            Event.Usage = UsageOrUnavailable();
            return Event;
        }
        if (Reason == "content_filter") {
            return MakeFailed(MakeError(
                FailureCode::ContentFiltered, false,
                "Provider content filter stopped the completion."));
        }
        if (Reason == "tool_calls") {
            if (RefusalMarker) {
                return MakeFailed(MakeError(
                    FailureCode::InvalidStream, false,
                    "Chat Completions refusal conflicts with tool calls."));
            }
            ResponseCompletedEvent Event;
            Event.Metadata = Metadata();
            Event.Kind = CompletionKind::ToolHandoff;
            Event.FinalUsage = UsageOrUnavailable();
            return Event;
        }
        if (Reason == "function_call") {
            return MakeFailed(MakeError(
                FailureCode::UnsupportedCapability, false,
                "V1 does not support legacy function_call."));
        }
        return MakeFailed(MakeError(FailureCode::InvalidStream, false,
                                    "Unknown finish_reason is not treated as "
                                    "success."));
    }

    CanonicalTerminalEvent MapHttpStatus(std::uint16_t Status) {
        if (Status == 401 || Status == 403) {
            return MakeFailed(MakeError(FailureCode::AuthenticationFailure,
                                        false,
                                        "Provider authentication failed."));
        }
        if (Status == 429) {
            return MakeFailed(MakeError(FailureCode::RateLimited, true,
                                        "Synthetic provider rate limit."));
        }
        if (Status == 404) {
            return MakeFailed(MakeError(FailureCode::ProviderRejected, false,
                                        "Provider endpoint was not found."));
        }
        if (Status >= 500) {
            if (Status == 501) {
                return MakeFailed(MakeError(FailureCode::ProviderRejected,
                                            false,
                                            "Provider rejected the request."));
            }
            return MakeFailed(MakeError(FailureCode::TransportFailure, true,
                                        "Provider service failed."));
        }
        if (HttpErrorCode == "context_length_exceeded") {
            ResponseOutputLimitedEvent Event;
            Event.Metadata = Metadata();
            Event.Kind = LimitKind::ContextWindow;
            Event.Usage = UsageOrUnavailable();
            return Event;
        }
        return MakeFailed(MakeError(FailureCode::InvalidRequest, false,
                                    "Provider rejected the request."));
    }

    CanonicalTerminalEvent MapTransportFailure(
        Platform::Network::TransportFailureCode Code) {
        using Platform::Network::TransportFailureCode;
        switch (Code) {
        case TransportFailureCode::InvalidRequest:
            return MakeFailed(MakeError(FailureCode::InvalidRequest, false,
                                        "Transport rejected the request."));
        case TransportFailureCode::EndpointNotGranted:
            return MakeFailed(MakeError(FailureCode::InvalidProfile, false,
                                        "Endpoint grant is not valid."));
        case TransportFailureCode::DnsFailure:
        case TransportFailureCode::ConnectFailure:
            return MakeFailed(MakeError(FailureCode::TransportFailure, true,
                                        "Provider connection failed."));
        case TransportFailureCode::TlsHandshakeFailure:
            return MakeFailed(MakeError(FailureCode::TransportFailure, false,
                                        "Provider TLS handshake failed."));
        case TransportFailureCode::ProxyAuthenticationRequired:
            return MakeFailed(MakeError(FailureCode::AuthenticationFailure,
                                        false,
                                        "Proxy authentication is required."));
        case TransportFailureCode::RequestWriteFailure:
        case TransportFailureCode::ResponseReadFailure:
            return MakeOutcomeUnknown("TransportStreamInterrupted");
        case TransportFailureCode::ResponseLimitExceeded:
        case TransportFailureCode::HeaderLimitExceeded:
        case TransportFailureCode::InvalidHttpResponse:
            return MakeFailed(MakeError(FailureCode::InvalidStream, false,
                                        "Provider response violated transport "
                                        "limits."));
        case TransportFailureCode::InternalError:
            return MakeFailed(MakeError(FailureCode::TransportFailure, false,
                                        "Transport internal error."));
        }
        return MakeFailed(MakeError(FailureCode::TransportFailure, false,
                                    "Transport internal error."));
    }

    const ModelProviderProfile& ProfileValue;
    const CanonicalModelRequest& RequestValue;
    const EventSink& EventSinkValue;
    std::string Line;
    std::string PendingData;
    std::string ResponseId;
    std::string HttpErrorCode;
    std::map<std::size_t, ToolState> Tools;
    std::optional<UsageSnapshot> LastUsage;
    std::optional<ProviderError> Fatal;
    std::optional<std::string> FinishReasonValue;
    std::uint64_t NextSequence{1};
    bool Started{false};
    bool ChunkSeen{false};
    bool SawContent{false};
    bool RefusalMarker{false};
    bool DoneSeen{false};
    bool DataAfterDone{false};
};

std::string SerializeRequest(const ModelProviderProfile& Profile,
                             const CanonicalModelRequest& Request,
                             const RequestOptions& Options) {
    json::object Body;
    Body.emplace("model", Request.ModelName.empty() ? Profile.ModelName
                                                    : Request.ModelName);

    json::array Messages;
    const auto AppendTextMessage = [&Messages](std::string_view Role,
                                               std::string Text) {
        json::object Message;
        Message.emplace("role", std::string(Role));
        Message.emplace("content", std::move(Text));
        Messages.emplace_back(std::move(Message));
    };
    if (!Request.SystemInstructions.empty()) {
        AppendTextMessage("system", Request.SystemInstructions);
    }
    if (!Request.DeveloperInstructions.empty()) {
        AppendTextMessage("developer", Request.DeveloperInstructions);
    }
    for (const CanonicalMessage& Message : Request.Messages) {
        json::object Wire;
        switch (Message.Role) {
        case MessageRole::System:
            Wire.emplace("role", "system");
            break;
        case MessageRole::Developer:
            Wire.emplace("role", "developer");
            break;
        case MessageRole::User:
            Wire.emplace("role", "user");
            break;
        case MessageRole::Assistant:
            Wire.emplace("role", "assistant");
            break;
        case MessageRole::Tool:
            Wire.emplace("role", "tool");
            break;
        }
        if (Message.Role == MessageRole::Tool) {
            if (!Message.ToolCallResultId.has_value() ||
                Message.ToolCallResultId->Value.empty()) {
                return {};
            }
            Wire.emplace("tool_call_id", Message.ToolCallResultId->Value);
        }
        bool HasImage = false;
        for (const ContentPart& Part : Message.ContentParts) {
            if (std::holds_alternative<ImageContentReference>(Part)) {
                HasImage = true;
            }
        }
        if (HasImage && !Profile.Capabilities.Vision) {
            return {};
        }
        if (HasImage) {
            json::array Content;
            for (const ContentPart& Part : Message.ContentParts) {
                if (const auto* Text = std::get_if<TextContentPart>(&Part)) {
                    json::object Item;
                    Item.emplace("type", "text");
                    Item.emplace("text", Text->Text);
                    Content.emplace_back(std::move(Item));
                } else if (const auto* Image =
                               std::get_if<ImageContentReference>(&Part)) {
                    json::object Item;
                    Item.emplace("type", "image_url");
                    json::object Url;
                    Url.emplace("url", Image->OpaqueReference);
                    Item.emplace("image_url", std::move(Url));
                    Content.emplace_back(std::move(Item));
                }
            }
            Wire.emplace("content", std::move(Content));
        } else {
            std::string Text = JoinTextParts(Message.ContentParts);
            if (Message.Role == MessageRole::Assistant &&
                !Message.FunctionCalls.empty()) {
                json::array Calls;
                for (const FunctionCall& Call : Message.FunctionCalls) {
                    json::value Arguments;
                    if (!ParseJsonText(Call.Arguments.JsonText, Arguments)) {
                        return {};
                    }
                    json::object Function;
                    Function.emplace("name", Call.Name);
                    Function.emplace(
                        "arguments",
                        json::string(Call.Arguments.JsonText));
                    json::object Item;
                    Item.emplace("id", Call.Id.Value);
                    Item.emplace("type", "function");
                    Item.emplace("function", std::move(Function));
                    Calls.emplace_back(std::move(Item));
                }
                Wire.emplace("content", nullptr);
                Wire.emplace("tool_calls", std::move(Calls));
            } else {
                Wire.emplace("content", std::move(Text));
            }
        }
        if (json::value* Existing = Wire.if_contains("content");
            Existing == nullptr) {
            Wire.emplace("content", "");
        }
        Messages.emplace_back(std::move(Wire));
    }
    Body.emplace("messages", std::move(Messages));

    if (!Request.ToolDefinitions.empty()) {
        json::array Tools;
        for (const ToolDefinition& Definition : Request.ToolDefinitions) {
            json::value Schema;
            if (!ParseJsonText(Definition.Parameters.JsonText, Schema)) {
                return {};
            }
            json::object Function;
            Function.emplace("name", Definition.Name);
            Function.emplace("description", Definition.Description);
            Function.emplace("parameters", std::move(Schema));
            json::object Tool;
            Tool.emplace("type", "function");
            Tool.emplace("function", std::move(Function));
            Tools.emplace_back(std::move(Tool));
        }
        Body.emplace("tools", std::move(Tools));
    }
    if (Options.ToolChoice.has_value()) {
        Body.emplace("tool_choice", *Options.ToolChoice);
    }
    if (Request.Limit.MaximumOutputTokens.has_value()) {
        Body.emplace("max_completion_tokens", *Request.Limit.MaximumOutputTokens);
    }
    Body.emplace("n", 1);
    if (Request.Sampling.Temperature.has_value()) {
        Body.emplace("temperature", *Request.Sampling.Temperature);
    }
    if (Request.Sampling.TopP.has_value()) {
        Body.emplace("top_p", *Request.Sampling.TopP);
    }
    Body.emplace("stream", true);
    if (Options.IncludeUsage) {
        json::object StreamOptions;
        StreamOptions.emplace("include_usage", true);
        Body.emplace("stream_options", std::move(StreamOptions));
    }
    if (Options.ServiceTier.has_value()) {
        Body.emplace("service_tier", *Options.ServiceTier);
    }
    return json::serialize(Body);
}

} // namespace

ChatCompletionsProvider::ChatCompletionsProvider(
    Platform::Network::HttpTransportPort& InTransport,
    EndpointGrantProvider ResolveGrant)
    : Transport(InTransport), Grants(std::move(ResolveGrant)) {}

ProviderResult<std::monostate> ChatCompletionsProvider::ValidateProfile(
    const ModelProviderProfile& Profile) const {
    const auto Result = ValidateAndCacheProfile(Profile);
    if (const auto* Error = std::get_if<ProviderError>(&Result)) {
        return *Error;
    }
    return std::monostate{};
}

ProviderResult<ChatCompletionsProvider::ProfileState>
ChatCompletionsProvider::ValidateAndCacheProfile(
    const ModelProviderProfile& Profile) const {
    if (Profile.Protocol != ProviderProtocol::OpenAICompatibleChatCompletions) {
        return MakeError(FailureCode::InvalidProfile, false,
                         "Profile protocol is not Chat Completions.");
    }
    if (Profile.Authentication != AuthenticationScheme::Bearer) {
        return MakeError(FailureCode::InvalidProfile, false,
                         "Chat Completions V1 requires Bearer authentication.");
    }
    if (Profile.ModelName.empty() || Profile.ProtocolVersion.empty()) {
        return MakeError(FailureCode::InvalidProfile, false,
                         "Profile model name and protocol version are required.");
    }
    if (Profile.Id.Value.empty()) {
        return MakeError(FailureCode::InvalidProfile, false,
                         "Profile id is required.");
    }
    if (!Profile.Capabilities.Streaming) {
        return MakeError(FailureCode::InvalidProfile, false,
                         "Chat Completions V1 requires streaming capability.");
    }
    const auto Destination = ParseHttpsEndpoint(Profile.Endpoint);
    if (!Destination.has_value()) {
        return MakeError(FailureCode::InvalidProfile, false,
                         "Profile endpoint must be an HTTPS destination.");
    }
    if (Profile.Sampling.TopK.has_value()) {
        return MakeError(FailureCode::UnsupportedCapability, false,
                         "Chat Completions V1 does not support top_k.");
    }
    ProfileState State{Profile, *Destination};
    {
        const std::lock_guard<std::mutex> Lock(Mutex);
        Profiles[Profile.Id.Value] = State;
    }
    return State;
}

ProviderResult<ProviderCapabilities> ChatCompletionsProvider::GetCapabilities(
    const ProviderProfileId& ProfileId) const {
    const std::lock_guard<std::mutex> Lock(Mutex);
    const auto Found = Profiles.find(ProfileId.Value);
    if (Found == Profiles.end()) {
        return MakeError(FailureCode::InvalidProfile, false,
                         "Profile must be validated before use.");
    }
    return Found->second.Profile.Capabilities;
}

CanonicalTerminalEvent ChatCompletionsProvider::StreamTurn(
    const ModelProviderProfile& Profile, const CredentialLease& Lease,
    const CanonicalModelRequest& Request, std::stop_token CancellationToken,
    const EventSink& Sink) {
    const auto EmitCancelledBeforeSend = [&Request]() -> CanonicalTerminalEvent {
        if (Request.Id.Value.empty()) {
            return Failed(FailureCode::InvalidRequest, false,
                          "Canonical request id is required.", 1);
        }
        ResponseCancelledEvent Event;
        Event.Metadata = ProviderEventMetadata{
            Request.Id, Request.TurnId, 1};
        Event.Reason =
            CancelReasonCode{StableReasonCode{"CancelledBeforeSend"}};
        return Event;
    };
    if (CancellationToken.stop_requested()) {
        return EmitCancelledBeforeSend();
    }

    const auto Validation = ValidateAndCacheProfile(Profile);
    if (const auto* Error = std::get_if<ProviderError>(&Validation)) {
        const ProviderError Copy = *Error;
        return ResponseFailedEvent{
            ProviderEventMetadata{Request.Id, Request.TurnId, 1}, Copy};
    }
    const ProfileState State = std::get<ProfileState>(Validation);

    RequestOptions Options;
    if (const auto OptionError = ReadRequestOptions(Request, Profile, Options);
        OptionError.has_value()) {
        return ResponseFailedEvent{
            ProviderEventMetadata{Request.Id, Request.TurnId, 1},
            *OptionError};
    }

    const std::string Body = SerializeRequest(Profile, Request, Options);
    if (Body.empty()) {
        return Failed(FailureCode::InvalidRequest, false,
                      "Canonical request cannot be mapped to Chat "
                      "Completions.",
                      1);
    }

    TransportRequest WireRequest;
    WireRequest.Id =
        Platform::Network::TransportRequestId{Request.Id.Value};
    WireRequest.Grant = Grants(Profile);
    WireRequest.Method = Platform::Network::TransportMethod::Post;
    WireRequest.Destination = State.Destination;
    WireRequest.Headers.push_back(
        Platform::Network::TransportHeader{"Content-Type", "application/json"});
    WireRequest.Headers.push_back(
        Platform::Network::TransportHeader{"Accept", "text/event-stream"});
    const std::string Secret(Lease.GetAuthenticationSecret());
    WireRequest.Headers.push_back(
        Platform::Network::TransportHeader{"Authorization",
                                           "Bearer " + Secret});
    WireRequest.BodyUtf8 = Body;
    if (Profile.Timeouts.ConnectionTimeout.count() > 0) {
        WireRequest.Timeouts.Connect = Profile.Timeouts.ConnectionTimeout;
    }
    if (Profile.Timeouts.RequestTimeout.count() > 0) {
        WireRequest.Timeouts.Send = Profile.Timeouts.RequestTimeout;
        WireRequest.Timeouts.Response = Profile.Timeouts.RequestTimeout;
        WireRequest.Timeouts.Idle = Profile.Timeouts.RequestTimeout;
    }

    TurnProcessor Processor(Profile, Request, Sink);
    std::uint16_t StatusCode = 0;
    std::string ErrorBody;
    const Platform::Network::ResponseHeadSink HeadSink =
        [&StatusCode](const ResponseHeadMetadata& Head) {
            StatusCode = Head.StatusCode;
        };
    const Platform::Network::ResponseChunkSink ChunkSink =
        [&Processor, &StatusCode, &ErrorBody](std::span<const std::byte> Chunk) {
            if (StatusCode != 0 && StatusCode != 200) {
                if (ErrorBody.size() < 65536U) {
                    const std::size_t Remaining = 65536U - ErrorBody.size();
                    const std::size_t Count = std::min(Remaining, Chunk.size());
                    ErrorBody.append(
                        reinterpret_cast<const char*>(Chunk.data()), Count);
                }
                return;
            }
            Processor.Feed(Chunk);
        };

    TransportTerminal Terminal =
        Transport.Stream(WireRequest, HeadSink, ChunkSink, CancellationToken);
    Processor.FinishStream();
    if (StatusCode != 0 && StatusCode != 200 && !ErrorBody.empty()) {
        json::value ErrorValue;
        if (ParseJsonText(ErrorBody, ErrorValue)) {
            if (const json::object* ErrorObject = ErrorValue.if_object();
                ErrorObject != nullptr) {
                if (const json::value* ErrorField =
                        ErrorObject->if_contains("error");
                    ErrorField != nullptr && ErrorField->is_object()) {
                    Processor.SetHttpErrorCode(
                        TextOf(ErrorField->as_object(), "code"));
                    if (Processor.GetHttpErrorCode().empty()) {
                        Processor.SetHttpErrorCode(
                            TextOf(ErrorField->as_object(), "type"));
                    }
                }
            }
        }
    }
    return Processor.TerminalForTransport(Terminal, StatusCode);
}

ProviderResult<ProviderHealthStatus> ChatCompletionsProvider::HealthCheck(
    const ModelProviderProfile& Profile) {
    const auto Validation = ValidateAndCacheProfile(Profile);
    if (const auto* Error = std::get_if<ProviderError>(&Validation)) {
        return *Error;
    }
    const Platform::Network::TransportHealthState State =
        Transport.Health(std::stop_token{});
    ProviderHealthStatus Status;
    Status.State = State == Platform::Network::TransportHealthState::Ready
                       ? ProviderHealthState::Available
                       : ProviderHealthState::Unavailable;
    return Status;
}

} // namespace EmulativeDialog::Providers::OpenAIChatCompletions
