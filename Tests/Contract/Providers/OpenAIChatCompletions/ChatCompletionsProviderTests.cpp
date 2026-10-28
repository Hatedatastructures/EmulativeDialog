#include "Source/Providers/OpenAIChatCompletions/ChatCompletionsProvider.hpp"
#include "Source/Platform/Network/HttpTransportPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace EmulativeDialog;
using namespace EmulativeDialog::Providers;
using namespace EmulativeDialog::Platform::Network;

int Failures = 0;

void Check(bool Condition, std::string_view Description) {
    if (!Condition) {
        ++Failures;
        std::fprintf(stderr, "FAILED: %.*s\n",
                     static_cast<int>(Description.size()), Description.data());
    }
}

class FakeCredentialLease final : public CredentialLease {
public:
    [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept
        override {
        return "fixture-lease-secret";
    }
};

class FakeHttpTransport final : public HttpTransportPort {
public:
    struct Script final {
        std::uint16_t StatusCode{200};
        std::vector<std::string> Chunks;
        TransportTerminal Terminal{CompletedTransport{200, 0}};
    };

    Script Next;
    std::vector<TransportRequest> Requests;
    std::size_t HealthCalls{0};
    TransportHealthState HealthState{TransportHealthState::Ready};

    TransportTerminal Stream(const TransportRequest& Request,
                             const ResponseHeadSink& OnHead,
                             const ResponseChunkSink& OnChunk,
                             std::stop_token) override {
        Requests.push_back(Request);
        if (Next.StatusCode != 0) {
            OnHead(ResponseHeadMetadata{Next.StatusCode, "text/event-stream",
                                        std::nullopt});
        }
        for (const std::string& Chunk : Next.Chunks) {
            OnChunk(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(Chunk.data()),
                Chunk.size()));
        }
        return Next.Terminal;
    }

    TransportHealthState Health(std::stop_token) noexcept override {
        ++HealthCalls;
        return HealthState;
    }
};

ModelProviderProfile MakeProfile(std::string Endpoint =
                                     "https://chat-fixture.example.invalid/"
                                     "v1/chat/completions") {
    ModelProviderProfile Profile;
    Profile.Id = ProviderProfileId{"fixture-chat-profile-v1"};
    Profile.Protocol = ProviderProtocol::OpenAICompatibleChatCompletions;
    Profile.Endpoint = std::move(Endpoint);
    Profile.ModelName = "fixture-chat-model";
    Profile.Credential = CredentialRef{"fixture-reference-not-a-credential"};
    Profile.Authentication = AuthenticationScheme::Bearer;
    Profile.ProtocolVersion = "chat-completions-v1";
    Profile.Capabilities = ProviderCapabilities{true, true, false, true, true,
                                                true};
    Profile.Limit.MaximumOutputTokens = 96;
    return Profile;
}

CanonicalModelRequest MakeRequest() {
    CanonicalModelRequest Request;
    Request.Id = RequestId{"request-fixture-001"};
    Request.TurnId = AgentTurnId{"turn-fixture-001"};
    Request.ModelName = "fixture-chat-model";
    Request.Limit.MaximumOutputTokens = 96;
    Request.Sampling.Temperature = 0.2;
    CanonicalMessage UserMessage;
    UserMessage.Role = MessageRole::User;
    UserMessage.ContentParts.emplace_back(TextContentPart{"hello"});
    Request.Messages.push_back(UserMessage);
    return Request;
}

std::string Data(const std::string& Payload) {
    return "data: " + Payload + "\n\n";
}

std::string Done() { return "data: [DONE]\n\n"; }

struct RunResult final {
    std::vector<CanonicalNonTerminalEvent> NonTerminal;
    CanonicalTerminalEvent Terminal;
};

RunResult Run(
    OpenAIChatCompletions::ChatCompletionsProvider& Provider,
    const ModelProviderProfile& Profile,
    const CanonicalModelRequest& Request,
    std::stop_token Token = {}) {
    RunResult Result;
    const EventSink Sink = [&Result](
                               const CanonicalNonTerminalEvent& Event) {
        Result.NonTerminal.push_back(Event);
    };
    FakeCredentialLease Lease;
    Result.Terminal = Provider.StreamTurn(Profile, Lease, Request, Token, Sink);
    return Result;
}

EventType TypeOf(const CanonicalNonTerminalEvent& Event) {
    return std::visit([](const auto& Value) { return Value.Type; }, Event);
}

ProviderEventMetadata MetadataOf(const CanonicalNonTerminalEvent& Event) {
    return std::visit([](const auto& Value) { return Value.Metadata; }, Event);
}

ProviderEventMetadata MetadataOf(const CanonicalTerminalEvent& Event) {
    return std::visit([](const auto& Value) { return Value.Metadata; }, Event);
}

EventType TypeOf(const CanonicalTerminalEvent& Event) {
    return std::visit([](const auto& Value) { return Value.Type; }, Event);
}

