#include "Source/Providers/OpenAIResponses/OpenAIResponsesProvider.hpp"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace Json = boost::json;
namespace Net = EmulativeDialog::Platform::Network;
using namespace EmulativeDialog::Providers;
using EmulativeDialog::Providers::OpenAIResponses::OpenAIResponsesProvider;

void Expect(bool Condition, const char* File = __builtin_FILE(),
            int Line = __builtin_LINE()) {
    if (!Condition) {
        std::fprintf(stderr, "EXPECT failed at %s:%d\n", File, Line);
        std::abort();
    }
}

std::string_view StringView(const Json::object& Object,
                            std::string_view Key) {
    const Json::value* Value = Object.if_contains(Key);
    if (Value == nullptr || !Value->is_string()) {
        return {};
    }
    const Json::string& Text = Value->as_string();
    return std::string_view(Text.data(), Text.size());
}

const Json::array* ArrayField(const Json::object& Object,
                              std::string_view Key) {
    const Json::value* Value = Object.if_contains(Key);
    return Value != nullptr && Value->is_array() ? &Value->as_array() : nullptr;
}

std::string_view EventTypeName(EventType Type) {
    switch (Type) {
    case EventType::ResponseStarted: return "ResponseStarted";
    case EventType::TextDelta: return "TextDelta";
    case EventType::RefusalDelta: return "RefusalDelta";
    case EventType::ToolCallStarted: return "ToolCallStarted";
    case EventType::ToolArgumentDelta: return "ToolArgumentDelta";
    case EventType::ToolCallReady: return "ToolCallReady";
    case EventType::UsageUpdated: return "UsageUpdated";
    case EventType::ResponseCompleted: return "ResponseCompleted";
    case EventType::ResponseOutputLimited: return "ResponseOutputLimited";
    case EventType::ResponseFailed: return "ResponseFailed";
    case EventType::ResponseCancelled: return "ResponseCancelled";
    case EventType::ResponseOutcomeUnknown: return "ResponseOutcomeUnknown";
    }
    return "Unknown";
}

std::string_view CompletionKindName(CompletionKind Kind) {
    switch (Kind) {
    case CompletionKind::FinalAnswer: return "FinalAnswer";
    case CompletionKind::ToolHandoff: return "ToolHandoff";
    case CompletionKind::Refusal: return "Refusal";
    }
    return "Unknown";
}

std::string_view FailureCodeName(FailureCode Code) {
    switch (Code) {
    case FailureCode::InvalidProfile: return "InvalidProfile";
    case FailureCode::InvalidRequest: return "InvalidRequest";
    case FailureCode::UnsupportedCapability: return "UnsupportedCapability";
    case FailureCode::AuthenticationFailure: return "AuthenticationFailure";
    case FailureCode::RateLimited: return "RateLimited";
    case FailureCode::TransportFailure: return "TransportFailure";
    case FailureCode::InvalidStream: return "InvalidStream";
    case FailureCode::ProviderRejected: return "ProviderRejected";
    case FailureCode::ContentFiltered: return "ContentFiltered";
    }
    return "Unknown";
}

std::string_view UsageCompletenessName(UsageCompleteness Value) {
    switch (Value) {
    case UsageCompleteness::Partial: return "Partial";
    case UsageCompleteness::Final: return "Final";
    case UsageCompleteness::Unavailable: return "Unavailable";
    }
    return "Unavailable";
}

std::string FixturePath(std::string_view Name) {
    std::string Path = EMULATIVE_DIALOG_RESPONSES_FIXTURE_DIR;
    Path.append("/");
    Path.append(Name);
    return Path;
}

Json::value LoadFixture(std::string_view Name) {
    std::ifstream Input(FixturePath(Name), std::ios::binary);
    Expect(Input.good());
    std::string Text((std::istreambuf_iterator<char>(Input)),
                     std::istreambuf_iterator<char>());
    boost::system::error_code Error;
    Json::value Parsed = Json::parse(Text, Error);
    Expect(!Error);
    return Parsed;
}

class FixedCredentialLease final : public CredentialLease {
public:
    explicit FixedCredentialLease(std::string Secret)
        : Value(std::move(Secret)) {}

    [[nodiscard]] std::string_view GetAuthenticationSecret()
        const noexcept override {
        return Value;
    }

private:
    std::string Value;
};

struct FakeResponse final {
    int StatusCode{200};
    std::vector<std::string> Chunks;
    Net::TransportTerminal Terminal{Net::CompletedTransport{
        .StatusCode = 200, .ReceivedBodyBytes = 0}};
};

class FakeHttpTransport final : public Net::HttpTransportPort {
public:
    explicit FakeHttpTransport(FakeResponse Response)
        : Script(std::move(Response)) {}

    [[nodiscard]] Net::TransportTerminal Stream(
        const Net::TransportRequest& Request, const Net::ResponseHeadSink& OnHead,
        const Net::ResponseChunkSink& OnChunk,
        std::stop_token CancellationToken) override {
        ++CallCount;
        LastRequest = Request;
        if (CancellationToken.stop_requested()) {
            return Net::CancelledTransport{
                .RequestMayHaveReachedEndpoint = false};
        }
        if (OnHead) {
            OnHead(Net::ResponseHeadMetadata{
                .StatusCode = static_cast<std::uint16_t>(Script.StatusCode),
                .ContentType = "text/event-stream",
                .RetryAfter = std::nullopt});
        }
        for (const std::string& Chunk : Script.Chunks) {
            if (CancellationToken.stop_requested()) {
                return Net::CancelledTransport{
                    .RequestMayHaveReachedEndpoint = true};
            }
            if (OnChunk) {
                OnChunk(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(Chunk.data()),
                    Chunk.size()));
            }
        }
        return Script.Terminal;
    }

    [[nodiscard]] Net::TransportHealthState Health(
        std::stop_token) noexcept override {
        return Net::TransportHealthState::Ready;
    }

    int CallCount{0};
    Net::TransportRequest LastRequest;

