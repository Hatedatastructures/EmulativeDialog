#include "Source/Providers/OpenAIChatCompletions/ChatCompletionsProvider.hpp"
#include "Source/Platform/Network/HttpTransportPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
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

namespace json = boost::json;

using namespace EmulativeDialog;
using namespace EmulativeDialog::Providers;
using namespace EmulativeDialog::Platform::Network;

int Failures = 0;
std::string CurrentFixtureCase;
// Fixture labels express stable identity classes; adapter ItemId values are opaque.
std::map<std::string, std::string> FixtureToAdapterItemIds;
std::map<std::string, std::string> AdapterToFixtureItemIds;

void Check(bool Condition, std::string_view Description) {
    if (!Condition) {
        ++Failures;
        if (!CurrentFixtureCase.empty()) {
            std::fprintf(stderr, "FAILED [%s]: %.*s\n",
                         CurrentFixtureCase.c_str(),
                         static_cast<int>(Description.size()),
                         Description.data());
        } else {
            std::fprintf(stderr, "FAILED: %.*s\n",
                         static_cast<int>(Description.size()),
                         Description.data());
        }
    }
}

class FakeCredentialLease final : public CredentialLease {
public:
    [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept
        override {
        return "fixture-lease-secret";
    }
};

class ValidEndpointGrantResolver final : public EndpointGrantResolver {
public:
    [[nodiscard]] EndpointGrantStatus Validate(
        const EndpointGrantId&,
        const HttpsDestination&) const noexcept override {
        return EndpointGrantStatus::Valid;
    }
};

EndpointGrantResolver& DefaultEndpointGrantResolver() {
    static ValidEndpointGrantResolver Resolver;
    return Resolver;
}

class FakeHttpTransport final : public HttpTransportPort {
public:
    FakeHttpTransport() : HttpTransportPort(DefaultEndpointGrantResolver()) {}

    struct Script final {
        std::uint16_t StatusCode{200};
        std::vector<std::string> Chunks;
        TransportTerminal Terminal{CompletedTransport{200, 0}};
    };