std::vector<EventType> TypesOf(const RunResult& Result) {
    std::vector<EventType> Types;
    for (const auto& Event : Result.NonTerminal) {
        Types.push_back(TypeOf(Event));
    }
    Types.push_back(TypeOf(Result.Terminal));
    return Types;
}

void CheckSequence(const RunResult& Result) {
    std::uint64_t Expected = 1;
    for (const auto& Event : Result.NonTerminal) {
        Check(MetadataOf(Event).SequenceNumber == Expected,
              "non-terminal sequence is monotonic");
        ++Expected;
    }
    Check(MetadataOf(Result.Terminal).SequenceNumber == Expected,
          "terminal sequence follows non-terminals");
}

OpenAIChatCompletions::ChatCompletionsProvider MakeProvider(
    FakeHttpTransport& Transport) {
    return OpenAIChatCompletions::ChatCompletionsProvider(
        Transport,
        [](const ModelProviderProfile&) {
            return EndpointGrantId{"fixture-grant"};
        });
}

void AddIncludeUsage(CanonicalModelRequest& Request) {
    Request.ProviderOptions.push_back(RegisteredProviderOption{
        RegisteredOptionId{"IncludeUsage"}, true});
}

void TestTextStopFragmentedSse() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_fixture_text","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_fixture_text","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Hello "},"finish_reason":null}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_fixture_text","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"你"},"finish_reason":null}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_fixture_text","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":null})");
    Stream += Done();

    // Split inside the UTF-8 code point for "你" (e4 bd a0).
    const auto Place = Stream.find("\xE4\xBD\xA0");
    Check(Place != std::string::npos, "fixture contains the split code point");
    Transport.Next.Chunks = {
        Stream.substr(0, Place + 1),
        Stream.substr(Place + 1, 1),
        Stream.substr(Place + 2),
    };
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};

    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);

    const std::vector<EventType> Expected{
        EventType::ResponseStarted, EventType::TextDelta,
        EventType::TextDelta, EventType::ResponseCompleted};
    Check(TypesOf(Result) == Expected, "text stop stream mapping");
    CheckSequence(Result);
    const auto* Text1 = std::get_if<TextDeltaEvent>(&Result.NonTerminal[1]);
    const auto* Text2 = std::get_if<TextDeltaEvent>(&Result.NonTerminal[2]);
    Check(Text1 != nullptr && Text1->Utf8Delta == "Hello ", "first delta");
    Check(Text2 != nullptr && Text2->Utf8Delta == "你",
          "split code point decoded after buffering");
    Check(Text1 != nullptr && Text2 != nullptr &&
              Text1->Item.Value == Text2->Item.Value,
          "text item id is stable");
    const auto* Completed = std::get_if<ResponseCompletedEvent>(&Result.Terminal);
    Check(Completed != nullptr &&
              Completed->Kind == CompletionKind::FinalAnswer,
          "stop maps to final answer");
    Check(Completed != nullptr && Completed->FinalUsage.has_value() &&
              Completed->FinalUsage->Completeness ==
                  UsageCompleteness::Unavailable,
          "missing usage stays unavailable");
    Check(Transport.Requests.size() == 1, "exactly one transport exchange");
    if (!Transport.Requests.empty()) {
        const std::string& Body = Transport.Requests.front().BodyUtf8;
        Check(Body.find("\"stream\":true") != std::string::npos,
              "request streams");
        Check(Body.find("\"n\":1") != std::string::npos, "request fixes n=1");
        Check(Body.find("\"max_completion_tokens\":96") != std::string::npos,
              "request maps output limit");
        Check(Body.find("request-fixture-001") == std::string::npos,
              "request id is local only");
        Check(Body.find("turn-fixture-001") == std::string::npos,
              "turn id is local only");
        Check(Body.find("fixture-lease-secret") == std::string::npos,
              "credential never enters the body");
        Check(Transport.Requests.front().Headers.size() == 3,
              "three wire headers");
        if (Transport.Requests.front().Headers.size() == 3) {
            Check(Transport.Requests.front().Headers[2].Name == "Authorization",
                  "authorization header present");
            Check(Transport.Requests.front().Headers[2].Value ==
                      "Bearer fixture-lease-secret",
                  "authorization uses the lease");
        }
    }
}