private:
    FakeResponse Script;
};

struct ObservedEvents final {
    std::vector<Json::object> Events;

    [[nodiscard]] const Json::object& At(std::size_t Index) const {
        Expect(Index < Events.size());
        return Events[Index];
    }
};

Json::object PayloadFromEvent(const CanonicalNonTerminalEvent& Event) {
    return std::visit(
        [](const auto& Value) -> Json::object {
            using Type = std::decay_t<decltype(Value)>;
            if constexpr (std::is_same_v<Type, ResponseStartedEvent>) {
                Json::object Payload;
                if (Value.ProviderResponse.has_value()) {
                    Payload["ProviderResponseId"] =
                        Value.ProviderResponse->Value;
                }
                return Payload;
            } else if constexpr (std::is_same_v<Type, TextDeltaEvent>) {
                return Json::object{{"Text", Value.Utf8Delta}};
            } else if constexpr (std::is_same_v<Type, RefusalDeltaEvent>) {
                return Json::object{{"Text", Value.Utf8Delta}};
            } else if constexpr (std::is_same_v<Type, ToolCallStartedEvent>) {
                return Json::object{{"ToolName", Value.ToolName}};
            } else if constexpr (std::is_same_v<Type,
                                                ToolArgumentDeltaEvent>) {
                return Json::object{{"JsonText", Value.JsonTextDelta}};
            } else if constexpr (std::is_same_v<Type, ToolCallReadyEvent>) {
                boost::system::error_code ParseError;
                Json::value Arguments =
                    Json::parse(Value.Arguments.JsonText, ParseError);
                return Json::object{{"ToolName", Value.ToolName},
                                    {"Arguments", std::move(Arguments)}};
            } else if constexpr (std::is_same_v<Type, UsageUpdatedEvent>) {
                Json::object Usage;
                if (Value.Usage.InputTokens.has_value()) {
                    Usage["InputTokens"] = *Value.Usage.InputTokens;
                }
                if (Value.Usage.OutputTokens.has_value()) {
                    Usage["OutputTokens"] = *Value.Usage.OutputTokens;
                }
                if (Value.Usage.TotalTokens.has_value()) {
                    Usage["TotalTokens"] = *Value.Usage.TotalTokens;
                }
                if (Value.Usage.CachedInputReadTokens.has_value()) {
                    Usage["CachedInputReadTokens"] =
                        *Value.Usage.CachedInputReadTokens;
                }
                if (Value.Usage.CachedInputWriteTokens.has_value()) {
                    Usage["CachedInputWriteTokens"] =
                        *Value.Usage.CachedInputWriteTokens;
                }
                if (Value.Usage.ReasoningOutputTokens.has_value()) {
                    Usage["ReasoningOutputTokens"] =
                        *Value.Usage.ReasoningOutputTokens;
                }
                Usage["Completeness"] =
                    UsageCompletenessName(Value.Usage.Completeness);
                return Json::object{{"UsageSnapshot", std::move(Usage)}};
            }
            return {};
        },
        Event);
}

Json::object ToObserved(const CanonicalNonTerminalEvent& Event,
                        const CanonicalModelRequest& Request) {
    return std::visit(
        [&Request](const auto& Value) -> Json::object {
            Json::object Observed{
                {"RequestId", Request.Id.Value},
                {"AgentTurnId", Request.TurnId.Value},
                {"SequenceNumber", Value.Metadata.SequenceNumber},
                {"EventType", std::string(EventTypeName(Value.Type))},
                {"Payload", PayloadFromEvent(CanonicalNonTerminalEvent{Value})}};
            using Type = std::decay_t<decltype(Value)>;
            if constexpr (std::is_same_v<Type, TextDeltaEvent> ||
                          std::is_same_v<Type, RefusalDeltaEvent> ||
                          std::is_same_v<Type, ToolCallStartedEvent> ||
                          std::is_same_v<Type, ToolArgumentDeltaEvent> ||
                          std::is_same_v<Type, ToolCallReadyEvent>) {
                Observed["ItemId"] = Value.Item.Value;
            }
            if constexpr (std::is_same_v<Type, ToolCallStartedEvent> ||
                          std::is_same_v<Type, ToolArgumentDeltaEvent> ||
                          std::is_same_v<Type, ToolCallReadyEvent>) {
                Observed["ToolCallId"] = Value.Call.Value;
            }
            return Observed;
        },
        Event);
}