    Script Next;
    std::vector<TransportRequest> Requests;
    std::size_t HealthCalls{0};
    TransportHealthState HealthState{TransportHealthState::Ready};

protected:
    TransportTerminal DoStream(const TransportRequest& Request,
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

public:
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

std::string Data(std::string_view Payload) {
    std::string Frame = "data: ";
    Frame.append(Payload);
    Frame += "\n\n";
    return Frame;
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

const json::value* Field(const json::object& Object, std::string_view Name) {
    return Object.if_contains(Name);
}

std::string StringField(const json::object& Object, std::string_view Name,
                        std::string Default = {}) {
    const json::value* Value = Field(Object, Name);
    if (Value == nullptr || !Value->is_string()) {
        return Default;
    }
    const json::string& Text = Value->as_string();
    return std::string(Text.data(), Text.size());
}

std::optional<std::uint64_t> UnsignedValue(const json::value* Value) {
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

std::optional<std::uint64_t> UnsignedFieldValue(const json::object& Object,
                                               std::string_view Name) {
    return UnsignedValue(Field(Object, Name));
}

const json::object* ObjectField(const json::object& Object,
                                std::string_view Name) {
    const json::value* Value = Field(Object, Name);
    return Value == nullptr ? nullptr : Value->if_object();
}

const json::array* ArrayField(const json::object& Object,
                              std::string_view Name) {
    const json::value* Value = Field(Object, Name);
    return Value == nullptr ? nullptr : Value->if_array();
}

std::optional<json::value> LoadFixture(std::string_view RelativePath) {
#ifndef ED_CHAT_COMPLETIONS_FIXTURE_ROOT
#error ED_CHAT_COMPLETIONS_FIXTURE_ROOT must be defined by the test target
#endif
    const std::filesystem::path Path =
        std::filesystem::path(ED_CHAT_COMPLETIONS_FIXTURE_ROOT) /
        std::string(RelativePath);
    std::ifstream Input(Path, std::ios::binary);
    if (!Input) {
        Check(false, std::string("fixture file opens: ") + Path.string());
        return std::nullopt;
    }
    const std::string Text((std::istreambuf_iterator<char>(Input)),
                           std::istreambuf_iterator<char>());
    boost::system::error_code Error;
    json::value Value = json::parse(Text, Error);
    if (Error) {
        Check(false, std::string("fixture parses: ") + Path.string());
        return std::nullopt;
    }
    return Value;
}

void AppendContentParts(const json::array* Parts,
                        std::vector<ContentPart>& Destination) {
    if (Parts == nullptr) {
        return;
    }
    for (const json::value& PartValue : *Parts) {
        const json::object* Part = PartValue.if_object();
        if (Part == nullptr) {
            Check(false, "fixture content part is an object");
            continue;
        }
        const std::string Type = StringField(*Part, "Type");
        if (Type == "Text") {
            Destination.emplace_back(TextContentPart{
                StringField(*Part, "Text")});
        } else if (Type == "Image" || Type == "ImageReference") {
            Destination.emplace_back(ImageContentReference{
                StringField(*Part, "OpaqueReference",
                            StringField(*Part, "Reference"))});
        } else {
            Check(false, "fixture content part type is supported by the test builder");
        }
    }
}

std::optional<MessageRole> ParseMessageRole(std::string_view Role) {
    if (Role == "system") {
        return MessageRole::System;
    }
    if (Role == "developer") {
        return MessageRole::Developer;
    }
    if (Role == "user") {
        return MessageRole::User;
    }
    if (Role == "assistant") {
        return MessageRole::Assistant;
    }
    if (Role == "tool") {
        return MessageRole::Tool;
    }
    return std::nullopt;
}

void AppendProviderOption(const json::value& Value, std::string Id,
                          CanonicalModelRequest& Request) {
    RegisteredOptionValue OptionValue;
    if (Value.is_bool()) {
        OptionValue = Value.as_bool();
    } else if (Value.is_string()) {
        const json::string& Text = Value.as_string();
        OptionValue = std::string(Text.data(), Text.size());
    } else if (Value.is_int64()) {
        OptionValue = Value.as_int64();
    } else if (Value.is_uint64() &&
               Value.as_uint64() <= static_cast<std::uint64_t>(
                                        std::numeric_limits<std::int64_t>::max())) {
        OptionValue = static_cast<std::int64_t>(Value.as_uint64());
    } else if (Value.is_double()) {
        OptionValue = Value.as_double();
    } else {
        OptionValue = std::string{"invalid-fixture-option-type"};
    }
    Request.ProviderOptions.push_back(RegisteredProviderOption{
        RegisteredOptionId{std::move(Id)}, std::move(OptionValue)});
}

CanonicalModelRequest BuildRequest(const json::object& FixtureRequest) {
    CanonicalModelRequest Request;
    Request.Id = RequestId{
        StringField(FixtureRequest, "RequestId", "request-fixture-001")};
    Request.TurnId = AgentTurnId{
        StringField(FixtureRequest, "AgentTurnId", "turn-fixture-001")};
    Request.ModelName = StringField(FixtureRequest, "ModelName");
    if (const auto Limit = UnsignedFieldValue(FixtureRequest, "OutputLimit");
        Limit.has_value() && *Limit <= std::numeric_limits<std::uint32_t>::max()) {
        Request.Limit.MaximumOutputTokens = static_cast<std::uint32_t>(*Limit);
    }
    if (const json::object* Sampling =
            ObjectField(FixtureRequest, "SamplingOptions");
        Sampling != nullptr) {
        if (const json::value* Temperature = Field(*Sampling, "Temperature");
            Temperature != nullptr && Temperature->is_number()) {
            Request.Sampling.Temperature =
                Temperature->is_double()
                    ? Temperature->as_double()
                    : static_cast<double>(Temperature->as_int64());
        }
        if (const json::value* TopP = Field(*Sampling, "TopP");
            TopP != nullptr && TopP->is_number()) {
            Request.Sampling.TopP = TopP->is_double()
                                        ? TopP->as_double()
                                        : static_cast<double>(TopP->as_int64());
        }
    }
    if (const json::array* Messages = ArrayField(FixtureRequest, "Messages");
        Messages != nullptr) {
        for (const json::value& MessageValue : *Messages) {
            const json::object* MessageObject = MessageValue.if_object();
            if (MessageObject == nullptr) {
                Check(false, "fixture message is an object");
                continue;
            }
            CanonicalMessage Message;
            const auto Role = ParseMessageRole(
                StringField(*MessageObject, "Role"));
            if (!Role.has_value()) {
                Check(false, "fixture message role is known");
                continue;
            }
            Message.Role = *Role;
            AppendContentParts(ArrayField(*MessageObject, "ContentParts"),
                               Message.ContentParts);
            if (const json::array* Calls =
                    ArrayField(*MessageObject, "ToolCalls");
                Calls != nullptr) {
                for (const json::value& CallValue : *Calls) {
                    const json::object* Call = CallValue.if_object();
                    if (Call == nullptr) {
                        Check(false, "fixture tool call is an object");
                        continue;
                    }
                    Message.FunctionCalls.push_back(FunctionCall{
                        ToolCallId{StringField(*Call, "ToolCallId")},
                        StringField(*Call, "ToolName"),
                        JsonValue{StringField(*Call, "ArgumentsJson")}});
                }
            }
            if (const json::value* CallId =
                    Field(*MessageObject, "ToolCallId");
                CallId != nullptr && CallId->is_string()) {
                Message.ToolCallResultId =
                    ToolCallId{StringField(*MessageObject, "ToolCallId")};
            }
            Request.Messages.push_back(std::move(Message));
        }
    }
    AppendContentParts(ArrayField(FixtureRequest, "ContentParts"),
                       Request.ContentParts);
    if (const json::array* Definitions =
            ArrayField(FixtureRequest, "ToolDefinitions");
        Definitions != nullptr) {
        for (const json::value& DefinitionValue : *Definitions) {
            const json::object* Definition = DefinitionValue.if_object();
            if (Definition == nullptr) {
                Check(false, "fixture tool definition is an object");
                continue;
            }
            const json::value* Parameters = Field(*Definition, "Parameters");
            Request.ToolDefinitions.push_back(ToolDefinition{
                StringField(*Definition, "Name"),
                StringField(*Definition, "Description"),
                JsonSchema{Parameters == nullptr
                               ? std::string{"{}"}
                               : json::serialize(*Parameters)},
                false});
        }
    }
    if (const json::object* Options =
            ObjectField(FixtureRequest, "ProviderOptions");
        Options != nullptr) {
        for (const auto& Member : *Options) {
            const std::string Name(Member.key().data(), Member.key().size());
            const json::value& Value = Member.value();
            if (Name != "Compatibility") {
                AppendProviderOption(Value, Name, Request);
                continue;
            }
            const json::object* Compatibility = Value.if_object();
            if (Compatibility == nullptr) {
                AppendProviderOption(Value, "Compatibility.invalid", Request);
                continue;
            }
            for (const auto& CompatibilityMember : *Compatibility) {
                const std::string CompatibilityName(
                    CompatibilityMember.key().data(),
                    CompatibilityMember.key().size());
                if (CompatibilityName == "ProfileId") {
                    continue;
                }
                AppendProviderOption(
                    CompatibilityMember.value(),
                    "Compatibility." + CompatibilityName, Request);
            }
        }
    }
    return Request;
}

ModelProviderProfile BuildProfile(const json::object* FixtureProfile) {
    ModelProviderProfile Profile = MakeProfile();
    if (FixtureProfile == nullptr) {
        return Profile;
    }
    Profile.Id = ProviderProfileId{
        StringField(*FixtureProfile, "ProviderProfileId", Profile.Id.Value)};
    Profile.Endpoint =
        StringField(*FixtureProfile, "Endpoint", Profile.Endpoint);
    Profile.ModelName =
        StringField(*FixtureProfile, "ModelName", Profile.ModelName);
    Profile.ProtocolVersion = StringField(*FixtureProfile, "ProtocolVersion",
                                          Profile.ProtocolVersion);
    if (const json::value* Compatibility =
            Field(*FixtureProfile, "CompatibilityProfileId");
        Compatibility != nullptr && Compatibility->is_string()) {
        Profile.CompatibilityProfile = CompatibilityProfileId{
            StringField(*FixtureProfile, "CompatibilityProfileId")};
    }
    if (const json::array* Capabilities =
            ArrayField(*FixtureProfile, "Capabilities");
        Capabilities != nullptr) {
        Profile.Capabilities = ProviderCapabilities{};
        for (const json::value& CapabilityValue : *Capabilities) {
            if (!CapabilityValue.is_string()) {
                continue;
            }
            const json::string& CapabilityText = CapabilityValue.as_string();
            const std::string_view Capability(CapabilityText.data(),
                                              CapabilityText.size());
            if (Capability == "Streaming") {
                Profile.Capabilities.Streaming = true;
            } else if (Capability == "ToolCalls") {
                Profile.Capabilities.ToolCalls = true;
            } else if (Capability == "Vision") {
                Profile.Capabilities.Vision = true;
            } else if (Capability == "Usage") {
                Profile.Capabilities.Usage = true;
            } else if (Capability == "Cancellation") {
                Profile.Capabilities.Cancellation = true;
            } else if (Capability == "OutputLimit") {
                Profile.Capabilities.OutputLimit = true;
            }
        }
    }
    return Profile;
}

std::optional<std::size_t> CaseIndexFromPointer(std::string_view Pointer) {
    constexpr std::string_view Prefix = "/Cases/";
    if (!Pointer.starts_with(Prefix)) {
        return std::nullopt;
    }
    const std::string_view Number = Pointer.substr(Prefix.size());
    std::size_t Index = 0;
    const auto Parsed =
        std::from_chars(Number.data(), Number.data() + Number.size(), Index);
    if (Parsed.ec != std::errc{} || Parsed.ptr != Number.data() + Number.size()) {
        return std::nullopt;
    }
    return Index;
}

std::optional<std::string> DecodeHex(std::string_view Hex) {
    if (Hex.size() % 2U != 0U) {
        return std::nullopt;
    }
    std::string Bytes;
    Bytes.reserve(Hex.size() / 2U);
    for (std::size_t Index = 0; Index < Hex.size(); Index += 2U) {
        unsigned int Value = 0;
        const auto Parsed = std::from_chars(
            Hex.data() + Index, Hex.data() + Index + 2U, Value, 16);
        if (Parsed.ec != std::errc{} || Parsed.ptr != Hex.data() + Index + 2U ||
            Value > 0xFFU) {
            return std::nullopt;
        }
        Bytes.push_back(static_cast<char>(Value));
    }
    return Bytes;
}

std::string FrameToSse(const json::value& Frame) {
    return Data(json::serialize(Frame));
}

std::vector<std::string> ChunksFromWireFrames(
    const json::array& Frames, const json::object* FixtureCase = nullptr) {
    if (FixtureCase != nullptr) {
        if (const json::object* Split = ObjectField(*FixtureCase, "TransportSplit");
            Split != nullptr) {
            if (const json::array* HexChunks =
                    ArrayField(*Split, "TransportChunksUtf8Hex");
                HexChunks != nullptr) {
                std::vector<std::string> Chunks;
                for (const json::value& HexValue : *HexChunks) {
                    if (!HexValue.is_string()) {
                        Check(false, "fixture transport chunk hex is a string");
                        continue;
                    }
                    const json::string& HexText = HexValue.as_string();
                    const auto Bytes = DecodeHex(std::string_view(
                        HexText.data(), HexText.size()));
                    Check(Bytes.has_value(), "fixture transport chunk hex decodes");
                    if (Bytes.has_value()) {
                        Chunks.push_back(*Bytes);
                    }
                }
                return Chunks;
            }
        }
    }
    std::string Stream;
    for (const json::value& FrameValue : Frames) {
        const json::object* Frame = FrameValue.if_object();
        if (Frame == nullptr) {
            Check(false, "fixture wire frame is an object");
            continue;
        }
        const std::string Kind = StringField(*Frame, "Kind");
        if (Kind == "Done") {
            Stream += Done();
        } else {
            const json::value* DataValue = Field(*Frame, "Data");
            if (DataValue == nullptr) {
                Check(false, "fixture data frame has a Data field");
                continue;
            }
            Stream += FrameToSse(*DataValue);
        }
    }
    return {std::move(Stream)};
}

void AppendJsonFrames(const json::array& Frames, std::string& Stream) {
    for (const json::value& Frame : Frames) {
        Stream += FrameToSse(Frame);
    }
}

void ApplyRequestSettings(const json::object& FixtureCase,
                          CanonicalModelRequest& Request) {
    const json::object* Settings = ObjectField(FixtureCase, "RequestSettings");
    if (Settings == nullptr) {
        return;
    }
    if (const json::value* IncludeUsage = Field(*Settings, "include_usage");
        IncludeUsage != nullptr && IncludeUsage->is_bool() &&
        IncludeUsage->as_bool()) {
        AddIncludeUsage(Request);
    }
}

bool ContainsString(const json::array* Values, std::string_view Expected) {
    if (Values == nullptr) {
        return false;
    }
    for (const json::value& Value : *Values) {
        if (Value.is_string()) {
            const json::string& Text = Value.as_string();
            if (std::string_view(Text.data(), Text.size()) == Expected) {
                return true;
            }
        }
    }
    return false;
}

void CheckFixtureEvents(const json::object& FixtureCase,
                        const RunResult& Result);

void RunCanonicalRequestFixture(const json::object& FixtureCase) {
    const json::object* Canonical =
        ObjectField(FixtureCase, "CanonicalRequest");
    const json::object* ExpectedWire =
        ObjectField(FixtureCase, "ExpectedWireRequest");
    Check(Canonical != nullptr && ExpectedWire != nullptr,
          "canonical request fixture includes input and expected wire request");
    if (Canonical == nullptr || ExpectedWire == nullptr) {
        return;
    }

    FakeHttpTransport Transport;
    const std::string Response =
        Data(R"({"id":"fixture-request-response","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})") +
        Done();
    Transport.Next.Chunks = {Response};
    Transport.Next.Terminal =
        CompletedTransport{200, static_cast<std::uint64_t>(Response.size())};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile =
        BuildProfile(ObjectField(FixtureCase, "Profile"));
    CanonicalModelRequest Request = BuildRequest(*Canonical);
    const RunResult Result = Run(Provider, Profile, Request);
    Check(TypeOf(Result.Terminal) == EventType::ResponseCompleted,
          "canonical request fixture reaches a terminal response");
    Check(Transport.Requests.size() == 1,
          "canonical request fixture sends exactly one request");
    if (Transport.Requests.empty()) {
        return;
    }

    boost::system::error_code Error;
    const json::value ActualBody =
        json::parse(Transport.Requests.front().BodyUtf8, Error);
    Check(!Error && ActualBody == json::value{*ExpectedWire},
          "canonical request fixture matches exact wire JSON");
    const json::array* LocalOnly =
        ArrayField(FixtureCase, "ExpectedLocalOnlyFieldsAbsentFromWire");
    if (LocalOnly != nullptr) {
        for (const json::value& NameValue : *LocalOnly) {
            if (!NameValue.is_string()) {
                continue;
            }
            const json::string& Name = NameValue.as_string();
            Check(Transport.Requests.front().BodyUtf8.find(
                      std::string_view(Name.data(), Name.size())) ==
                      std::string::npos,
                  "canonical fixture local-only field is absent from wire body");
        }
    }
}

void RunStreamFixture(const json::object& FixtureCase) {
    const json::array* Frames = ArrayField(FixtureCase, "WireFrames");
    Check(Frames != nullptr, "stream fixture includes wire frames");
    if (Frames == nullptr) {
        return;
    }
    FakeHttpTransport Transport;
    Transport.Next.StatusCode = 200;
    Transport.Next.Chunks = ChunksFromWireFrames(*Frames, &FixtureCase);
    std::uint64_t BodyBytes = 0;
    for (const std::string& Chunk : Transport.Next.Chunks) {
        BodyBytes += static_cast<std::uint64_t>(Chunk.size());
    }
    Transport.Next.Terminal = CompletedTransport{200, BodyBytes};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile =
        BuildProfile(ObjectField(FixtureCase, "Profile"));
    CanonicalModelRequest Request = MakeRequest();
    ApplyRequestSettings(FixtureCase, Request);
    const RunResult Result = Run(Provider, Profile, Request);
    CheckFixtureEvents(FixtureCase, Result);
    Check(Transport.Requests.size() == 1,
          "stream fixture dispatches one offline scripted request");
}

void RunErrorFixture(const json::object& FixtureCase) {
    FakeHttpTransport Transport;
    Transport.Next.StatusCode = 200;
    Transport.Next.Terminal = CompletedTransport{200, 0};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile =
        BuildProfile(ObjectField(FixtureCase, "Profile"));
    CanonicalModelRequest Request = MakeRequest();
    if (const json::object* Canonical =
            ObjectField(FixtureCase, "CanonicalRequest");
        Canonical != nullptr) {
        Request = BuildRequest(*Canonical);
    }
    ApplyRequestSettings(FixtureCase, Request);
    std::stop_source StopSource;
    bool UseStopToken = false;

    if (const json::object* Wire = ObjectField(FixtureCase, "WireResponse");
        Wire != nullptr) {
        const std::uint64_t Status =
            UnsignedFieldValue(*Wire, "Status").value_or(200);
        Transport.Next.StatusCode = static_cast<std::uint16_t>(Status);
        if (Status == 200) {
            Transport.Next.Chunks = {Data(json::serialize(*Wire)) + Done()};
        } else if (const json::value* Body = Field(*Wire, "Body");
                   Body != nullptr) {
            Transport.Next.Chunks = {json::serialize(*Body)};
        }
        std::uint64_t BodyBytes = 0;
        for (const std::string& Chunk : Transport.Next.Chunks) {
            BodyBytes += static_cast<std::uint64_t>(Chunk.size());
        }
        Transport.Next.Terminal = CompletedTransport{
            static_cast<std::uint16_t>(Status), BodyBytes};
    } else if (const json::array* Frames =
                   ArrayField(FixtureCase, "ReceivedFramesBeforeDisconnect");
               Frames != nullptr) {
        std::string Stream;
        AppendJsonFrames(*Frames, Stream);
        Transport.Next.Chunks = {std::move(Stream)};
    } else if (const json::array* Frames =
                   ArrayField(FixtureCase, "ReceivedFrames");
               Frames != nullptr) {
        std::string Stream;
        AppendJsonFrames(*Frames, Stream);
        const std::string TerminalFrame =
            StringField(FixtureCase, "TerminalSseFrame");
        if (!TerminalFrame.empty()) {
            Stream += TerminalFrame;
            Stream += "\n\n";
        }
        Transport.Next.Chunks = {std::move(Stream)};
    } else if (const json::value* SseData =
                   Field(FixtureCase, "ReceivedSseData");
               SseData != nullptr) {
        std::string Stream;
        if (const json::array* DataValues = SseData->if_array();
            DataValues != nullptr) {
            for (const json::value& DataValue : *DataValues) {
                if (!DataValue.is_string()) {
                    Check(false, "fixture SSE data entries are strings");
                    continue;
                }
                const json::string& Text = DataValue.as_string();
                Stream += Data(std::string_view(Text.data(), Text.size()));
            }
        } else if (SseData->is_string()) {
            const json::string& Text = SseData->as_string();
            Stream += Data(std::string_view(Text.data(), Text.size()));
        } else {
            Check(false, "fixture SSE data is a string or string array");
        }
        const std::string TerminalFrame =
            StringField(FixtureCase, "TerminalSseFrame");
        if (!TerminalFrame.empty()) {
            Stream += TerminalFrame;
            Stream += "\n\n";
        }
        Transport.Next.Chunks = {std::move(Stream)};
    } else if (const json::object* Cancellation =
                   ObjectField(FixtureCase, "Cancellation");
               Cancellation != nullptr) {
        const json::value* Requested = Field(*Cancellation, "Requested");
        const json::value* RequestSent = Field(*Cancellation, "RequestSent");
        if (Requested != nullptr && Requested->is_bool() &&
            Requested->as_bool() && RequestSent != nullptr &&
            RequestSent->is_bool() && !RequestSent->as_bool()) {
            StopSource.request_stop();
            UseStopToken = true;
        } else {
            Transport.Next.StatusCode = 0;
            Transport.Next.Terminal = CancelledTransport{true};
        }
    }

    if (!std::holds_alternative<UnknownOutcomeTransport>(
            Transport.Next.Terminal) &&
        !std::holds_alternative<CancelledTransport>(Transport.Next.Terminal)) {
        std::uint64_t BodyBytes = 0;
        for (const std::string& Chunk : Transport.Next.Chunks) {
            BodyBytes += static_cast<std::uint64_t>(Chunk.size());
        }
        Transport.Next.Terminal = CompletedTransport{
            Transport.Next.StatusCode, BodyBytes};
    }
    const RunResult Result = UseStopToken
                                 ? Run(Provider, Profile, Request,
                                       StopSource.get_token())
                                 : Run(Provider, Profile, Request);
    CheckFixtureEvents(FixtureCase, Result);

    if (const json::value* ExpectedNetwork =
            Field(FixtureCase, "NetworkRequestExpected");
        ExpectedNetwork != nullptr && ExpectedNetwork->is_bool()) {
        Check((Transport.Requests.size() == 1) ==
                  ExpectedNetwork->as_bool(),
              "fixture network request expectation matches");
    }
    if (const json::value* AutomaticRetry =
            Field(FixtureCase, "AutomaticRetry");
        AutomaticRetry != nullptr && AutomaticRetry->is_bool() &&
        !AutomaticRetry->as_bool()) {
        Check(Transport.Requests.size() <= 1,
              "fixture automatic retry is not performed");
    }
}

void RunRegisteredFixtureCase(std::string_view File,
                              const json::object& FixtureCase) {
    const std::string PreviousFixtureCase = CurrentFixtureCase;
    CurrentFixtureCase = StringField(FixtureCase, "CaseId");
    FixtureToAdapterItemIds.clear();
    AdapterToFixtureItemIds.clear();
    if (File == "CanonicalRequest.json") {
        RunCanonicalRequestFixture(FixtureCase);
    } else if (File == "StreamCases.json" ||
               File == "RefusalCases.json") {
        RunStreamFixture(FixtureCase);
    } else if (File == "ErrorCases.json") {
        RunErrorFixture(FixtureCase);
    } else {
        Check(false, std::string("fixture file is registered: ") +
                         std::string(File));
    }
    CurrentFixtureCase = PreviousFixtureCase;
}

void RunFixtureIndex(std::string_view IndexPath,
                     std::string_view RelativeDirectory,
                     std::span<const std::string_view> ExpectedFiles) {
    const auto IndexValue = LoadFixture(IndexPath);
    Check(IndexValue.has_value(), "fixture index loads");
    if (!IndexValue.has_value()) {
        return;
    }
    const json::object* Index = IndexValue->if_object();
    Check(Index != nullptr, "fixture index is a JSON object");
    if (Index == nullptr) {
        return;
    }
    const json::array* Files = ArrayField(*Index, "FixtureFiles");
    for (const std::string_view ExpectedFile : ExpectedFiles) {
        Check(ContainsString(Files, ExpectedFile),
              "fixture index registers every required file");
    }
    const json::array* Entries = ArrayField(*Index, "Cases");
    Check(Entries != nullptr, "fixture index has case entries");
    if (Entries == nullptr) {
        return;
    }

    std::map<std::string, std::size_t> ReferencedCaseCounts;
    for (const json::value& EntryValue : *Entries) {
        const json::object* Entry = EntryValue.if_object();
        if (Entry == nullptr) {
            Check(false, "fixture index entry is an object");
            continue;
        }
        const std::string File = StringField(*Entry, "File");
        const std::string Pointer = StringField(*Entry, "JsonPointer");
        const auto CaseIndex = CaseIndexFromPointer(Pointer);
        Check(CaseIndex.has_value(), "fixture index pointer is a Cases index");
        if (!CaseIndex.has_value()) {
            continue;
        }
        std::string FixturePath;
        if (!RelativeDirectory.empty()) {
            FixturePath = std::string(RelativeDirectory) + "/" + File;
        } else {
            FixturePath = File;
        }
        const auto FixtureValue = LoadFixture(FixturePath);
        Check(FixtureValue.has_value(), "indexed fixture file loads");
        if (!FixtureValue.has_value()) {
            continue;
        }
        const json::object* Fixture = FixtureValue->if_object();
        const json::array* Cases =
            Fixture == nullptr ? nullptr : ArrayField(*Fixture, "Cases");
        Check(Cases != nullptr && *CaseIndex < Cases->size(),
              "fixture JSON pointer resolves to a case");
        if (Cases == nullptr || *CaseIndex >= Cases->size()) {
            continue;
        }
        const json::object* Case = (*Cases)[*CaseIndex].if_object();
        Check(Case != nullptr, "indexed fixture case is an object");
        if (Case == nullptr) {
            continue;
        }
        Check(StringField(*Case, "CaseId") ==
                  StringField(*Entry, "CaseId"),
              "fixture index CaseId matches resolved case");
        ++ReferencedCaseCounts[File];
        RunRegisteredFixtureCase(File, *Case);
    }

    for (const std::string_view ExpectedFile : ExpectedFiles) {
        if (ExpectedFile == IndexPath.substr(
                                IndexPath.find_last_of('/') ==
                                        std::string_view::npos
                                    ? 0
                                    : IndexPath.find_last_of('/') + 1)) {
            continue;
        }
        std::string FixturePath;
        if (!RelativeDirectory.empty()) {
            FixturePath = std::string(RelativeDirectory) + "/" +
                          std::string(ExpectedFile);
        } else {
            FixturePath = std::string(ExpectedFile);
        }
        const auto FixtureValue = LoadFixture(FixturePath);
        if (!FixtureValue.has_value()) {
            continue;
        }
        const json::object* Fixture = FixtureValue->if_object();
        const json::array* Cases =
            Fixture == nullptr ? nullptr : ArrayField(*Fixture, "Cases");
        Check(Cases != nullptr &&
                  ReferencedCaseCounts[std::string(ExpectedFile)] ==
                      (Cases == nullptr ? 0U : Cases->size()),
              "fixture index references every case in its registered file");
    }
}

void TestRegisteredFixturesExecute() {
    constexpr std::array<std::string_view, 4> MainFixtureFiles{
        "FixtureIndex.json", "CanonicalRequest.json", "StreamCases.json",
        "ErrorCases.json"};
    RunFixtureIndex("FixtureIndex.json", "", MainFixtureFiles);

    constexpr std::array<std::string_view, 2> RefusalFixtureFiles{
        "FixtureIndex.json", "RefusalCases.json"};
    RunFixtureIndex("Version1Point2/FixtureIndex.json", "Version1Point2",
                    RefusalFixtureFiles);
}

std::string_view EventName(EventType Type) {
    switch (Type) {
    case EventType::ResponseStarted:
        return "ResponseStarted";
    case EventType::TextDelta:
        return "TextDelta";
    case EventType::RefusalDelta:
        return "RefusalDelta";
    case EventType::ToolCallStarted:
        return "ToolCallStarted";
    case EventType::ToolArgumentDelta:
        return "ToolArgumentDelta";
    case EventType::ToolCallReady:
        return "ToolCallReady";
    case EventType::UsageUpdated:
        return "UsageUpdated";
    case EventType::ResponseCompleted:
        return "ResponseCompleted";
    case EventType::ResponseOutputLimited:
        return "ResponseOutputLimited";
    case EventType::ResponseFailed:
        return "ResponseFailed";
    case EventType::ResponseCancelled:
        return "ResponseCancelled";
    case EventType::ResponseOutcomeUnknown:
        return "ResponseOutcomeUnknown";
    }
    return "UnknownEvent";
}

std::string_view FailureName(FailureCode Code) {
    switch (Code) {
    case FailureCode::InvalidProfile:
        return "InvalidProfile";
    case FailureCode::InvalidRequest:
        return "InvalidRequest";
    case FailureCode::UnsupportedCapability:
        return "UnsupportedCapability";
    case FailureCode::AuthenticationFailure:
        return "AuthenticationFailure";
    case FailureCode::RateLimited:
        return "RateLimited";
    case FailureCode::TransportFailure:
        return "TransportFailure";
    case FailureCode::InvalidStream:
        return "InvalidStream";
    case FailureCode::ProviderRejected:
        return "ProviderRejected";
    case FailureCode::ContentFiltered:
        return "ContentFiltered";
    }
    return "UnknownFailure";
}

std::string_view CompletionName(CompletionKind Kind) {
    switch (Kind) {
    case CompletionKind::FinalAnswer:
        return "FinalAnswer";
    case CompletionKind::ToolHandoff:
        return "ToolHandoff";
    case CompletionKind::Refusal:
        return "Refusal";
    }
    return "UnknownCompletion";
}

std::string_view LimitName(LimitKind Kind) {
    switch (Kind) {
    case LimitKind::OutputTokens:
        return "OutputTokens";
    case LimitKind::ContextWindow:
        return "ContextWindow";
    }
    return "UnknownLimit";
}

std::string_view UsageCompletenessName(UsageCompleteness Completeness) {
    switch (Completeness) {
    case UsageCompleteness::Partial:
        return "Partial";
    case UsageCompleteness::Final:
        return "Final";
    case UsageCompleteness::Unavailable:
        return "Unavailable";
    }
    return "UnknownCompleteness";
}

void CheckOptionalUsageField(const json::object& Expected,
                             std::string_view Name,
                             const std::optional<std::uint64_t>& Actual) {
    const json::value* Value = Field(Expected, Name);
    if (Value == nullptr) {
        return;
    }
    const auto Number = UnsignedValue(Value);
    Check(Number.has_value() && Actual.has_value() && *Number == *Actual,
          std::string("fixture usage field matches: ") + std::string(Name));
}

void CheckUsageSnapshot(const json::object& Expected,
                        const UsageSnapshot& Actual) {
    CheckOptionalUsageField(Expected, "InputTokens", Actual.InputTokens);
    CheckOptionalUsageField(Expected, "OutputTokens", Actual.OutputTokens);
    CheckOptionalUsageField(Expected, "TotalTokens", Actual.TotalTokens);
    CheckOptionalUsageField(Expected, "CachedInputReadTokens",
                            Actual.CachedInputReadTokens);
    CheckOptionalUsageField(Expected, "CachedInputWriteTokens",
                            Actual.CachedInputWriteTokens);
    CheckOptionalUsageField(Expected, "ReasoningOutputTokens",
                            Actual.ReasoningOutputTokens);
    const json::value* Completeness = Field(Expected, "Completeness");
    if (Completeness != nullptr && Completeness->is_string()) {
        const json::string& Text = Completeness->as_string();
        Check(std::string_view(Text.data(), Text.size()) ==
                  UsageCompletenessName(Actual.Completeness),
              "fixture usage completeness matches");
    }
}

void CheckExpectedNonTerminal(const json::object& Expected,
                              const CanonicalNonTerminalEvent& Actual) {
    if (const json::value* Sequence = Field(Expected, "SequenceNumber");
        Sequence != nullptr) {
        const auto Number = UnsignedValue(Sequence);
        Check(Number.has_value() &&
                  *Number == MetadataOf(Actual).SequenceNumber,
              "fixture event sequence matches");
    }
    if (const json::value* ResponseId = Field(Expected, "ProviderResponseId");
        ResponseId != nullptr) {
        const auto* Started = std::get_if<ResponseStartedEvent>(&Actual);
        Check(Started != nullptr && Started->ProviderResponse.has_value() &&
                  Started->ProviderResponse->Value ==
                      StringField(Expected, "ProviderResponseId"),
              "fixture provider response id matches");
    }
    if (Field(Expected, "ItemId") != nullptr) {
        const std::string ExpectedItem = StringField(Expected, "ItemId");
        std::string ActualItem;
        if (const auto* Text = std::get_if<TextDeltaEvent>(&Actual)) {
            ActualItem = Text->Item.Value;
        } else if (const auto* Refusal =
                       std::get_if<RefusalDeltaEvent>(&Actual)) {
            ActualItem = Refusal->Item.Value;
        } else if (const auto* Started =
                       std::get_if<ToolCallStartedEvent>(&Actual)) {
            ActualItem = Started->Item.Value;
        } else if (const auto* Delta =
                       std::get_if<ToolArgumentDeltaEvent>(&Actual)) {
            ActualItem = Delta->Item.Value;
        } else if (const auto* Ready =
                       std::get_if<ToolCallReadyEvent>(&Actual)) {
            ActualItem = Ready->Item.Value;
        }
        bool Matches = !ActualItem.empty();
        if (Matches) {
            const auto [ExpectedMapping, ExpectedInserted] =
                FixtureToAdapterItemIds.try_emplace(ExpectedItem, ActualItem);
            const auto [ActualMapping, ActualInserted] =
                AdapterToFixtureItemIds.try_emplace(ActualItem, ExpectedItem);
            Matches = (ExpectedInserted ||
                       ExpectedMapping->second == ActualItem) &&
                      (ActualInserted ||
                       ActualMapping->second == ExpectedItem);
        }
        Check(Matches, "fixture item identity is nonempty and stable");
    }
    if (Field(Expected, "TextUtf8Delta") != nullptr) {
        const std::string ExpectedText = StringField(Expected, "TextUtf8Delta");
        const auto* Text = std::get_if<TextDeltaEvent>(&Actual);
        const auto* Refusal = std::get_if<RefusalDeltaEvent>(&Actual);
        Check((Text != nullptr && Text->Utf8Delta == ExpectedText) ||
                  (Refusal != nullptr && Refusal->Utf8Delta == ExpectedText),
              "fixture text delta matches");
    }
    if (Field(Expected, "ToolCallId") != nullptr) {
        const std::string ExpectedCall = StringField(Expected, "ToolCallId");
        bool Matches = false;
        if (const auto* Started = std::get_if<ToolCallStartedEvent>(&Actual)) {
            Matches = Started->Call.Value == ExpectedCall;
        } else if (const auto* Delta =
                       std::get_if<ToolArgumentDeltaEvent>(&Actual)) {
            Matches = Delta->Call.Value == ExpectedCall;
        } else if (const auto* Ready =
                       std::get_if<ToolCallReadyEvent>(&Actual)) {
            Matches = Ready->Call.Value == ExpectedCall;
        }
        Check(Matches, "fixture tool call id matches");
    }
    if (Field(Expected, "ToolName") != nullptr) {
        const std::string ExpectedName = StringField(Expected, "ToolName");
        const auto* Started = std::get_if<ToolCallStartedEvent>(&Actual);
        const auto* Ready = std::get_if<ToolCallReadyEvent>(&Actual);
        Check((Started != nullptr && Started->ToolName == ExpectedName) ||
                  (Ready != nullptr && Ready->ToolName == ExpectedName),
              "fixture tool name matches");
    }
    if (Field(Expected, "ArgumentsJsonDelta") != nullptr) {
        const auto* Delta = std::get_if<ToolArgumentDeltaEvent>(&Actual);
        Check(Delta != nullptr &&
                  Delta->JsonTextDelta ==
                      StringField(Expected, "ArgumentsJsonDelta"),
              "fixture tool arguments delta matches");
    }
    if (Field(Expected, "ArgumentsJson") != nullptr) {
        const auto* Ready = std::get_if<ToolCallReadyEvent>(&Actual);
        Check(Ready != nullptr &&
                  Ready->Arguments.JsonText ==
                      StringField(Expected, "ArgumentsJson"),
              "fixture complete tool arguments match");
    }
    if (const json::object* Usage = ObjectField(Expected, "UsageSnapshot");
        Usage != nullptr) {
        const auto* Updated = std::get_if<UsageUpdatedEvent>(&Actual);
        Check(Updated != nullptr, "fixture usage event is present");
        if (Updated != nullptr) {
            CheckUsageSnapshot(*Usage, Updated->Usage);
        }
    }
}

void CheckExpectedTerminal(const json::object& Expected,
                           const CanonicalTerminalEvent& Actual) {
    if (const json::value* Sequence = Field(Expected, "SequenceNumber");
        Sequence != nullptr) {
        const auto Number = UnsignedValue(Sequence);
        Check(Number.has_value() &&
                  *Number == MetadataOf(Actual).SequenceNumber,
              "fixture terminal sequence matches");
    }
    if (Field(Expected, "CompletionKind") != nullptr) {
        const auto* Completed = std::get_if<ResponseCompletedEvent>(&Actual);
        Check(Completed != nullptr &&
                  CompletionName(Completed->Kind) ==
                      StringField(Expected, "CompletionKind"),
              "fixture completion kind matches");
    }
    if (Field(Expected, "LimitKind") != nullptr) {
        const auto* Limited = std::get_if<ResponseOutputLimitedEvent>(&Actual);
        Check(Limited != nullptr &&
                  LimitName(Limited->Kind) ==
                      StringField(Expected, "LimitKind"),
              "fixture limit kind matches");
    }
    if (Field(Expected, "ErrorCode") != nullptr) {
        const auto* Failed = std::get_if<ResponseFailedEvent>(&Actual);
        Check(Failed != nullptr &&
                  FailureName(Failed->Failure.Code) ==
                      StringField(Expected, "ErrorCode"),
              "fixture failure code matches");
        if (Failed != nullptr && Field(Expected, "Retryable") != nullptr &&
            Field(Expected, "Retryable")->is_bool()) {
            Check(Failed->Failure.Retryable ==
                      Field(Expected, "Retryable")->as_bool(),
                  "fixture retryable flag matches");
        }
    }
    if (Field(Expected, "ReasonCode") != nullptr) {
        const std::string Reason = StringField(Expected, "ReasonCode");
        bool Matches = false;
        if (const auto* Cancelled =
                std::get_if<ResponseCancelledEvent>(&Actual)) {
            Matches = Cancelled->Reason.Code.Value == Reason;
        } else if (const auto* Unknown =
                       std::get_if<ResponseOutcomeUnknownEvent>(&Actual)) {
            Matches = Unknown->Reason.Code.Value == Reason;
        }
        Check(Matches, "fixture terminal reason code matches");
    }
    if (const json::object* Usage = ObjectField(Expected, "FinalUsageSnapshot");
        Usage != nullptr) {
        const auto* Completed = std::get_if<ResponseCompletedEvent>(&Actual);
        const auto* Limited = std::get_if<ResponseOutputLimitedEvent>(&Actual);
        Check((Completed != nullptr && Completed->FinalUsage.has_value()) ||
                  Limited != nullptr,
              "fixture final usage snapshot is present");
        if (Completed != nullptr && Completed->FinalUsage.has_value()) {
            CheckUsageSnapshot(*Usage, *Completed->FinalUsage);
        } else if (Limited != nullptr) {
            CheckUsageSnapshot(*Usage, Limited->Usage);
        }
    }
    if (const json::object* Usage = ObjectField(Expected, "Usage");
        Usage != nullptr) {
        const auto* Limited = std::get_if<ResponseOutputLimitedEvent>(&Actual);
        Check(Limited != nullptr, "fixture output limited event is present");
        if (Limited != nullptr) {
            CheckUsageSnapshot(*Usage, Limited->Usage);
        }
    }
}

void CheckFixtureEvents(const json::object& FixtureCase,
                        const RunResult& Result) {
    const json::array* ExpectedEvents =
        ArrayField(FixtureCase, "ExpectedCanonicalEvents");
    Check(ExpectedEvents != nullptr,
          "fixture case contains expected canonical events");
    if (ExpectedEvents == nullptr) {
        return;
    }
    const std::vector<EventType> ActualTypes = TypesOf(Result);
    Check(ActualTypes.size() == ExpectedEvents->size(),
          "fixture canonical event count matches");
    const std::size_t Count =
        std::min(ActualTypes.size(), ExpectedEvents->size());
    for (std::size_t Index = 0; Index < Count; ++Index) {
        const json::object* Expected = (*ExpectedEvents)[Index].if_object();
        if (Expected == nullptr) {
            Check(false, "fixture expected event is an object");
            continue;
        }
        const std::string ExpectedType = StringField(*Expected, "EventType");
        Check(EventName(ActualTypes[Index]) == ExpectedType,
              "fixture event type and order match");
        if (Index < Result.NonTerminal.size()) {
            CheckExpectedNonTerminal(*Expected, Result.NonTerminal[Index]);
        } else {
            Check(Index == Result.NonTerminal.size(),
                  "fixture has one terminal event at the end");
            CheckExpectedTerminal(*Expected, Result.Terminal);
        }
    }
    if (Field(FixtureCase, "ExpectedTerminalEventType") != nullptr) {
        Check(EventName(TypeOf(Result.Terminal)) ==
                  StringField(FixtureCase, "ExpectedTerminalEventType"),
              "fixture terminal type matches");
    }
    if (const json::value* ExactlyOnce =
            Field(FixtureCase, "TerminalExactlyOnce");
        ExactlyOnce != nullptr && ExactlyOnce->is_bool()) {
        Check(ExactlyOnce->as_bool(),
              "fixture declares exactly one terminal event");
        Check(ActualTypes.size() == Result.NonTerminal.size() + 1,
              "adapter returns exactly one terminal event");
    }
    const json::array* DoNotEmit = ArrayField(FixtureCase, "DoNotEmit");
    if (DoNotEmit == nullptr) {
        DoNotEmit = ArrayField(FixtureCase, "MustNotEmit");
    }
    if (DoNotEmit != nullptr) {
        for (const json::value& ForbiddenValue : *DoNotEmit) {
            if (!ForbiddenValue.is_string()) {
                continue;
            }
            const json::string& ForbiddenText = ForbiddenValue.as_string();
            const std::string_view Forbidden(ForbiddenText.data(),
                                             ForbiddenText.size());
            const std::size_t Slash = Forbidden.find('/');
            const std::string_view ForbiddenType =
                Forbidden.substr(0, Slash);
            bool Present = false;
            for (const EventType Type : ActualTypes) {
                if (EventName(Type) == ForbiddenType) {
                    if (Slash == std::string_view::npos) {
                        Present = true;
                    } else if (Type == EventType::ResponseCompleted) {
                        const auto* Completed =
                            std::get_if<ResponseCompletedEvent>(&Result.Terminal);
                        Present = Completed != nullptr &&
                                  CompletionName(Completed->Kind) ==
                                      Forbidden.substr(Slash + 1);
                    }
                }
            }
            Check(!Present, "fixture forbidden event is absent");
        }
    }
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
    const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
    Check(Failure != nullptr &&
              Failure->Failure.Code == FailureCode::UnsupportedCapability,
          "unregistered usage extension is rejected");
    Check(std::none_of(Result.NonTerminal.begin(), Result.NonTerminal.end(),
                       [](const CanonicalNonTerminalEvent& Event) {
                           return TypeOf(Event) == EventType::UsageUpdated;
                       }),
          "unregistered usage extension emits no partial usage event");
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
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 200;
        Transport.Next.Terminal =
            TimedOutTransport{TransportTimeoutPhase::Response};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseOutcomeUnknown,
              "response timeout after HTTP head is outcome unknown");
        Check(Transport.Requests.size() == 1,
              "response timeout never starts a second request");
    }
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 200;
        Transport.Next.Terminal = TimedOutTransport{TransportTimeoutPhase::Idle};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseOutcomeUnknown,
              "idle timeout after HTTP head is outcome unknown");
    }
    {
        FakeHttpTransport Transport;
        Transport.Next.StatusCode = 0;
        Transport.Next.Terminal = TimedOutTransport{TransportTimeoutPhase::Send};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        CanonicalModelRequest Request = MakeRequest();
        const RunResult Result = Run(Provider, Profile, Request);
        Check(TypeOf(Result.Terminal) == EventType::ResponseOutcomeUnknown,
              "send timeout without proof of non-delivery is outcome unknown");
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
    const auto ExpectFailure = [&Provider, &Transport, &Profile](
                                   CanonicalModelRequest Request,
                                   FailureCode Expected,
                                   std::string_view Description) {
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure =
            std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr &&
                  Failure->Failure.Code == Expected,
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
        ExpectFailure(Request, FailureCode::InvalidRequest,
                      "tool message without tool_call_id rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        CanonicalMessage Assistant;
        Assistant.Role = MessageRole::Assistant;
        Assistant.FunctionCalls.push_back(
            FunctionCall{ToolCallId{"call"}, "lookup", JsonValue{"{"}});
        Request.Messages.push_back(Assistant);
        ExpectFailure(Request, FailureCode::InvalidRequest,
                      "invalid assistant tool arguments rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.ToolDefinitions.push_back(ToolDefinition{
            "lookup", "description", JsonSchema{"{"}, false});
        ExpectFailure(Request, FailureCode::InvalidRequest,
                      "invalid tool schema rejected");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        Request.Messages[0].ContentParts.emplace_back(
            ImageContentReference{"opaque-image"});
        ExpectFailure(Request, FailureCode::UnsupportedCapability,
                      "image without vision capability rejected explicitly");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        CanonicalMessage Assistant;
        Assistant.Role = MessageRole::Assistant;
        Assistant.FunctionCalls.push_back(FunctionCall{
            ToolCallId{"call-before-result"}, "lookup", JsonValue{"{}"}});
        Request.Messages.push_back(Assistant);
        CanonicalMessage Tool;
        Tool.Role = MessageRole::Tool;
        Tool.ToolCallResultId = ToolCallId{"different-call"};
        Tool.ContentParts.emplace_back(TextContentPart{"result"});
        Request.Messages.push_back(Tool);
        ExpectFailure(Request, FailureCode::InvalidRequest,
                      "tool result id must match a preceding assistant call");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        CanonicalMessage Assistant;
        Assistant.Role = MessageRole::Assistant;
        Assistant.FunctionCalls.push_back(FunctionCall{
            ToolCallId{"call-once"}, "lookup", JsonValue{"{}"}});
        Request.Messages.push_back(Assistant);
        for (int Index = 0; Index < 2; ++Index) {
            CanonicalMessage Tool;
            Tool.Role = MessageRole::Tool;
            Tool.ToolCallResultId = ToolCallId{"call-once"};
            Tool.ContentParts.emplace_back(TextContentPart{"result"});
            Request.Messages.push_back(Tool);
        }
        ExpectFailure(Request, FailureCode::InvalidRequest,
                      "a tool result cannot consume an assistant call twice");
    }
    {
        CanonicalModelRequest Request = MakeRequest();
        CanonicalMessage Assistant;
        Assistant.Role = MessageRole::Assistant;
        Assistant.FunctionCalls.push_back(FunctionCall{
            ToolCallId{"duplicate-call"}, "lookup", JsonValue{"{}"}});
        Assistant.FunctionCalls.push_back(FunctionCall{
            ToolCallId{"duplicate-call"}, "lookup", JsonValue{"{}"}});
        Request.Messages.push_back(Assistant);
        ExpectFailure(Request, FailureCode::InvalidRequest,
                      "assistant tool call ids are unique in the request");
    }
}

void TestRequestLevelContentPartsAreSerialized() {
    FakeHttpTransport Transport;
    Transport.Next.Chunks = {
        Data(R"({"id":"content-parts","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})"),
        Done()};
    Transport.Next.Terminal = CompletedTransport{200, 64};
    auto Provider = MakeProvider(Transport);
    ModelProviderProfile Profile = MakeProfile();
    Profile.Capabilities.Vision = true;
    CanonicalModelRequest Request = MakeRequest();
    Request.ContentParts.emplace_back(TextContentPart{"current request text"});
    Request.ContentParts.emplace_back(ImageContentReference{"opaque-image-ref"});