void TestInterleavedToolCallsWithUsage() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_fixture_tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant","tool_calls":[{"index":0,"id":"call_fixture_lookup_001","type":"function","function":{"name":"lookup_ticket_status","arguments":"{\"ticket_id\":\""}}]},"finish_reason":null}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_fixture_tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"tool_calls":[{"index":1,"function":{"arguments":"{\"style\":\""}}]},"finish_reason":null}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_fixture_tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"CASE-001\"}"}},{"index":1,"id":"call_fixture_format_002","type":"function","function":{"name":"format_summary","arguments":"brief\"}"}}]},"finish_reason":null}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_fixture_tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_fixture_tools","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":50,"completion_tokens":20,"total_tokens":70,"prompt_tokens_details":{"cached_tokens":4},"completion_tokens_details":{"reasoning_tokens":3}}})");
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};

    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    Profile.CompatibilityProfile = CompatibilityProfileId{
        "fixture-chat-extensions-v1"};
    CanonicalModelRequest Request = MakeRequest();
    AddIncludeUsage(Request);
    const RunResult Result = Run(Provider, Profile, Request);

    const std::vector<EventType> Expected{
        EventType::ResponseStarted,       EventType::ToolCallStarted,
        EventType::ToolArgumentDelta,     EventType::ToolArgumentDelta,
        EventType::ToolCallStarted,       EventType::ToolArgumentDelta,
        EventType::ToolArgumentDelta,     EventType::ToolCallReady,
        EventType::ToolCallReady,         EventType::UsageUpdated,
        EventType::ResponseCompleted};
    Check(TypesOf(Result) == Expected, "interleaved tool call mapping");
    CheckSequence(Result);
    const auto* Ready0 = std::get_if<ToolCallReadyEvent>(&Result.NonTerminal[7]);
    const auto* Ready1 = std::get_if<ToolCallReadyEvent>(&Result.NonTerminal[8]);
    Check(Ready0 != nullptr &&
              Ready0->Arguments.JsonText == R"({"ticket_id":"CASE-001"})",
          "first tool arguments complete");
    Check(Ready1 != nullptr && Ready1->Arguments.JsonText == R"({"style":"brief"})",
          "second tool arguments complete");
    const auto* Usage = std::get_if<UsageUpdatedEvent>(&Result.NonTerminal[9]);
    Check(Usage != nullptr && Usage->Usage.InputTokens.has_value() &&
              *Usage->Usage.InputTokens == 50,
          "usage input tokens mapped");
    Check(Usage != nullptr && Usage->Usage.CachedInputReadTokens.has_value() &&
              *Usage->Usage.CachedInputReadTokens == 4,
          "compatibility cached tokens mapped");
    Check(Usage != nullptr && Usage->Usage.ReasoningOutputTokens.has_value() &&
              *Usage->Usage.ReasoningOutputTokens == 3,
          "compatibility reasoning tokens mapped");
    const auto* Completed = std::get_if<ResponseCompletedEvent>(&Result.Terminal);
    Check(Completed != nullptr &&
              Completed->Kind == CompletionKind::ToolHandoff,
          "tool_calls maps to handoff");
}

void TestUsageWithoutCompatibilityProfileStaysUnknown() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_usage","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":null}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_usage","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":null})");
    Stream += Data(
        R"({"id":"chatcmpl_usage","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5,"prompt_tokens_details":{"cached_tokens":1}}})");
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};

    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    AddIncludeUsage(Request);
    const RunResult Result = Run(Provider, Profile, Request);
    Check(!Result.NonTerminal.empty() &&
              TypeOf(Result.NonTerminal.back()) == EventType::UsageUpdated,
          "usage trailer still emits usage updated");
    if (!Result.NonTerminal.empty()) {
        const auto* Usage =
            std::get_if<UsageUpdatedEvent>(&Result.NonTerminal.back());
        Check(Usage != nullptr &&
                  !Usage->Usage.CachedInputReadTokens.has_value(),
              "cached tokens require a compatibility profile");
    }
}

void TestFinishReasonMatrix() {
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"chatcmpl_length","choices":[{"index":0,"delta":{"content":"partial"},"finish_reason":null}]})");
        Stream += Data(
            R"({"id":"chatcmpl_length","choices":[{"index":0,"delta":{},"finish_reason":"length"}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseOutputLimited,
              "length maps to output limited");
        const auto* Limited =
            std::get_if<ResponseOutputLimitedEvent>(&Result.Terminal);
        Check(Limited != nullptr && Limited->Kind == LimitKind::OutputTokens,
              "output limit kind");
    }
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"chatcmpl_filter","choices":[{"index":0,"delta":{},"finish_reason":"content_filter"}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseFailed,
              "content filter fails");
        const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::ContentFiltered &&
                  !Failure->Failure.Retryable,
              "content filter failure code");
    }
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"chatcmpl_unknown","choices":[{"index":0,"delta":{},"finish_reason":"future_reason"}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidStream,
              "unknown finish reason fails closed");
    }
}

void TestEofBeforeFinish() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_fixture_disconnect","choices":[{"index":0,"delta":{"content":"partial answer"},"finish_reason":null}]})");
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    Check(TypeOf(Result.Terminal) == EventType::ResponseOutcomeUnknown,
          "early EOF is unknown outcome");
    for (const auto& Event : Result.NonTerminal) {
        Check(TypeOf(Event) != EventType::UsageUpdated,
              "no usage event on early EOF");
    }
}

void TestHttpErrorMapping() {
    struct Case final {
        std::uint16_t Status;
        FailureCode Expected;
        bool Retryable;
    };
    const Case Cases[] = {
        {401, FailureCode::AuthenticationFailure, false},
        {403, FailureCode::AuthenticationFailure, false},
        {404, FailureCode::ProviderRejected, false},
        {429, FailureCode::RateLimited, true},
        {500, FailureCode::TransportFailure, true},
        {503, FailureCode::TransportFailure, true},
    };
    for (const Case& Current : Cases) {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = Current.Status;
        Transport.Next.Chunks = {
            R"({"error":{"type":"provider_error","message":"fixture"}})"};
        Transport.Next.Terminal =
            CompletedTransport{Current.Status, 44};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(Result.NonTerminal.empty(), "HTTP errors emit no stream events");
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr && Failure->Failure.Code == Current.Expected &&
                  Failure->Failure.Retryable == Current.Retryable,
              "HTTP status mapping");
    }
}