Json::object PayloadFromTerminal(const CanonicalTerminalEvent& Event) {
    return std::visit(
        [](const auto& Value) -> Json::object {
            using Type = std::decay_t<decltype(Value)>;
            if constexpr (std::is_same_v<Type, ResponseCompletedEvent>) {
                Json::object Payload{
                    {"CompletionKind", CompletionKindName(Value.Kind)}};
                return Payload;
            } else if constexpr (std::is_same_v<Type,
                                                ResponseOutputLimitedEvent>) {
                Json::object Usage;
                if (Value.Usage.InputTokens.has_value()) {
                    Usage["InputTokens"] = *Value.Usage.InputTokens;
                }
                if (Value.Usage.OutputTokens.has_value()) {
                    Usage["OutputTokens"] = *Value.Usage.OutputTokens;
                }
                if (Value.Usage.TotalTokens.has_value()) {
                    Usage["TotalTokens"] = *Value.Usage.TotalTokens;
                }
                Usage["Completeness"] =
                    UsageCompletenessName(Value.Usage.Completeness);
                Json::object Payload{
                    {"LimitKind",
                     Value.Kind == LimitKind::OutputTokens ? "OutputTokens"
                                                           : "ContextWindow"},
                    {"UsageSnapshot", std::move(Usage)}};
                if (Value.SafeGeneratedText.has_value()) {
                    Payload["SafePartialText"] = *Value.SafeGeneratedText;
                }
                return Payload;
            } else if constexpr (std::is_same_v<Type, ResponseFailedEvent>) {
                return Json::object{
                    {"ErrorCode", FailureCodeName(Value.Failure.Code)},
                    {"Retryable", Value.Failure.Retryable},
                    {"Description", Value.Failure.SanitizedDescription}};
            } else if constexpr (std::is_same_v<Type, ResponseCancelledEvent>) {
                return Json::object{{"ReasonCode", Value.Reason.Code.Value}};
            } else if constexpr (std::is_same_v<Type,
                                                ResponseOutcomeUnknownEvent>) {
                return Json::object{{"ReasonCode", Value.Reason.Code.Value}};
            }
            return {};
        },
        Event);
}

Json::object ToObserved(const CanonicalTerminalEvent& Event,
                        const CanonicalModelRequest& Request) {
    return std::visit(
        [&Request](const auto& Value) -> Json::object {
            return Json::object{
                {"RequestId", Request.Id.Value},
                {"AgentTurnId", Request.TurnId.Value},
                {"SequenceNumber", Value.Metadata.SequenceNumber},
                {"EventType", std::string(EventTypeName(Value.Type))},
                {"Payload", PayloadFromTerminal(CanonicalTerminalEvent{Value})}};
        },
        Event);
}

bool IsSubset(const Json::value& Expected, const Json::value& Actual) {
    if (!Expected.is_object()) {
        return Expected == Actual;
    }
    if (!Actual.is_object()) {
        return false;
    }
    const Json::object& ExpectedObject = Expected.as_object();
    const Json::object& ActualObject = Actual.as_object();
    for (const auto& Entry : ExpectedObject) {
        if (Entry.key() == "Description") {
            continue; // Fixture descriptions are synthetic placeholders.
        }
        const Json::value* ActualValue =
            ActualObject.if_contains(Entry.key());
        if (ActualValue == nullptr ||
            !IsSubset(Entry.value(), *ActualValue)) {
            return false;
        }
    }
    return true;
}

bool EventMatches(const Json::object& Expected, const Json::object& Actual) {
    if (!IsSubset(Json::value(Expected), Json::value(Actual))) {
        return false;
    }
    return true;
}

CanonicalMessage MessageFromFixture(const Json::object& Fixture) {
    CanonicalMessage Message;
    const std::string Role =
        std::string(Fixture.at("Role").as_string());
    Message.Role = Role == "assistant" ? MessageRole::Assistant
                   : Role == "tool"    ? MessageRole::Tool
                                       : MessageRole::User;
    if (const Json::array* Parts = ArrayField(Fixture, "ContentParts");
        Parts != nullptr) {
        for (const Json::value& Part : *Parts) {
            const Json::object& Object = Part.as_object();
            if (Object.at("Type").as_string() == "Text") {
                Message.ContentParts.push_back(
                    TextContentPart{.Text =
                                        std::string(Object.at("Text")
                                                        .as_string())});
            }
        }
    }
    if (const Json::value* Result = Fixture.if_contains("ToolCallResultId");
        Result != nullptr && Result->is_string()) {
        Message.ToolCallResultId =
            ToolCallId{std::string(Result->as_string())};
    }
    if (const Json::value* Calls = Fixture.if_contains("FunctionCalls");
        Calls != nullptr && Calls->is_array()) {
        for (const Json::value& Call : Calls->as_array()) {
            const Json::object& Object = Call.as_object();
            Message.FunctionCalls.push_back(FunctionCall{
                .Id = ToolCallId{
                    std::string(Object.at("Id").as_string())},
                .Name = std::string(Object.at("Name").as_string()),
                .Arguments = JsonValue{
                    Json::serialize(Object.at("Arguments"))}});
        }
    }
    return Message;
}