    const RunResult Result = Run(Provider, Profile, Request);
    Check(TypeOf(Result.Terminal) == EventType::ResponseCompleted,
          "request-level content parts are accepted when supported");
    Check(Transport.Requests.size() == 1,
          "request-level content parts produce one request");
    if (Transport.Requests.empty()) {
        return;
    }

    boost::system::error_code Error;
    const json::value BodyValue =
        json::parse(Transport.Requests.front().BodyUtf8, Error);
    const json::object* Body = BodyValue.if_object();
    const json::value* MessagesValue =
        Body == nullptr ? nullptr : Body->if_contains("messages");
    const json::array* Messages =
        MessagesValue == nullptr ? nullptr : MessagesValue->if_array();
    Check(!Error && Messages != nullptr && Messages->size() == 2,
          "request-level content becomes a final user message");
    if (Messages == nullptr || Messages->size() != 2) {
        return;
    }
    const json::object* User = (*Messages)[1].if_object();
    const json::value* PartsValue =
        User == nullptr ? nullptr : User->if_contains("content");
    const json::array* Parts =
        PartsValue == nullptr ? nullptr : PartsValue->if_array();
    Check(User != nullptr && User->if_contains("role") != nullptr &&
              User->at("role").as_string() == "user" && Parts != nullptr &&
              Parts->size() == 2,
          "request-level text and image preserve order");
    if (Parts != nullptr && Parts->size() == 2) {
        const json::object* Text = (*Parts)[0].if_object();
        const json::object* Image = (*Parts)[1].if_object();
        Check(Text != nullptr && Text->at("text").as_string() ==
                                     "current request text",
              "request-level text content is preserved");
        Check(Image != nullptr && Image->at("image_url").is_object(),
              "supported request-level image is preserved");
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
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{},"finish_reason":"stop","x_future":true}]})"),
         FailureCode::UnsupportedCapability},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"x_future":true},"finish_reason":"stop"}]})"),
         FailureCode::UnsupportedCapability},
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
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"n","arguments":"{}"},"x_future":true}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::UnsupportedCapability},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"n","arguments":"{}","x_future":true}}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::UnsupportedCapability},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"n","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":-1}})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":"3"}})"),
         FailureCode::InvalidStream},
        {Data(R"({"id":"x","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5,"x_future":1}})"),
         FailureCode::UnsupportedCapability},
    };
    for (const Case& Current : Cases) {
        const auto [Result, Requests] = RunStream(Current.Stream);
        static_cast<void>(Requests);
        const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr && Failure->Failure.Code == Current.Expected,
              "stream protocol error mapping");
    }
}