void TestContextLengthMapping() {
    FakeHttpTransport Transport;
    Transport.Next.StatusCode = 400;
    Transport.Next.Chunks = {
        R"({"error":{"code":"context_length_exceeded","message":"fixture"}})"};
    Transport.Next.Terminal = CompletedTransport{400, 62};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    const auto* Limited = std::get_if<ResponseOutputLimitedEvent>(&Result.Terminal);
    Check(Limited != nullptr && Limited->Kind == LimitKind::ContextWindow,
          "context length maps to context window limit");
}

void TestCancelBeforeSend() {
    FakeHttpTransport Transport;
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    std::stop_source Source;
    Source.request_stop();
    const RunResult Result = Run(Provider, Profile, Request, Source.get_token());
    Check(Result.NonTerminal.empty(), "cancel before send emits no events");
    Check(TypeOf(Result.Terminal) == EventType::ResponseCancelled,
          "cancel before send terminal");
    Check(Transport.Requests.empty(), "cancel before send performs no request");
}

void TestMalformedSse() {
    FakeHttpTransport Transport;
    Transport.Next.Chunks = {Data(R"({"id":"chatcmpl_bad","choices":[")")};
    Transport.Next.Terminal = CompletedTransport{200, 32};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    const std::vector<EventType> Expected{EventType::ResponseStarted,
                                          EventType::ResponseFailed};
    Check(TypesOf(Result) == Expected, "malformed SSE mapping");
    const auto* Started =
        std::get_if<ResponseStartedEvent>(&Result.NonTerminal[0]);
    Check(Started != nullptr && Started->ProviderResponse.has_value() &&
              Started->ProviderResponse->Value.empty(),
          "malformed first frame starts without a provider id");
    const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
    Check(Failure != nullptr &&
              Failure->Failure.Code == FailureCode::InvalidStream,
          "malformed SSE fails closed");
}

void TestInvalidToolArguments() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_invalid_tool","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_bad","type":"function","function":{"name":"lookup","arguments":"{\"ticket_id\":"}}]},"finish_reason":"tool_calls"}]})");
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    for (const auto& Event : Result.NonTerminal) {
        Check(TypeOf(Event) != EventType::ToolCallReady,
              "invalid tool arguments never become ready");
    }
    const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
    Check(Failure != nullptr &&
              Failure->Failure.Code == FailureCode::InvalidStream,
          "invalid tool arguments fail closed");
}

void TestRefusalSemantics() {
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"chatcmpl_refusal","choices":[{"index":0,"delta":{"role":"assistant","refusal":"I cannot help."},"finish_reason":null}]})");
        Stream += Data(
            R"({"id":"chatcmpl_refusal","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const std::vector<EventType> Expected{
            EventType::ResponseStarted, EventType::RefusalDelta,
            EventType::ResponseCompleted};
        Check(TypesOf(Result) == Expected, "refusal mapping");
        const auto* Completed =
            std::get_if<ResponseCompletedEvent>(&Result.Terminal);
        Check(Completed != nullptr &&
                  Completed->Kind == CompletionKind::Refusal &&
                  !Completed->StopDetails.has_value(),
              "refusal completes without synthetic stop details");
    }
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"chatcmpl_mixed","choices":[{"index":0,"delta":{"refusal":""},"finish_reason":null}]})");
        Stream += Data(
            R"({"id":"chatcmpl_mixed","choices":[{"index":0,"delta":{"content":"text"},"finish_reason":null}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidStream,
              "empty refusal marker conflicts with later content");
    }
}

void TestVendorFieldRejected() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_vendor","object":"chat.completion.chunk","choices":[],"usage":null,"x_vendor_trace":"synthetic"})");
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
    Check(Failure != nullptr &&
              Failure->Failure.Code == FailureCode::UnsupportedCapability,
          "unregistered vendor field is rejected");
}

void TestLegacyFunctionCallRejected() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_legacy","choices":[{"index":0,"delta":{"function_call":{"name":"lookup","arguments":"{}"}},"finish_reason":"function_call"}]})");
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
    Check(Failure != nullptr &&
              Failure->Failure.Code == FailureCode::UnsupportedCapability,
          "legacy function_call rejected");
}

void TestRequestRejections() {
    {
        FakeHttpTransport Transport;
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        Request.ProviderOptions.push_back(
            RegisteredProviderOption{RegisteredOptionId{"N"},
                                     static_cast<std::int64_t>(2)});
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidRequest,
              "n>1 rejected");
        Check(Transport.Requests.empty(), "n>1 performs no network request");
    }
    {
        FakeHttpTransport Transport;
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        Request.ProviderOptions.push_back(RegisteredProviderOption{
            RegisteredOptionId{"Compatibility.vendor_mode"},
            std::string("fast")});
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidRequest,
              "unregistered extension rejected");
        Check(Transport.Requests.empty(),
              "unregistered extension performs no network request");
    }
}