CanonicalModelRequest CanonicalFromFixture(const Json::object& Fixture) {
    CanonicalModelRequest Request;
    Request.Id = RequestId{std::string(Fixture.at("RequestId").as_string())};
    Request.TurnId =
        AgentTurnId{std::string(Fixture.at("AgentTurnId").as_string())};
    Request.SystemInstructions =
        std::string(Fixture.at("SystemInstructions").as_string());
    if (const Json::value* Developer =
            Fixture.if_contains("DeveloperInstructions");
        Developer != nullptr && Developer->is_string()) {
        Request.DeveloperInstructions =
            std::string(Developer->as_string());
    }
    for (const Json::value& Message : Fixture.at("Messages").as_array()) {
        Request.Messages.push_back(
            MessageFromFixture(Message.as_object()));
    }
    if (const Json::value* Tools = Fixture.if_contains("ToolDefinitions");
        Tools != nullptr && Tools->is_array()) {
        for (const Json::value& Tool : Tools->as_array()) {
            const Json::object& Object = Tool.as_object();
            Request.ToolDefinitions.push_back(ToolDefinition{
                .Name = std::string(Object.at("Name").as_string()),
                .Description =
                    std::string(Object.at("Description").as_string()),
                .Parameters = JsonSchema{
                    Json::serialize(Object.at("Parameters"))},
                .Strict = Object.at("Strict").as_bool()});
        }
    }
    Request.ModelName =
        std::string(Fixture.at("ModelName").as_string());
    if (const Json::value* Limit = Fixture.if_contains("OutputLimit");
        Limit != nullptr && Limit->is_object()) {
        const Json::value* Maximum =
            Limit->as_object().if_contains("MaxOutputTokens");
        if (Maximum != nullptr && Maximum->is_int64()) {
            Request.Limit.MaximumOutputTokens =
                static_cast<std::uint32_t>(Maximum->as_int64());
        }
    }
    return Request;
}

ModelProviderProfile MakeProfile() {
    ModelProviderProfile Profile;
    Profile.Id = ProviderProfileId{std::string{"profile-responses"}};
    Profile.Protocol = ProviderProtocol::OpenAIResponses;
    Profile.Endpoint = "https://api.openai.example/v1/responses";
    Profile.ModelName = "synthetic-model";
    Profile.Credential = CredentialRef{std::string{"credential-ref"}};
    Profile.Authentication = AuthenticationScheme::Bearer;
    Profile.ProtocolVersion = "v1";
    Profile.Timeouts.ConnectionTimeout = std::chrono::milliseconds(1000);
    Profile.Timeouts.RequestTimeout = std::chrono::milliseconds(5000);
    Profile.Capabilities = ProviderCapabilities{.Streaming = true,
                                                .ToolCalls = true,
                                                .Usage = true,
                                                .Cancellation = true,
                                                .OutputLimit = true};
    return Profile;
}

CanonicalMessage UserMessage(std::string Text) {
    CanonicalMessage Message;
    Message.Role = MessageRole::User;
    Message.ContentParts.push_back(
        TextContentPart{.Text = std::move(Text)});
    return Message;
}

std::string BuildSse(const Json::array& Events,
                     std::string_view LineEnding = "\n") {
    std::string Text;
    for (const Json::value& Event : Events) {
        Text.append("data: ");
        Text.append(Json::serialize(Event));
        Text.append(LineEnding);
        Text.append(LineEnding);
    }
    return Text;
}

std::vector<std::string> SplitEvery(std::string_view Text,
                                    std::size_t ChunkSize) {
    std::vector<std::string> Chunks;
    for (std::size_t Offset = 0; Offset < Text.size();
         Offset += ChunkSize) {
        Chunks.emplace_back(
            Text.substr(Offset, std::min(ChunkSize, Text.size() - Offset)));
    }
    return Chunks;
}

ObservedEvents RunStream(FakeHttpTransport& Transport,
                         const ModelProviderProfile& Profile,
                         const CanonicalModelRequest& Request,
                         std::stop_token Token = {},
                         bool StopAfterFirstEvent = false) {
    FixedCredentialLease Lease("synthetic-secret");
    OpenAIResponsesProvider Provider(
        Profile.Id, Transport,
        Net::EndpointGrantId{std::string{"grant-responses"}},
        Net::HttpsDestination{.Host = "api.openai.example",
                              .Port = 443,
                              .Path = "/v1/responses"});
    ObservedEvents Observed;
    std::stop_source LocalSource;
    const std::stop_token EffectiveToken =
        StopAfterFirstEvent ? LocalSource.get_token() : Token;
    const EventSink Sink = [&Observed, &Request, &LocalSource,
                            StopAfterFirstEvent](
                               const CanonicalNonTerminalEvent& Event) {
        Observed.Events.push_back(ToObserved(Event, Request));
        if (StopAfterFirstEvent) {
            LocalSource.request_stop();
        }
    };
    const CanonicalTerminalEvent Terminal =
        Provider.StreamTurn(Profile, Lease, Request, EffectiveToken, Sink);
    Observed.Events.push_back(ToObserved(Terminal, Request));
    return Observed;
}

void CompareExpected(const Json::array& Expected,
                     const ObservedEvents& Observed) {
    Expect(Observed.Events.size() == Expected.size());
    for (std::size_t Index = 0; Index < Expected.size(); ++Index) {
        const Json::object& ExpectedEvent = Expected[Index].as_object();
        if (!EventMatches(ExpectedEvent, Observed.Events[Index])) {
            std::fprintf(stderr,
                         "Fixture mismatch at event %zu: expected %s got %s\n",
                         Index,
                         Json::serialize(ExpectedEvent).c_str(),
                         Json::serialize(Observed.Events[Index]).c_str());
            Expect(false);
        }
    }
}