void TestMalformedCompatibilityUsageFailsClosed() {
    struct Case final {
        std::string Usage;
        FailureCode Expected;
    };
    const Case Cases[] = {
        {R"({"prompt_tokens":3,"completion_tokens":2,"total_tokens":5,"prompt_tokens_details":{"cached_tokens":-1}})",
         FailureCode::InvalidStream},
        {R"({"prompt_tokens":3,"completion_tokens":2,"total_tokens":5,"prompt_tokens_details":{"cached_tokens":"1"}})",
         FailureCode::InvalidStream},
        {R"({"prompt_tokens":3,"completion_tokens":2,"total_tokens":5,"prompt_tokens_details":{"cached_tokens":1,"x_future":2}})",
         FailureCode::UnsupportedCapability},
    };
    for (const Case& Current : Cases) {
        FakeHttpTransport Transport;
        const std::string Stream =
            Data("{\"id\":\"usage-invalid\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":" +
                 Current.Usage + "}") +
            Done();
        Transport.Next.Chunks = {Stream};
        Transport.Next.Terminal = CompletedTransport{200, Stream.size()};
        auto Provider = MakeProvider(Transport);
        ModelProviderProfile Profile = MakeProfile();
        Profile.CompatibilityProfile =
            CompatibilityProfileId{"fixture-chat-extensions-v1"};
        CanonicalModelRequest Request = MakeRequest();
        AddIncludeUsage(Request);
        const RunResult Result = Run(Provider, Profile, Request);
        const auto* Failure = std::get_if<ResponseFailedEvent>(&Result.Terminal);
        Check(Failure != nullptr && Failure->Failure.Code == Current.Expected,
              "malformed compatibility usage fails closed");
        Check(std::none_of(Result.NonTerminal.begin(), Result.NonTerminal.end(),
                           [](const CanonicalNonTerminalEvent& Event) {
                               return TypeOf(Event) == EventType::UsageUpdated;
                           }),
              "malformed usage is not emitted as final usage");
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
    TestRequestLevelContentPartsAreSerialized();
    TestRegisteredFixturesExecute();
    TestStreamProtocolErrors();
    TestMalformedCompatibilityUsageFailsClosed();
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