void TestProfileValidation() {
    FakeHttpTransport Transport;
    auto Provider = MakeProvider(Transport);
    {
        ModelProviderProfile Profile = MakeProfile();
        Check(std::holds_alternative<std::monostate>(
                  Provider.ValidateProfile(Profile)),
              "valid profile accepted");
        const auto Capabilities =
            Provider.GetCapabilities(Profile.Id);
        Check(std::holds_alternative<ProviderCapabilities>(Capabilities),
              "capabilities available after validation");
    }
    {
        ModelProviderProfile Profile =
            MakeProfile("http://insecure.example.invalid/v1");
        const auto Result = Provider.ValidateProfile(Profile);
        const auto* Error = std::get_if<ProviderError>(&Result);
        Check(Error != nullptr &&
                  Error->Code == FailureCode::InvalidProfile,
              "non-HTTPS endpoint rejected");
    }
    {
        ModelProviderProfile Profile = MakeProfile();
        Profile.Authentication = AuthenticationScheme::XApiKeyHeader;
        const auto Result = Provider.ValidateProfile(Profile);
        const auto* Error = std::get_if<ProviderError>(&Result);
        Check(Error != nullptr &&
                  Error->Code == FailureCode::InvalidProfile,
              "non-Bearer authentication rejected");
    }
    {
        ModelProviderProfile Profile = MakeProfile();
        Profile.Sampling.TopK = 5.0;
        const auto Result = Provider.ValidateProfile(Profile);
        const auto* Error = std::get_if<ProviderError>(&Result);
        Check(Error != nullptr &&
                  Error->Code == FailureCode::UnsupportedCapability,
              "top_k rejected");
    }
    {
        const auto Result =
            Provider.GetCapabilities(ProviderProfileId{"unknown-profile"});
        const auto* Error = std::get_if<ProviderError>(&Result);
        Check(Error != nullptr &&
                  Error->Code == FailureCode::InvalidProfile,
              "unvalidated profile has no capabilities");
    }
}

void TestSseCommentCrlfAndSplitData() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += ": keep-alive comment\r\n\r\n";
    Stream +=
        "data: {\"id\":\"chatcmpl_comment\",\"choices\":[{\"index\":0,\"delta\":"
        "{\"content\":\"hi\"},\"finish_reason\":null}]}\r\n\r\n";
    Stream +=
        "data: {\"id\":\"chatcmpl_comment\",\"choices\":[\r\n";
    Stream += "data: {\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\r\n\r\n";
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    const std::vector<EventType> Expected{
        EventType::ResponseStarted, EventType::TextDelta,
        EventType::ResponseCompleted};
    Check(TypesOf(Result) == Expected,
          "comments, CRLF, and multi-line data are handled");
}

void TestDuplicateFinishAndDataAfterDone() {
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"chatcmpl_dup","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})");
        Stream += Data(
            R"({"id":"chatcmpl_dup","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidStream,
              "duplicate finish reason rejected");
    }
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"chatcmpl_done","choices":[{"index":0,"delta":{"content":"a"},"finish_reason":"stop"}]})");
        Stream += Done();
        Stream += Data(
            R"({"id":"chatcmpl_done","choices":[{"index":0,"delta":{"content":"b"},"finish_reason":"stop"}]})");
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidStream,
              "data after [DONE] rejected");
    }
}

void TestTransportFailureMapping() {
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 0;
        Transport.Next.Terminal = TimedOutTransport{
            TransportTimeoutPhase::Connect};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::TransportFailure &&
                  Failure->Failure.Retryable,
              "connect timeout maps to retryable transport failure");
    }
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 200;
        Transport.Next.Chunks = {
            Data(R"({"id":"chatcmpl_cut","choices":[{"index":0,"delta":{"content":"x"},"finish_reason":null}]})")};
        Transport.Next.Terminal =
            UnknownOutcomeTransport{UnknownOutcomeReason::
                                        StreamInterruptedAfterRequestSent,
                                    true};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseOutcomeUnknown,
              "unknown transport outcome");
    }
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 200;
        Transport.Next.Terminal = CancelledTransport{false};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseCancelled,
              "confirmed cancelled before send");
    }
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 200;
        Transport.Next.Terminal = CancelledTransport{true};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseOutcomeUnknown,
              "unconfirmed cancellation is unknown");
    }
}