void TestCanonicalRequestWireMapping() {
    const Json::value Fixture = LoadFixture("CanonicalRequest.json");
    const Json::object& Root = Fixture.as_object();
    const Json::object& Case = Root.at("Cases").as_array().front().as_object();
    CanonicalModelRequest Request =
        CanonicalFromFixture(Case.at("CanonicalRequest").as_object());
    ModelProviderProfile Profile = MakeProfile();

    FakeHttpTransport Transport(FakeResponse{
        .StatusCode = 200,
        .Chunks = {},
        .Terminal = Net::FailedTransport{
            .Code = Net::TransportFailureCode::ConnectFailure}});
    FixedCredentialLease Lease("synthetic-secret");
    OpenAIResponsesProvider Provider(
        Profile.Id, Transport,
        Net::EndpointGrantId{std::string{"grant-responses"}},
        Net::HttpsDestination{.Host = "api.openai.example",
                              .Port = 443,
                              .Path = "/v1/responses"});
    static_cast<void>(Provider.StreamTurn(Profile, Lease, Request,
                                          std::stop_token{}, {}));

    const Json::value Actual =
        Json::parse(Transport.LastRequest.BodyUtf8);
    const Json::value& Expected = Case.at("ExpectedWireRequest");
    Expect(IsSubset(Expected, Actual));
    Expect(Transport.LastRequest.Destination.Host == "api.openai.example");
    bool HasAuthorization = false;
    for (const Net::TransportHeader& Header : Transport.LastRequest.Headers) {
        if (Header.Name == "Authorization") {
            HasAuthorization = Header.Value == "Bearer synthetic-secret";
        }
    }
    Expect(HasAuthorization);
    Expect(Transport.LastRequest.BodyUtf8.find("synthetic-secret") ==
           std::string::npos);
}

void TestStreamCases() {
    const Json::value Fixture = LoadFixture("StreamCases.json");
    const Json::object& Root = Fixture.as_object();
    for (const Json::value& CaseValue : Root.at("Cases").as_array()) {
        const Json::object& Case = CaseValue.as_object();
        const std::string CaseId =
            std::string(Case.at("CaseId").as_string());
        const std::string Sse = BuildSse(Case.at("WireEvents").as_array());
        FakeHttpTransport Transport(FakeResponse{
            .StatusCode = 200,
            .Chunks = {Sse},
            .Terminal = Net::CompletedTransport{
                .StatusCode = 200,
                .ReceivedBodyBytes = Sse.size()}});
        CanonicalModelRequest Request;
        Request.Id =
            RequestId{std::string(Case.at("RequestId").as_string())};
        Request.TurnId =
            AgentTurnId{std::string(Case.at("AgentTurnId").as_string())};
        Request.ModelName = "synthetic-model";
        Request.Messages.push_back(UserMessage("fixture input"));
        const ObservedEvents Observed =
            RunStream(Transport, MakeProfile(), Request);
        CompareExpected(Case.at("ExpectedCanonicalEvents").as_array(),
                        Observed);
        if (CaseId == "text_completed_with_usage") {
            for (const std::size_t ChunkSize : {std::size_t{1},
                                                std::size_t{7},
                                                Sse.size()}) {
                FakeHttpTransport Chunked(FakeResponse{
                    .StatusCode = 200,
                    .Chunks = SplitEvery(Sse, ChunkSize),
                    .Terminal = Net::CompletedTransport{
                        .StatusCode = 200,
                        .ReceivedBodyBytes = Sse.size()}});
                const ObservedEvents Variant =
                    RunStream(Chunked, MakeProfile(), Request);
                CompareExpected(
                    Case.at("ExpectedCanonicalEvents").as_array(), Variant);
                FakeHttpTransport Crlf(FakeResponse{
                    .StatusCode = 200,
                    .Chunks = {BuildSse(Case.at("WireEvents").as_array(),
                                        "\r\n")},
                    .Terminal = Net::CompletedTransport{
                        .StatusCode = 200, .ReceivedBodyBytes = 0}});
                CompareExpected(
                    Case.at("ExpectedCanonicalEvents").as_array(),
                    RunStream(Crlf, MakeProfile(), Request));
            }
        }
    }
}

void TestErrorCases() {
    const Json::value Fixture = LoadFixture("ErrorCases.json");
    const Json::object& Root = Fixture.as_object();
    int DeferredCases = 0;
    for (const Json::value& CaseValue : Root.at("Cases").as_array()) {
        const Json::object& Case = CaseValue.as_object();
        const std::string CaseId =
            std::string(Case.at("CaseId").as_string());
        CanonicalModelRequest Request;
        Request.Id =
            RequestId{std::string(Case.at("RequestId").as_string())};
        Request.TurnId =
            AgentTurnId{std::string(Case.at("AgentTurnId").as_string())};
        Request.ModelName = "synthetic-model";
        Request.Messages.push_back(UserMessage("fixture input"));

        if (CaseId == "background_cancel_confirmed") {
            // Explicitly contract-deferred, never silently skipped:
            // ModelProviderContract V1 exposes no background cancellation
            // endpoint, so the adapter cannot issue CancelHttpResponse.
            Expect(Case.if_contains("CancelHttpResponse") != nullptr);
            Expect(Case.if_contains("WireEvents") == nullptr);
            std::printf(
                "CONTRACT-DEFERRED: background_cancel_confirmed "
                "(ModelProviderContract V1 has no background cancellation "
                "endpoint)\n");
            ++DeferredCases;
            continue;
        }
        if (CaseId == "cancel_before_send") {
            FakeHttpTransport Transport(FakeResponse{});
            std::stop_source Source;
            Source.request_stop();
            const ObservedEvents Observed =
                RunStream(Transport, MakeProfile(), Request,
                          Source.get_token());
            CompareExpected(Case.at("ExpectedCanonicalEvents").as_array(),
                            Observed);
            Expect(Transport.CallCount == 0);
            continue;
        }
        if (CaseId == "cancel_after_send_unconfirmed") {
            const std::string Sse = BuildSse(
                Case.at("WireEventsBeforeCancellation").as_array());
            FakeHttpTransport Transport(FakeResponse{
                .StatusCode = 200,
                .Chunks = {Sse},
                .Terminal = Net::CancelledTransport{
                    .RequestMayHaveReachedEndpoint = true}});
            CompareExpected(Case.at("ExpectedCanonicalEvents").as_array(),
                            RunStream(Transport, MakeProfile(), Request));
            continue;
        }
        if (CaseId == "eof_after_partial_output") {
            const std::string Sse =
                BuildSse(Case.at("WireEventsBeforeEof").as_array());
            FakeHttpTransport Transport(FakeResponse{
                .StatusCode = 200,
                .Chunks = {Sse},
                .Terminal = Net::CompletedTransport{
                    .StatusCode = 200,
                    .ReceivedBodyBytes = Sse.size()}});
            CompareExpected(Case.at("ExpectedCanonicalEvents").as_array(),
                            RunStream(Transport, MakeProfile(), Request));
            continue;
        }
        if (const Json::value* Wire = Case.if_contains("Wire");
            Wire != nullptr && Wire->is_object() &&
            StringView(Wire->as_object(), "Kind") == "HttpResponse") {
            const Json::object& WireObject = Wire->as_object();
            const int Status =
                static_cast<int>(WireObject.at("StatusCode").as_int64());
            const std::string Body =
                Json::serialize(WireObject.at("Body"));
            FakeHttpTransport Transport(FakeResponse{
                .StatusCode = static_cast<std::uint16_t>(Status),
                .Chunks = {Body},
                .Terminal = Net::CompletedTransport{
                    .StatusCode = static_cast<std::uint16_t>(Status),
                    .ReceivedBodyBytes = Body.size()}});
            CompareExpected(Case.at("ExpectedCanonicalEvents").as_array(),
                            RunStream(Transport, MakeProfile(), Request));
            continue;
        }

        const Json::array& Wire = Case.at("WireEvents").as_array();
        const std::string Sse = BuildSse(Wire);
        FakeHttpTransport Transport(FakeResponse{
            .StatusCode = 200,
            .Chunks = {Sse},
            .Terminal = Net::CompletedTransport{
                .StatusCode = 200,
                .ReceivedBodyBytes = Sse.size()}});
        const ObservedEvents Observed =
            RunStream(Transport, MakeProfile(), Request);
        CompareExpected(Case.at("ExpectedCanonicalEvents").as_array(),
                        Observed);
        if (CaseId == "completed_without_usage") {
            for (const Json::object& Event : Observed.Events) {
                Expect(Event.at("EventType") != "UsageUpdated");
            }
        }
        if (CaseId == "invalid_tool_json") {
            for (const Json::object& Event : Observed.Events) {
                Expect(Event.at("EventType") != "ToolCallReady");
            }
            Expect(Observed.Events.back().at("EventType") == "ResponseFailed");
            const Json::object& Payload =
                Observed.Events.back().at("Payload").as_object();
            Expect(Payload.at("ErrorCode") == "InvalidStream");
        }
    }
    Expect(DeferredCases == 1);
}

void TestTransportTerminalMappings() {
    const auto RunTerminal =
        [](const Net::TransportTerminal& Terminal, int Status = 200,
           std::string Body = {}) {
            FakeHttpTransport Transport(FakeResponse{
                .StatusCode = Status,
                .Chunks = {std::move(Body)},
                .Terminal = Terminal});
            CanonicalModelRequest Request;
            Request.Id = RequestId{std::string{"terminal-request"}};
            Request.TurnId = AgentTurnId{std::string{"terminal-turn"}};
            Request.ModelName = "synthetic-model";
            Request.Messages.push_back(UserMessage("terminal input"));
            return RunStream(Transport, MakeProfile(), Request).Events.back();
        };

    const Json::object Tls = RunTerminal(Net::FailedTransport{
        .Code = Net::TransportFailureCode::TlsHandshakeFailure});
    Expect(Tls.at("EventType") == "ResponseFailed");
    Expect(Tls.at("Payload").as_object().at("ErrorCode") ==
           "TransportFailure");

    const Json::object ConnectTimeout = RunTerminal(Net::TimedOutTransport{
        .Phase = Net::TransportTimeoutPhase::Connect});
    Expect(ConnectTimeout.at("EventType") == "ResponseFailed");
    Expect(ConnectTimeout.at("Payload").as_object().at("ErrorCode") ==
           "TransportFailure");

    const Json::object ResponseTimeout = RunTerminal(Net::TimedOutTransport{
        .Phase = Net::TransportTimeoutPhase::Response});
    Expect(ResponseTimeout.at("EventType") == "ResponseOutcomeUnknown");

    const Json::object Redirect = RunTerminal(Net::RedirectDeniedTransport{
        .StatusCode = 307, .RedactedLocationHost = "other.example"});
    Expect(Redirect.at("EventType") == "ResponseFailed");
    Expect(Redirect.at("Payload").as_object().at("ErrorCode") ==
           "ProviderRejected");

    const Json::object Unknown = RunTerminal(Net::UnknownOutcomeTransport{
        .Reason = Net::UnknownOutcomeReason::StreamInterruptedAfterRequestSent,
        .RequestMayHaveReachedEndpoint = true});
    Expect(Unknown.at("EventType") == "ResponseOutcomeUnknown");

    const Json::object NotGranted = RunTerminal(Net::FailedTransport{
        .Code = Net::TransportFailureCode::EndpointNotGranted});
    Expect(NotGranted.at("EventType") == "ResponseFailed");
    Expect(NotGranted.at("Payload").as_object().at("ErrorCode") ==
           "InvalidProfile");

    const Json::object TransportCancelled =
        RunTerminal(Net::CancelledTransport{
            .RequestMayHaveReachedEndpoint = false});
    Expect(TransportCancelled.at("EventType") == "ResponseCancelled");
    Expect(TransportCancelled.at("Payload")
               .as_object()
               .at("ReasonCode") == "CancelledBeforeSend");

    const Json::object TransportCancelUnconfirmed =
        RunTerminal(Net::CancelledTransport{
            .RequestMayHaveReachedEndpoint = true});
    Expect(TransportCancelUnconfirmed.at("EventType") ==
           "ResponseOutcomeUnknown");

    const std::string ServerErrorBody =
        R"({"error":{"message":"Temporary upstream failure.","code":"server_error"}})";
    for (const int Status : {500, 502, 503, 504}) {
        const Json::object ServerError =
            RunTerminal(Net::CompletedTransport{
                            .StatusCode = static_cast<std::uint16_t>(Status),
                            .ReceivedBodyBytes = ServerErrorBody.size()},
                        Status, ServerErrorBody);
        Expect(ServerError.at("EventType") == "ResponseFailed");
        const Json::object& ServerPayload =
            ServerError.at("Payload").as_object();
        Expect(ServerPayload.at("ErrorCode") == "ProviderRejected");
        Expect(ServerPayload.at("Retryable").as_bool());
    }

    const std::string ClientErrorBody =
        R"({"error":{"message":"Bad request.","code":"invalid_request_error"}})";
    const Json::object ClientError =
        RunTerminal(Net::CompletedTransport{.StatusCode = 400,
                                            .ReceivedBodyBytes =
                                                ClientErrorBody.size()},
                    400, ClientErrorBody);
    Expect(ClientError.at("Payload").as_object().at("Retryable") == false);
}