void TestToolArgumentRequestRoundTripAndImages() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_request","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})");
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    Profile.Capabilities.Vision = true;
    Profile.CompatibilityProfile =
        CompatibilityProfileId{"fixture-chat-extensions-v1"};
    CanonicalModelRequest Request = MakeRequest();
    AddIncludeUsage(Request);
    Request.ProviderOptions.push_back(RegisteredProviderOption{
        RegisteredOptionId{"ServiceTier"}, std::string("default")});
    CanonicalMessage Assistant;
    Assistant.Role = MessageRole::Assistant;
    Assistant.FunctionCalls.push_back(
        FunctionCall{ToolCallId{"call_fixture_lookup_001"},
                     "lookup_ticket_status",
                     JsonValue{R"({"ticket_id":"CASE-001"})"}});
    Request.Messages.push_back(Assistant);
    CanonicalMessage Tool;
    Tool.Role = MessageRole::Tool;
    Tool.ToolCallResultId = ToolCallId{"call_fixture_lookup_001"};
    Tool.ContentParts.emplace_back(TextContentPart{R"({"state":"pending"})"});
    Request.Messages.push_back(Tool);
    Request.ToolDefinitions.push_back(ToolDefinition{
        "lookup_ticket_status", "fixture description",
        JsonSchema{
            R"({"type":"object","properties":{"ticket_id":{"type":"string"}},"required":["ticket_id"],"additionalProperties":false})"},
        false});
    const RunResult Result = Run(Provider, Profile, Request);
    Check(TypeOf(Result.Terminal) == EventType::ResponseCompleted,
          "request round trip completes");
    Check(Transport.Requests.size() == 1, "one request captured");
    if (!Transport.Requests.empty()) {
        const std::string& Body = Transport.Requests.front().BodyUtf8;
        Check(Body.find("\"tool_calls\"") != std::string::npos,
              "assistant tool calls serialized");
        Check(Body.find("\"tool_call_id\":\"call_fixture_lookup_001\"") !=
                  std::string::npos,
              "tool result correlation serialized");
        Check(Body.find("\"tools\"") != std::string::npos,
              "tool definitions serialized");
        Check(Body.find("\"include_usage\":true") != std::string::npos,
              "include usage option serialized");
        Check(Body.find("\"service_tier\":\"default\"") != std::string::npos,
              "registered compatibility field serialized");
        Check(Body.find("\"max_tokens\"") == std::string::npos,
              "deprecated max_tokens is not sent");
    }
}

void TestHealthCheck() {
    FakeHttpTransport Transport;
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    const auto Result = Provider.HealthCheck(Profile);
    Check(std::holds_alternative<ProviderHealthStatus>(Result),
          "health check returns a status");
    if (const auto* Status = std::get_if<ProviderHealthStatus>(&Result)) {
        Check(Status->State == ProviderHealthState::Available,
              "ready transport reports available");
    }
    Check(Transport.HealthCalls == 1, "health check consults the transport");
}

void TestNonStreamingSingleObject() {
    FakeHttpTransport Transport;
    std::string Stream;
    Stream += Data(
        R"({"id":"chatcmpl_single","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"single answer"},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})");
    Stream += Done();
    Transport.Next.Chunks = {Stream};
    Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    CanonicalModelRequest Request = MakeRequest();
    const RunResult Result = Run(Provider, Profile, Request);
    const std::vector<EventType> Expected{
        EventType::ResponseStarted, EventType::TextDelta,
        EventType::UsageUpdated, EventType::ResponseCompleted};
    Check(TypesOf(Result) == Expected, "single message object maps");
}

void TestProfileEdgeCases() {
    FakeHttpTransport Transport;
    auto Provider = MakeProvider(Transport);
    struct Case final {
        ModelProviderProfile Profile;
        FailureCode Expected;
    };
    std::vector<Case> Cases;
    {
        ModelProviderProfile Profile = MakeProfile();
        Profile.Protocol = ProviderProtocol::OpenAIResponses;
        Cases.push_back({Profile, FailureCode::InvalidProfile});
    }
    {
        ModelProviderProfile Profile = MakeProfile();
        Profile.ModelName.clear();
        Cases.push_back({Profile, FailureCode::InvalidProfile});
    }
    {
        ModelProviderProfile Profile = MakeProfile();
        Profile.ProtocolVersion.clear();
        Cases.push_back({Profile, FailureCode::InvalidProfile});
    }
    {
        ModelProviderProfile Profile = MakeProfile();
        Profile.Capabilities.Streaming = false;
        Cases.push_back({Profile, FailureCode::InvalidProfile});
    }
    {
        ModelProviderProfile Profile = MakeProfile("https://user@host/v1");
        Cases.push_back({Profile, FailureCode::InvalidProfile});
    }
    {
        ModelProviderProfile Profile = MakeProfile("https://host/v1#frag");
        Cases.push_back({Profile, FailureCode::InvalidProfile});
    }
    {
        ModelProviderProfile Profile = MakeProfile("https://host:bad/v1");
        Cases.push_back({Profile, FailureCode::InvalidProfile});
    }
    for (const Case& Current : Cases) {
        const auto Result = Provider.ValidateProfile(Current.Profile);
        const auto* Error = std::get_if<ProviderError>(&Result);
        Check(Error != nullptr && Error->Code == Current.Expected,
              "profile edge case rejected");
    }
    {
        ModelProviderProfile Profile = MakeProfile("https://[::1]:8443/v1");
        // A valid IPv6 literal endpoint must validate successfully.
        Profile.Id = ProviderProfileId{"fixture-ipv6"};
        const auto Result = Provider.ValidateProfile(Profile);
        Check(std::holds_alternative<std::monostate>(Result),
              "IPv6 literal endpoint validated");
    }
}