void TestSseOverflowCannotBeMasked() {
    const Json::value Fixture = LoadFixture("StreamCases.json");
    const Json::object& Root = Fixture.as_object();
    const Json::object& TextCase =
        Root.at("Cases").as_array().front().as_object();
    const std::string ValidSse =
        BuildSse(TextCase.at("WireEvents").as_array());
    std::string Oversized = "data: ";
    Oversized.append(2U * 1024U * 1024U + 64U, 'x'); // no frame delimiter

    FakeHttpTransport Transport(FakeResponse{
        .StatusCode = 200,
        .Chunks = {Oversized, ValidSse},
        .Terminal = Net::CompletedTransport{
            .StatusCode = 200,
            .ReceivedBodyBytes = Oversized.size() + ValidSse.size()}});
    CanonicalModelRequest Request;
    Request.Id = RequestId{std::string{"overflow-request"}};
    Request.TurnId = AgentTurnId{std::string{"overflow-turn"}};
    Request.ModelName = "synthetic-model";
    Request.Messages.push_back(UserMessage("overflow input"));
    const ObservedEvents Observed =
        RunStream(Transport, MakeProfile(), Request);
    Expect(!Observed.Events.empty());
    for (const Json::object& Event : Observed.Events) {
        Expect(Event.at("EventType") != "ResponseCompleted");
    }
    const Json::object& Terminal = Observed.Events.back();
    Expect(Terminal.at("EventType") == "ResponseFailed");
    Expect(Terminal.at("Payload").as_object().at("ErrorCode") ==
           "InvalidStream");

    // A single Append containing a complete delimiter-terminated oversized
    // frame must also trip the bound; the following valid stream must not
    // produce ResponseCompleted.
    std::string CompleteOversized = "data: ";
    CompleteOversized.append(2U * 1024U * 1024U + 64U, 'y');
    CompleteOversized.append("\n\n");
    FakeHttpTransport CompleteFrameTransport(FakeResponse{
        .StatusCode = 200,
        .Chunks = {CompleteOversized + ValidSse},
        .Terminal = Net::CompletedTransport{
            .StatusCode = 200,
            .ReceivedBodyBytes = CompleteOversized.size() + ValidSse.size()}});
    const ObservedEvents CompleteFrameObserved =
        RunStream(CompleteFrameTransport, MakeProfile(), Request);
    for (const Json::object& Event : CompleteFrameObserved.Events) {
        Expect(Event.at("EventType") != "ResponseCompleted");
    }
    Expect(CompleteFrameObserved.Events.back().at("EventType") ==
           "ResponseFailed");
    Expect(CompleteFrameObserved.Events.back()
               .at("Payload")
               .as_object()
               .at("ErrorCode") == "InvalidStream");
}

void TestRefusalV1Point2Cases() {
    const Json::value Fixture =
        LoadFixture("Version1Point2/RefusalCases.json");
    const Json::object& Root = Fixture.as_object();
    for (const Json::value& CaseValue : Root.at("Cases").as_array()) {
        const Json::object& Case = CaseValue.as_object();
        const std::string Sse = BuildSse(Case.at("WireEvents").as_array());
        FakeHttpTransport Transport(FakeResponse{
            .StatusCode = 200,
            .Chunks = {Sse},
            .Terminal = Net::CompletedTransport{
                .StatusCode = 200,
                .ReceivedBodyBytes = Sse.size()}});
        CanonicalModelRequest Request;
        Request.Id =
            RequestId{std::string(Case.at("RequestId").as_string())};
        Request.TurnId =
            AgentTurnId{std::string(Case.at("AgentTurnId").as_string())};
        Request.ModelName = "synthetic-model";
        Request.Messages.push_back(UserMessage("fixture input"));
        CompareExpected(Case.at("ExpectedCanonicalEvents").as_array(),
                        RunStream(Transport, MakeProfile(), Request));
    }
}