void TestRequestOptionEdgeCases() {
    FakeHttpTransport Transport;
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    const auto ExpectInvalid = [&Provider, &Transport, &Profile](
                                   CanonicalModelRequest Request,
                                   std::string_view Description) {
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidRequest,
              Description);
        Check(Transport.Requests.empty(),
              "invalid options perform no request");
    };
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.ProviderOptions.push_back(RegisteredProviderOption{
            RegisteredOptionId{"stream"}, false});
        ExpectInvalid(Request, "stream=false rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.ProviderOptions.push_back(RegisteredProviderOption{
            RegisteredOptionId{"include_usage"},
            static_cast<std::int64_t>(1)});
        ExpectInvalid(Request, "non-bool include_usage rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.ProviderOptions.push_back(RegisteredProviderOption{
            RegisteredOptionId{"tool_choice"}, static_cast<std::int64_t>(3)});
        ExpectInvalid(Request, "non-string tool_choice rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.ProviderOptions.push_back(RegisteredProviderOption{
            RegisteredOptionId{"ServiceTier"}, std::string("default")});
        ExpectInvalid(Request, "service_tier without compatibility rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.ProviderOptions.push_back(RegisteredProviderOption{
            RegisteredOptionId{"unknown_option"}, true});
        ExpectInvalid(Request, "unregistered option rejected");
    }
}

void TestSerializationRejections() {
    FakeHttpTransport Transport;
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    const auto ExpectInvalid = [&Provider, &Transport, &Profile](
                                   CanonicalModelRequest Request,
                                   std::string_view Description) {
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidRequest,
              Description);
        Check(Transport.Requests.empty(),
              "serialization rejection performs no request");
    };
    {
        CanonicalModelRequest Request = MakeRequest();
        CanonicalMessage Tool;
        Tool.Role = MessageRole::Tool;
        Tool.ContentParts.emplace_back(TextContentPart{"result"});
        Request.Messages.push_back(Tool);
        ExpectInvalid(Request, "tool message without tool_call_id rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        CanonicalMessage Assistant;
        Assistant.Role = MessageRole::Assistant;
        Assistant.FunctionCalls.push_back(
            FunctionCall{ToolCallId{"call"}, "lookup", JsonValue{"{"}});
        Request.Messages.push_back(Assistant);
        ExpectInvalid(Request, "invalid assistant tool arguments rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.ToolDefinitions.push_back(ToolDefinition{
            "lookup", "description", JsonSchema{"{"}, false});
        ExpectInvalid(Request, "invalid tool schema rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.Messages[0].ContentParts.emplace_back(
            ImageContentReference{"opaque-image"});
        ExpectInvalid(Request, "image without vision capability rejected");
    }
}

void TestStreamProtocolErrors() {
    const auto RunStream = [](std::string Stream,
                              ModelProviderProfile Profile = MakeProfile()) {
        FakeHttpTransport Transport;
        Transport.Next.Chunks = {std::move(Stream)};
        Transport.Next.Terminal = CompletedTransport{200, 16};
        auto Provider = MakeProvider(Transport);
        CanonicalModelRequest Request = MakeRequest();
        return std::make_pair(Run(Provider, Profile, Request),
                              Transport.Requests.size());
    };
    struct Case final {
        std::string Stream;
        FailureCode Expected;
    };
    const Case Cases[] = {
        {Data(R"([])"), FailureCode::InvalidStream},
        {Data(R"({"id":"x"})"), FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":{}})"), FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[1]})"), FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":1,"delta":{},"finish_reason":"stop"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[],"usage":"bad"})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"role":"user"},"finish_reason":"stop"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"role":1},"finish_reason":"stop"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"content":1},"finish_reason":"stop"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"refusal":1},"finish_reason":"stop"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":{}},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[1]},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"id":"a","type":"function","function":{"name":"n","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"type":"other","function":{"name":"n","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::UnsupportedCapability},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","type":1,"function":{"name":"n","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","function":{}}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"n","arguments":1}}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
    };
    for (const Case& Current : Cases) {
        const auto [Result, Requests] = RunStream(Current.Stream);
        static_cast<void>(Requests);
        const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr && Failure->Failure.Code == Current.Expected,
              "stream protocol error mapping");
    }
}

void TestToolIdentityConflicts() {
    const auto RunStream = [](std::string Stream) {
        FakeHttpTransport Transport;
        Transport.Next.Chunks = {std::move(Stream)};
        Transport.Next.Terminal = CompletedTransport{200, 16};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        return Run(Provider, Profile, Request);
    };
    {
        std::string Stream;
        Stream += Data(
            R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"n","arguments":"{}"}}]},"finish_reason":null}]})");
        Stream += Data(
            R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"b"}]},"finish_reason":"tool_calls"}]})");
        const RunResult Result = RunStream(Stream);
        const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidStream,
              "tool id change rejected");
    }
    {
        std::string Stream;
        Stream += Data(
            R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"n","arguments":"{}"}}]},"finish_reason":null}]})");
        Stream += Data(
            R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"name":"m"}}]},"finish_reason":"tool_calls"}]})");
        const RunResult Result = RunStream(Stream);
        const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == FailureCode::InvalidStream,
              "tool name change rejected");
    }
}