void ProviderStreamTurnWithApiKey(OpenAIResponsesProvider& Provider,
                                  FakeHttpTransport& Transport,
                                  const ModelProviderProfile& Profile);

void TestProfileCapabilitiesAndAuthSchemes() {
    FakeHttpTransport Transport(FakeResponse{
        .StatusCode = 200,
        .Chunks = {},
        .Terminal = Net::FailedTransport{
            .Code = Net::TransportFailureCode::ConnectFailure}});
    ModelProviderProfile Profile = MakeProfile();
    OpenAIResponsesProvider Provider(
        Profile.Id, Transport,
        Net::EndpointGrantId{std::string{"grant-responses"}},
        Net::HttpsDestination{.Host = "api.openai.example",
                              .Port = 443,
                              .Path = "/v1/responses"});

    const auto Capabilities = Provider.GetCapabilities(Profile.Id);
    Expect(std::holds_alternative<ProviderCapabilities>(Capabilities));
    Expect(std::get<ProviderCapabilities>(Capabilities).Streaming);
    Expect(!std::get<ProviderCapabilities>(Capabilities).Vision);
    Expect(std::holds_alternative<ProviderError>(
        Provider.GetCapabilities(ProviderProfileId{std::string{"other"}})));
    Expect(std::holds_alternative<std::monostate>(
        Provider.ValidateProfile(Profile)));
    Profile.Protocol = ProviderProtocol::AnthropicMessages;
    Expect(std::holds_alternative<ProviderError>(
        Provider.ValidateProfile(Profile)));
    Profile = MakeProfile();
    Profile.Authentication = AuthenticationScheme::XApiKeyHeader;
    ProviderStreamTurnWithApiKey(Provider, Transport, Profile);
    Expect(std::holds_alternative<ProviderHealthStatus>(
        Provider.HealthCheck(MakeProfile())));
}

void ProviderStreamTurnWithApiKey(OpenAIResponsesProvider& Provider,
                                  FakeHttpTransport& Transport,
                                  const ModelProviderProfile& Profile) {
    CanonicalModelRequest Request;
    Request.Id = RequestId{std::string{"api-key-request"}};
    Request.TurnId = AgentTurnId{std::string{"api-key-turn"}};
    Request.ModelName = "synthetic-model";
    Request.Messages.push_back(UserMessage("hello"));
    FixedCredentialLease Lease("synthetic-secret");
    static_cast<void>(Provider.StreamTurn(Profile, Lease, Request,
                                          std::stop_token{}, {}));
    bool HasApiKey = false;
    for (const Net::TransportHeader& Header : Transport.LastRequest.Headers) {
        if (Header.Name == "api-key") {
            HasApiKey = Header.Value == "synthetic-secret";
        }
    }
    Expect(HasApiKey);
}

void TestUnsupportedInputsAndOptions() {
    FakeHttpTransport Transport(FakeResponse{});
    ModelProviderProfile Profile = MakeProfile();
    OpenAIResponsesProvider Provider(
        Profile.Id, Transport,
        Net::EndpointGrantId{std::string{"grant-responses"}},
        Net::HttpsDestination{.Host = "api.openai.example",
                              .Port = 443,
                              .Path = "/v1/responses"});
    FixedCredentialLease Lease("synthetic-secret");

    CanonicalModelRequest Request;
    Request.Id = RequestId{std::string{"unsupported-request"}};
    Request.TurnId = AgentTurnId{std::string{"unsupported-turn"}};
    Request.ModelName = "synthetic-model";
    CanonicalMessage ImageMessage;
    ImageMessage.Role = MessageRole::User;
    ImageMessage.ContentParts.push_back(
        ImageContentReference{.OpaqueReference = "opaque-image"});
    Request.Messages.push_back(std::move(ImageMessage));
    const CanonicalTerminalEvent Image = Provider.StreamTurn(
        Profile, Lease, Request, std::stop_token{}, {});
    Expect(std::holds_alternative<ResponseFailedEvent>(Image));
    Expect(std::get<ResponseFailedEvent>(Image).Failure.Code ==
           FailureCode::UnsupportedCapability);
    Expect(Transport.CallCount == 0);

    Request.Messages.clear();
    Request.Sampling.TopK = 5.0;
    const CanonicalTerminalEvent TopK = Provider.StreamTurn(
        Profile, Lease, Request, std::stop_token{}, {});
    Expect(std::holds_alternative<ResponseFailedEvent>(TopK));
    Expect(std::get<ResponseFailedEvent>(TopK).Failure.Code ==
           FailureCode::UnsupportedCapability);
}

} // namespace

int main() {
    TestCanonicalRequestWireMapping();
    TestStreamCases();
    TestErrorCases();
    TestTransportTerminalMappings();
    TestSseOverflowCannotBeMasked();
    TestRefusalV1Point2Cases();
    TestProfileCapabilitiesAndAuthSchemes();
    TestUnsupportedInputsAndOptions();
    return 0;
}