void TestTransportFailureTable() {
    struct Case final {
        TransportFailureCode Code;
        EventType Expected;
    };
    const Case Cases[] = {
        {TransportFailureCode::InvalidRequest, EventType::ResponseFailed},
        {TransportFailureCode::EndpointNotGranted, EventType::ResponseFailed},
        {TransportFailureCode::DnsFailure, EventType::ResponseFailed},
        {TransportFailureCode::ConnectFailure, EventType::ResponseFailed},
        {TransportFailureCode::TlsHandshakeFailure, EventType::ResponseFailed},
        {TransportFailureCode::ProxyAuthenticationRequired,
         EventType::ResponseFailed},
        {TransportFailureCode::RequestWriteFailure,
         EventType::ResponseOutcomeUnknown},
        {TransportFailureCode::ResponseReadFailure,
         EventType::ResponseOutcomeUnknown},
        {TransportFailureCode::ResponseLimitExceeded,
         EventType::ResponseFailed},
        {TransportFailureCode::HeaderLimitExceeded, EventType::ResponseFailed},
        {TransportFailureCode::InvalidHttpResponse, EventType::ResponseFailed},
        {TransportFailureCode::InternalError, EventType::ResponseFailed},
    };
    for (const Case& Current : Cases) {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 0;
        Transport.Next.Terminal = FailedTransport{Current.Code};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == Current.Expected,
              "transport failure mapping");
    }
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 0;
        Transport.Next.Terminal = RedirectDeniedTransport{302, "elsewhere"};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseFailed,
              "redirect denied mapping");
    }
}

void TestAdditionalStreamShapes() {
    {
        FakeHttpTransport Transport;
        Transport.Next.Chunks = {Data(R"({"id":"x","choices":[],"usage":null})") +
                                 Done()};
        Transport.Next.Terminal = CompletedTransport{200, 16};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseOutcomeUnknown,
              "usage-only stream without finish is unknown");
    }
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"x","choices":[{"index":0,"delta":{"content":""},"finish_reason":null}]})");
        Stream += Data(
            R"({"id":"x","choices":[{"index":0,"delta":{"content":null},"finish_reason":"stop"}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, 16};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const std::vector<EventType> Expected{
            EventType::ResponseStarted, EventType::ResponseCompleted};
        Check(TypesOf(Result) == Expected, "empty and null content emit no delta");
    }
    {
        FakeHttpTransport Transport;
        std::string Stream;
        Stream += Data(
            R"({"id":"x","choices":[{"index":0,"delta":{"refusal":null},"finish_reason":"stop"}]})");
        Stream += Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, 16};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Completed =
            std::get_if<ResponseCompletedEvent>(&Result.Terminal);
        Check(Completed != nullptr &&
                  Completed->Kind == CompletionKind::FinalAnswer,
              "null refusal remains a final answer");
    }
}

void TestHealthUnavailable() {
    FakeHttpTransport Transport;
    Transport.HealthState = TransportHealthState::Unavailable;
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    const auto Result = Provider.HealthCheck(Profile);
    const auto* Status = std::get_if<ProviderHealthStatus>(&Result);
    Check(Status != nullptr &&
              Status->State == ProviderHealthState::Unavailable,
          "unavailable transport reports unavailable");
}

} // namespace

int main() {
    TestTextStopFragmentedSse();
    TestInterleavedToolCallsWithUsage();
    TestUsageWithoutCompatibilityProfileStaysUnknown();
    TestFinishReasonMatrix();
    TestEofBeforeFinish();
    TestHttpErrorMapping();
    TestContextLengthMapping();
    TestCancelBeforeSend();
    TestMalformedSse();
    TestInvalidToolArguments();
    TestRefusalSemantics();
    TestVendorFieldRejected();
    TestLegacyFunctionCallRejected();
    TestRequestRejections();
    TestProfileValidation();
    TestSseCommentCrlfAndSplitData();
    TestDuplicateFinishAndDataAfterDone();
    TestTransportFailureMapping();
    TestToolArgumentRequestRoundTripAndImages();
    TestHealthCheck();
    TestNonStreamingSingleObject();
    TestProfileEdgeCases();
    TestRequestOptionEdgeCases();
    TestSerializationRejections();
    TestStreamProtocolErrors();
    TestToolIdentityConflicts();
    TestTransportFailureTable();
    TestAdditionalStreamShapes();
    TestHealthUnavailable();

    if (Failures != 0) {
        std::fprintf(stderr, "%d assertion(s) failed\n", Failures);
        return 1;
    }
    std::puts("ChatCompletionsProvider tests passed");
    return 0;
}
