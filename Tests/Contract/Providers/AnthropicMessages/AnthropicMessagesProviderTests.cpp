#include "Source/Providers/AnthropicMessages/AnthropicMessagesProvider.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace json = boost::json;
using namespace EmulativeDialog::Providers;
using EmulativeDialog::Platform::Network::CompletedTransport;
using EmulativeDialog::Platform::Network::CancelledTransport;
using EmulativeDialog::Platform::Network::FailedTransport;
using EmulativeDialog::Platform::Network::HttpTransportPort;
using EmulativeDialog::Platform::Network::ResponseChunkSink;
using EmulativeDialog::Platform::Network::ResponseHeadMetadata;
using EmulativeDialog::Platform::Network::ResponseHeadSink;
using EmulativeDialog::Platform::Network::TransportFailureCode;
using EmulativeDialog::Platform::Network::TransportHealthState;
using EmulativeDialog::Platform::Network::TransportRequest;
using EmulativeDialog::Platform::Network::TransportTerminal;

int Failures = 0;

void Check(bool Condition, std::string_view Name) {
  if (Condition) {
    std::cout << "PASS " << Name << '\n';
  } else {
    ++Failures;
    std::cerr << "FAIL " << Name << '\n';
  }
}

std::filesystem::path RepositoryRoot() {
  auto Directory =
      std::filesystem::path(__FILE__).parent_path();
  for (int Step = 0; Step < 4; ++Step) {
    Directory = Directory.parent_path();
  }
  return Directory;
}

json::value LoadFixture(std::string_view RelativePath) {
  const auto Path = RepositoryRoot() / "Tests/Contract/Providers/" /
                    std::filesystem::path(RelativePath);
  std::ifstream Stream(Path, std::ios::binary);
  if (!Stream) {
    throw std::runtime_error("fixture missing: " + Path.string());
  }
  std::string Content((std::istreambuf_iterator<char>(Stream)),
                      std::istreambuf_iterator<char>());
  return json::parse(Content);
}

std::vector<std::string> FixtureFrames(std::string_view RelativePath,
                                       std::string_view CaseId) {
  const auto Root = LoadFixture(RelativePath).as_object();
  for (const auto& Case : Root.at("Cases").as_array()) {
    const auto& Object = Case.as_object();
    if (Object.at("CaseId").as_string() != CaseId) {
      continue;
    }
    std::vector<std::string> Frames;
    if (const auto* WireFrames = Object.if_contains("WireFrames");
        WireFrames != nullptr) {
      for (const auto& Frame : WireFrames->as_array()) {
        Frames.emplace_back(Frame.as_string().c_str());
      }
    }
    return Frames;
  }
  throw std::runtime_error("fixture case missing");
}

class FakeCredentialLease final : public CredentialLease {
public:
  [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept
      override {
    return "synthetic-test-key";
  }
};

class ValidEndpointGrantResolver final
    : public EmulativeDialog::Platform::Network::EndpointGrantResolver {
public:
  [[nodiscard]] EmulativeDialog::Platform::Network::EndpointGrantStatus
  Validate(const EmulativeDialog::Platform::Network::EndpointGrantId&,
           const EmulativeDialog::Platform::Network::HttpsDestination&) const
      noexcept override {
    return EmulativeDialog::Platform::Network::EndpointGrantStatus::Valid;
  }
};

EmulativeDialog::Platform::Network::EndpointGrantResolver&
DefaultEndpointGrantResolver() {
  static ValidEndpointGrantResolver Resolver;
  return Resolver;
}

class FakeHttpTransport final : public HttpTransportPort {
public:
  FakeHttpTransport() : HttpTransportPort(DefaultEndpointGrantResolver()) {}

  std::vector<std::string> Frames;
  TransportTerminal Result{CompletedTransport{.StatusCode = 200,
                                              .ReceivedBodyBytes = 0}};
  ResponseHeadMetadata Head{.StatusCode = 200,
                            .ContentType = "text/event-stream",
                            .RetryAfter = std::nullopt};
  std::optional<TransportRequest> LastRequest;
  int CallCount{0};

protected:
  TransportTerminal DoStream(const TransportRequest& Request,
                             const ResponseHeadSink& OnHead,
                             const ResponseChunkSink& OnChunk,
                             std::stop_token) override {
    ++CallCount;
    LastRequest = Request;
    OnHead(Head);
    for (const std::string& Frame : Frames) {
      const auto* Bytes = reinterpret_cast<const std::byte*>(Frame.data());
      OnChunk(std::span<const std::byte>(Bytes, Frame.size()));
    }
    return Result;
  }

public:
  TransportHealthState Health(std::stop_token) noexcept override {
    return TransportHealthState::Ready;
  }
};

ModelProviderProfile MakeProfile() {
  return ModelProviderProfile{
      .Id = ProviderProfileId{.Value = "profile-synthetic-anthropic-v1"},
      .Protocol = ProviderProtocol::AnthropicMessages,
      .Endpoint = "https://api.anthropic.com/v1/messages",
      .ModelName = "fixture-model",
      .Credential = {},
      .Authentication = AuthenticationScheme::XApiKeyHeader,
      .ProtocolVersion = "2026-10-21",
      .Sampling = {},
      .Limit = OutputLimit{.MaximumOutputTokens = 128},
      .Timeouts = {},
      .Retries = {},
      .Capabilities = ProviderCapabilities{.Streaming = true,
                                           .ToolCalls = true,
                                           .Usage = true,
                                           .Cancellation = true,
                                           .OutputLimit = true},
      .CompatibilityProfile = std::nullopt,
  };
}

AnthropicEndpointBinding MakeBinding() {
  return AnthropicEndpointBinding{
      .Grant = EmulativeDialog::Platform::Network::EndpointGrantId{
          .Value = "grant-synthetic-anthropic"},
      .Destination = EmulativeDialog::Platform::Network::HttpsDestination{
          .Host = "api.anthropic.com",
          .Port = 443,
          .Path = "/v1/messages"},
  };
}

AnthropicMessagesProvider MakeProvider(FakeHttpTransport& Transport) {
  return AnthropicMessagesProvider(Transport, MakeBinding());
}

CanonicalModelRequest MakeRequest(std::string RequestId = "req_fixture_text",
                                  std::string TurnId = "turn_fixture_text") {
  CanonicalModelRequest Request;
  Request.Id = EmulativeDialog::Providers::RequestId{
      .Value = std::move(RequestId)};
  Request.TurnId = EmulativeDialog::Providers::AgentTurnId{
      .Value = std::move(TurnId)};
  Request.SystemInstructions = "Synthetic system instruction.";
  Request.Messages.push_back(CanonicalMessage{
      .Role = MessageRole::User,
      .ContentParts = {TextContentPart{.Text = "Synthetic question."}},
      .FunctionCalls = {},
      .ToolCallResultId = std::nullopt,
  });
  Request.ModelName = "fixture-model";
  Request.Limit = OutputLimit{.MaximumOutputTokens = 128};
  return Request;
}

std::vector<CanonicalNonTerminalEvent> RunFrames(
    FakeHttpTransport& Transport, AnthropicMessagesProvider& Provider,
    const CanonicalModelRequest& Request, CanonicalTerminalEvent& Terminal,
    const ModelProviderProfile& Profile = MakeProfile(),
    std::stop_token Token = {}) {
  static_cast<void>(Transport);
  FakeCredentialLease Lease;
  std::vector<CanonicalNonTerminalEvent> Events;
  Terminal = Provider.StreamTurn(
      Profile, Lease, Request, Token,
      [&Events](const CanonicalNonTerminalEvent& Event) {
        Events.push_back(Event);
      });
  return Events;
}

std::optional<ProviderError> TerminalFailure(
    const CanonicalTerminalEvent& Terminal) {
  if (const auto* Failed = std::get_if<ResponseFailedEvent>(&Terminal)) {
    return Failed->Failure;
  }
  return std::nullopt;
}

const TextDeltaEvent* AsTextDelta(const CanonicalNonTerminalEvent& Event) {
  if (const auto* Value = std::get_if<TextDeltaEvent>(&Event)) {
    return Value;
  }
  return nullptr;
}

void TestCapabilitiesAndProfile() {
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  const auto Capabilities =
      Provider.GetCapabilities(ProviderProfileId{.Value = "profile"});
  Check(std::holds_alternative<ProviderCapabilities>(Capabilities),
        "capabilities available");
  if (const auto* Value = std::get_if<ProviderCapabilities>(&Capabilities)) {
    Check(Value->Streaming && Value->ToolCalls && Value->Usage &&
              !Value->Vision,
          "capabilities match V1");
  }
  Check(std::holds_alternative<std::monostate>(
            Provider.ValidateProfile(MakeProfile())),
        "valid profile accepted");

  auto WrongProtocol = MakeProfile();
  WrongProtocol.Protocol = ProviderProtocol::OpenAIResponses;
  Check(std::get<ProviderError>(Provider.ValidateProfile(WrongProtocol)).Code ==
            FailureCode::InvalidProfile,
        "wrong protocol rejected");

  auto WrongAuth = MakeProfile();
  WrongAuth.Authentication = AuthenticationScheme::Bearer;
  Check(std::get<ProviderError>(Provider.ValidateProfile(WrongAuth)).Code ==
            FailureCode::InvalidProfile,
        "wrong authentication rejected");

  auto MissingVersion = MakeProfile();
  MissingVersion.ProtocolVersion.clear();
  Check(
      std::get<ProviderError>(Provider.ValidateProfile(MissingVersion)).Code ==
          FailureCode::InvalidProfile,
      "missing Anthropic version rejected");

  auto Sampling = MakeProfile();
  Sampling.Sampling.Temperature = 0.5;
  Check(std::get<ProviderError>(Provider.ValidateProfile(Sampling)).Code ==
            FailureCode::UnsupportedCapability,
        "non-default sampling rejected");
}

void TestWireRequestAndHeaders() {
  FakeHttpTransport Transport;
  Transport.Frames = FixtureFrames("AnthropicMessages/Fixtures/"
                                   "StreamCases.json",
                                   "text_blocks_utf8_and_sse_fragmentation");
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  const auto Request = MakeRequest();
  static_cast<void>(RunFrames(Transport, Provider, Request, Terminal));
  Check(Transport.CallCount == 1, "wire request sent once");
  const auto& Wire = *Transport.LastRequest;
  Check(Wire.Destination.Host == "api.anthropic.com", "wire host");
  Check(Wire.Destination.Path == "/v1/messages", "wire path");
  Check(Wire.Grant.Value == "grant-synthetic-anthropic",
        "wire uses injected Core grant");
  bool SawVersionHeader = false;
  bool SawApiKeyHeader = false;
  for (const auto& Header : Wire.Headers) {
    SawVersionHeader =
        SawVersionHeader ||
        (Header.Name == "anthropic-version" &&
         Header.Value == "2026-10-21");
    SawApiKeyHeader = SawApiKeyHeader ||
                      (Header.Name == "x-api-key" &&
                       Header.Value == "synthetic-test-key");
  }
  Check(SawVersionHeader, "anthropic-version header");
  Check(SawApiKeyHeader, "credential header injected");

  const auto Body = json::parse(Wire.BodyUtf8).as_object();
  Check(Body.at("model").as_string() == "fixture-model", "wire model");
  Check(Body.at("max_tokens").as_int64() == 128, "wire max_tokens");
  Check(Body.at("stream").as_bool(), "wire stream flag");
  Check(Body.at("system").as_string() == "Synthetic system instruction.",
        "wire system instructions");
  Check(!Body.contains("RequestId") && !Body.contains("AgentTurnId") &&
            !Body.contains("CapabilitySnapshot"),
        "request metadata stays out of wire");
  const auto& Message = Body.at("messages").as_array().at(0).as_object();
  Check(Message.at("role").as_string() == "user", "wire user role");
  Check(Message.at("content").as_array().at(0).at("text").as_string() ==
            "Synthetic question.",
        "wire user text");
}

void TestSamplingRejectedBeforeSend() {
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  auto Request = MakeRequest();
  Request.Sampling.TopP = 0.9;
  CanonicalTerminalEvent Terminal;
  static_cast<void>(
      RunFrames(Transport, Provider, Request, Terminal));
  Check(Transport.CallCount == 0, "sampling rejection avoids network");
  const auto Failure = TerminalFailure(Terminal);
  Check(Failure.has_value() &&
            Failure->Code == FailureCode::UnsupportedCapability,
        "sampling rejection error code");
}

void TestTextStreamFragmentation() {
  FakeHttpTransport Transport;
  Transport.Frames = FixtureFrames("AnthropicMessages/Fixtures/"
                                   "StreamCases.json",
                                   "text_blocks_utf8_and_sse_fragmentation");
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  const auto Events = RunFrames(
      Transport, Provider, MakeRequest("req_fixture_text",
                                       "turn_fixture_text"),
      Terminal);
  Check(Events.size() == 6, "text stream non-terminal count");
  Check(std::holds_alternative<ResponseStartedEvent>(Events[0]),
        "text stream starts");
  const auto* FirstDelta = AsTextDelta(Events[2]);
  Check(FirstDelta != nullptr && FirstDelta->Utf8Delta == "你好🙂",
        "fragmented utf8 delta reassembled");
  Check(FirstDelta != nullptr &&
            FirstDelta->Item.Value == "anthropic:block:0",
        "text item id");
  const auto* SecondDelta = AsTextDelta(Events[3]);
  Check(SecondDelta != nullptr &&
            SecondDelta->Item.Value == "anthropic:block:1",
        "second text block item id");
  const auto* Completed =
      std::get_if<ResponseCompletedEvent>(&Terminal);
  Check(Completed != nullptr &&
            Completed->Kind == CompletionKind::FinalAnswer,
        "text terminal final answer");
  Check(Completed != nullptr && Completed->FinalUsage.has_value() &&
            Completed->FinalUsage->InputTokens == 12 &&
            Completed->FinalUsage->OutputTokens == 7 &&
            Completed->FinalUsage->CachedInputWriteTokens == 3 &&
            Completed->FinalUsage->CachedInputReadTokens == 2 &&
            Completed->FinalUsage->Completeness == UsageCompleteness::Final,
        "text terminal final usage");
}

void TestToolStream() {
  FakeHttpTransport Transport;
  Transport.Frames = FixtureFrames("AnthropicMessages/Fixtures/"
                                   "StreamCases.json",
                                   "tool_use_partial_json_and_stable_ids");
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  const auto Events = RunFrames(
      Transport, Provider,
      MakeRequest("req_fixture_tools", "turn_fixture_tools"), Terminal);
  Check(Events.size() == 12, "tool stream non-terminal count");
  int ReadyCount = 0;
  std::vector<std::string> ReadyArguments;
  for (const auto& Event : Events) {
    if (const auto* Ready = std::get_if<ToolCallReadyEvent>(&Event)) {
      ++ReadyCount;
      ReadyArguments.push_back(Ready->Arguments.JsonText);
    }
  }
  Check(ReadyCount == 2, "tool ready events emitted");
  bool ParsedFirst = false;
  if (!ReadyArguments.empty()) {
    const auto Parsed = json::parse(ReadyArguments.front()).as_object();
    ParsedFirst = Parsed.at("query").as_string() == "Shanghai";
  }
  Check(ParsedFirst, "tool arguments parsed");
  const auto* Completed =
      std::get_if<ResponseCompletedEvent>(&Terminal);
  Check(Completed != nullptr &&
            Completed->Kind == CompletionKind::ToolHandoff,
        "tool terminal handoff");
}

void TestOutputLimits() {
  {
    FakeHttpTransport Transport;
    Transport.Frames = FixtureFrames(
        "AnthropicMessages/Fixtures/ErrorCases.json",
        "max_tokens_output_limit");
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    CanonicalTerminalEvent Terminal;
    static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
    const auto* Limited =
        std::get_if<ResponseOutputLimitedEvent>(&Terminal);
    Check(Limited != nullptr && Limited->Kind == LimitKind::OutputTokens &&
              Limited->Usage.InputTokens == 10 &&
              Limited->Usage.OutputTokens == 20,
          "max_tokens output limit");
    Check(Limited != nullptr && !Limited->SafeGeneratedText.has_value(),
          "output limit text not sendable");
  }
  {
    FakeHttpTransport Transport;
    Transport.Frames = FixtureFrames(
        "AnthropicMessages/Fixtures/ErrorCases.json",
        "context_window_output_limit");
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    CanonicalTerminalEvent Terminal;
    static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
    const auto* Limited =
        std::get_if<ResponseOutputLimitedEvent>(&Terminal);
    Check(Limited != nullptr && Limited->Kind == LimitKind::ContextWindow,
          "context window output limit");
  }
  {
    FakeHttpTransport Transport;
    Transport.Frames = FixtureFrames(
        "AnthropicMessages/Fixtures/ErrorCases.json",
        "incomplete_tool_json_at_output_limit_never_becomes_ready");
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    CanonicalTerminalEvent Terminal;
    const auto Events =
        RunFrames(Transport, Provider, MakeRequest(), Terminal);
    bool SawStartedTool = false;
    bool SawArgument = false;
    bool SawReady = false;
    for (const auto& Event : Events) {
      SawStartedTool =
          SawStartedTool ||
          std::holds_alternative<ToolCallStartedEvent>(Event);
      SawArgument =
          SawArgument ||
          std::holds_alternative<ToolArgumentDeltaEvent>(Event);
      SawReady = SawReady || std::holds_alternative<ToolCallReadyEvent>(Event);
    }
    const auto* Limited = std::get_if<ResponseOutputLimitedEvent>(&Terminal);
    Check(SawStartedTool && SawArgument && !SawReady,
          "incomplete tool json never becomes ready");
    Check(Limited != nullptr && Limited->Kind == LimitKind::OutputTokens,
          "incomplete tool json still output limited");
  }
}

void TestStopSequence() {
  FakeHttpTransport Transport;
  Transport.Frames = FixtureFrames(
      "AnthropicMessages/Fixtures/ErrorCases.json",
      "stop_sequence_completion");
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  const auto* Completed = std::get_if<ResponseCompletedEvent>(&Terminal);
  Check(Completed != nullptr &&
            Completed->StopSequenceCode.has_value() &&
            Completed->StopSequenceCode->Value == "SYNTHETIC_STOP",
        "stop sequence code preserved");
}

void TestEarlyFailures() {
  const std::vector<std::pair<std::string_view, std::string_view>> Cases{
      {"pause_turn_server_tool_not_supported", "pause_turn"},
      {"server_tool_block_rejected", "server_tool"},
      {"unknown_content_block_rejected", "unknown_block"},
      {"mismatched_sse_event_and_data_type_is_invalid", "mismatch"},
      {"in_band_error_after_partial_text", "in_band"},
  };
  for (const auto& [CaseId, Label] : Cases) {
    FakeHttpTransport Transport;
    Transport.Frames =
        FixtureFrames("AnthropicMessages/Fixtures/ErrorCases.json", CaseId);
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    CanonicalTerminalEvent Terminal;
    static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
    const auto Failure = TerminalFailure(Terminal);
    const std::string Name = std::string("early failure ") +
                             std::string(Label);
    FailureCode Expected = FailureCode::UnsupportedCapability;
    if (CaseId == "mismatched_sse_event_and_data_type_is_invalid") {
      Expected = FailureCode::InvalidStream;
    } else if (CaseId == "in_band_error_after_partial_text") {
      Expected = FailureCode::ProviderRejected;
    }
    Check(Failure.has_value() && Failure->Code == Expected, Name);
    if (CaseId == "in_band_error_after_partial_text") {
      Check(Failure.has_value() && !Failure->Retryable,
            "in-band error after partial output is not retryable");
    }
  }
}

void TestUnexpectedEof() {
  FakeHttpTransport Transport;
  Transport.Frames = FixtureFrames(
      "AnthropicMessages/Fixtures/ErrorCases.json",
      "unexpected_eof_before_message_stop_is_unknown");
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  const auto* Unknown =
      std::get_if<ResponseOutcomeUnknownEvent>(&Terminal);
  Check(Unknown != nullptr &&
            Unknown->Reason.Code.Value == "StreamEndedBeforeTerminal",
        "unexpected eof is outcome unknown");
}

void TestCancellation() {
  {
    FakeHttpTransport Transport;
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    std::stop_source Source;
    Source.request_stop();
    CanonicalTerminalEvent Terminal;
    static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal,
                                MakeProfile(), Source.get_token()));
    const auto* Cancelled =
        std::get_if<ResponseCancelledEvent>(&Terminal);
    Check(Cancelled != nullptr &&
              Cancelled->Reason.Code.Value == "CancelledBeforeSend",
          "cancellation before send");
    Check(Transport.CallCount == 0, "cancellation before send avoids network");
  }
  {
    FakeHttpTransport Transport;
    Transport.Frames = FixtureFrames(
        "AnthropicMessages/Fixtures/ErrorCases.json",
        "cancellation_after_send_without_confirmation_is_unknown");
    Transport.Result =
        CancelledTransport{.RequestMayHaveReachedEndpoint = true};
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    CanonicalTerminalEvent Terminal;
    static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
    const auto* Unknown =
        std::get_if<ResponseOutcomeUnknownEvent>(&Terminal);
    Check(Unknown != nullptr &&
              Unknown->Reason.Code.Value == "LocalCancellationUnconfirmed",
          "cancellation after send is unknown");
  }
}

void TestHttpErrorMapping() {
  struct Case final {
    std::uint16_t Status;
    FailureCode Expected;
    bool Retryable;
  };
  const std::vector<Case> Cases{
      {401, FailureCode::AuthenticationFailure, false},
      {403, FailureCode::AuthenticationFailure, false},
      {400, FailureCode::ProviderRejected, false},
      {429, FailureCode::RateLimited, true},
      {500, FailureCode::TransportFailure, true},
      {529, FailureCode::RateLimited, true},
  };
  for (const Case& Current : Cases) {
    FakeHttpTransport Transport;
    Transport.Head.StatusCode = Current.Status;
    Transport.Result = CompletedTransport{
        .StatusCode = Current.Status, .ReceivedBodyBytes = 0};
    Transport.Frames = {
        "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
        "\"message\":\"Synthetic failure.\"}}"};
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    CanonicalTerminalEvent Terminal;
    static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
    const auto Failure = TerminalFailure(Terminal);
    Check(Failure.has_value() && Failure->Code == Current.Expected &&
              Failure->Retryable == Current.Retryable,
          "http status mapping");
  }
}

void TestRefusalV12() {
  FakeHttpTransport Transport;
  Transport.Frames = FixtureFrames(
      "AnthropicMessages/Fixtures/Version1Point2/RefusalCases.json",
      "refusal_empty_body_known_category_redacts_explanation");
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  const auto* Completed = std::get_if<ResponseCompletedEvent>(&Terminal);
  Check(Completed != nullptr &&
            Completed->Kind == CompletionKind::Refusal &&
            Completed->StopDetails.has_value() &&
            Completed->StopDetails->Category == RefusalCategory::GeneralHarms,
        "refusal stop details mapped");

  FakeHttpTransport Missing;
  Missing.Frames = FixtureFrames(
      "AnthropicMessages/Fixtures/Version1Point2/RefusalCases.json",
      "refusal_details_missing_fails_invalid_stream");
  AnthropicMessagesProvider MissingProvider = MakeProvider(Missing);
  CanonicalTerminalEvent MissingTerminal;
  static_cast<void>(RunFrames(Missing, MissingProvider, MakeRequest(),
                              MissingTerminal));
  const auto Failure = TerminalFailure(MissingTerminal);
  Check(Failure.has_value() && Failure->Code == FailureCode::InvalidStream,
        "refusal without stop details invalid");
}

void TestDuplicateTerminal() {
  FakeHttpTransport Transport;
  Transport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\",\"message\":"
      "{\"id\":\"msg_dup\",\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":1}}}\r\n\r\n",
      "event: message_delta\r\ndata: {\"type\":\"message_delta\",\"delta\":"
      "{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":1}}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n",
  };
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  const auto Failure = TerminalFailure(Terminal);
  Check(Failure.has_value() && Failure->Code == FailureCode::InvalidStream,
        "duplicate terminal invalid");
}

void TestThinkingFiltered() {
  FakeHttpTransport Transport;
  Transport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\",\"message\":"
      "{\"id\":\"msg_think\",\"usage\":{\"input_tokens\":2,"
      "\"output_tokens\":0}}}\r\n\r\n",
      "event: content_block_start\r\ndata: {\"type\":\"content_block_start\","
      "\"index\":0,\"content_block\":{\"type\":\"thinking\","
      "\"thinking\":\"\"}}\r\n\r\n",
      "event: content_block_delta\r\ndata: {\"type\":\"content_block_delta\","
      "\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
      "\"thinking\":\"private\"}}\r\n\r\n",
      "event: content_block_delta\r\ndata: {\"type\":\"content_block_delta\","
      "\"index\":0,\"delta\":{\"type\":\"signature_delta\","
      "\"signature\":\"sig\"}}\r\n\r\n",
      "event: content_block_stop\r\ndata: {\"type\":\"content_block_stop\","
      "\"index\":0}\r\n\r\n",
      "event: message_delta\r\ndata: {\"type\":\"message_delta\",\"delta\":"
      "{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":2,"
      "\"output_tokens\":1}}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n",
  };
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  const auto Events = RunFrames(Transport, Provider, MakeRequest(), Terminal);
  bool SawText = false;
  for (const auto& Event : Events) {
    SawText = SawText || std::holds_alternative<TextDeltaEvent>(Event);
  }
  Check(!SawText, "thinking deltas never reach the sink");
  Check(std::holds_alternative<ResponseCompletedEvent>(Terminal),
        "thinking stream still completes");
}

CanonicalTerminalEvent RunRaw(AnthropicMessagesProvider& Provider,
                              FakeHttpTransport& Transport,
                              std::vector<std::string> Frames,
                              CanonicalModelRequest Request = MakeRequest()) {
  Transport.Frames = std::move(Frames);
  CanonicalTerminalEvent Terminal;
  static_cast<void>(RunFrames(Transport, Provider, Request, Terminal));
  return Terminal;
}

void ExpectTerminalCode(AnthropicMessagesProvider& Provider,
                        FakeHttpTransport& Transport,
                        const std::vector<std::string>& Frames,
                        FailureCode Expected, std::string_view Name,
                        CanonicalModelRequest Request = MakeRequest()) {
  const auto Terminal = RunRaw(Provider, Transport, Frames, Request);
  const auto Failure = TerminalFailure(Terminal);
  Check(Failure.has_value() && Failure->Code == Expected, Name);
}

void TestRequestValidationMatrix() {
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);

  const auto RejectBeforeSend =
      [&Provider, &Transport](CanonicalModelRequest Request,
                              FailureCode Expected,
                              std::string_view Name) {
        Transport.CallCount = 0;
        CanonicalTerminalEvent Terminal;
        static_cast<void>(
            RunFrames(Transport, Provider, Request, Terminal));
        const auto Failure = TerminalFailure(Terminal);
        Check(Failure.has_value() && Failure->Code == Expected &&
                  Transport.CallCount == 0,
              Name);
      };

  auto MissingLimit = MakeRequest();
  MissingLimit.Limit = OutputLimit{};
  Transport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_fallback\",\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":0}}}\r\n\r\n",
      "event: message_delta\r\ndata: {\"type\":\"message_delta\","
      "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n",
  };
  Transport.CallCount = 0;
  {
    CanonicalTerminalEvent Terminal;
    static_cast<void>(
        RunFrames(Transport, Provider, MissingLimit, Terminal));
    const auto Body =
        json::parse(Transport.LastRequest->BodyUtf8).as_object();
    Check(Transport.CallCount == 1 &&
              Body.at("max_tokens").as_int64() == 128,
          "missing request max_tokens falls back to profile limit");
  }
  auto NoLimits = MakeRequest();
  NoLimits.Limit = OutputLimit{};
  auto ProfileWithoutLimit = MakeProfile();
  ProfileWithoutLimit.Limit = OutputLimit{};
  Transport.CallCount = 0;
  {
    CanonicalTerminalEvent Terminal;
    static_cast<void>(RunFrames(Transport, Provider, NoLimits, Terminal,
                                ProfileWithoutLimit));
    Check(TerminalFailure(Terminal).has_value() &&
              TerminalFailure(Terminal)->Code == FailureCode::InvalidRequest &&
              Transport.CallCount == 0,
          "missing request and profile max_tokens rejected");
  }
  auto ZeroLimit = MakeRequest();
  ZeroLimit.Limit = OutputLimit{.MaximumOutputTokens = 0};
  RejectBeforeSend(ZeroLimit, FailureCode::InvalidRequest,
                   "zero max_tokens rejected");
  auto RequestContent = MakeRequest();
  RequestContent.ContentParts.push_back(
      TextContentPart{.Text = "Synthetic request content."});
  RejectBeforeSend(RequestContent, FailureCode::InvalidRequest,
                   "request-level content parts rejected");
  auto CacheControl = MakeRequest();
  CacheControl.ProviderOptions.push_back(RegisteredProviderOption{
      .Id = RegisteredOptionId{.Value = "cache_control"},
      .Value = std::string("{\"type\":\"ephemeral\"}"),
  });
  RejectBeforeSend(CacheControl, FailureCode::UnsupportedCapability,
                   "top-level cache_control rejected");
  auto SystemRole = MakeRequest();
  SystemRole.Messages[0].Role = MessageRole::System;
  RejectBeforeSend(SystemRole, FailureCode::InvalidRequest,
                   "system role in messages rejected");
  auto ToolWithoutId = MakeRequest();
  ToolWithoutId.Messages[0].Role = MessageRole::Tool;
  RejectBeforeSend(ToolWithoutId, FailureCode::InvalidRequest,
                   "tool result without id rejected");
  auto EmptyTool = MakeRequest();
  EmptyTool.Messages[0].Role = MessageRole::Tool;
  EmptyTool.Messages[0].ToolCallResultId =
      ToolCallId{.Value = "toolu-1"};
  EmptyTool.Messages[0].ContentParts.clear();
  RejectBeforeSend(EmptyTool, FailureCode::InvalidRequest,
                   "empty tool result rejected");
  auto ImagePart = MakeRequest();
  ImagePart.Messages[0].ContentParts.push_back(
      ImageContentReference{.OpaqueReference = "opaque"});
  RejectBeforeSend(ImagePart, FailureCode::UnsupportedCapability,
                   "image content rejected");
  auto BadArguments = MakeRequest();
  BadArguments.Messages[0].Role = MessageRole::Assistant;
  BadArguments.Messages[0].FunctionCalls.push_back(FunctionCall{
      .Id = ToolCallId{.Value = "toolu-1"},
      .Name = "Synthetic",
      .Arguments = JsonValue{.JsonText = "{bad"},
  });
  RejectBeforeSend(BadArguments, FailureCode::InvalidRequest,
                   "invalid tool arguments rejected");
  auto BadSchema = MakeRequest();
  BadSchema.ToolDefinitions.push_back(ToolDefinition{
      .Name = "Synthetic",
      .Description = "desc",
      .Parameters = JsonSchema{.JsonText = "{bad"},
      .Strict = false,
  });
  RejectBeforeSend(BadSchema, FailureCode::InvalidRequest,
                   "invalid tool schema rejected");
  auto UnknownOption = MakeRequest();
  UnknownOption.ProviderOptions.push_back(RegisteredProviderOption{
      .Id = RegisteredOptionId{.Value = "unknown"},
      .Value = std::string("{}"),
  });
  RejectBeforeSend(UnknownOption, FailureCode::UnsupportedCapability,
                   "unknown provider option rejected");
  auto NonStringOption = MakeRequest();
  NonStringOption.ProviderOptions.push_back(RegisteredProviderOption{
      .Id = RegisteredOptionId{.Value = "thinking"},
      .Value = std::int64_t{1},
  });
  RejectBeforeSend(NonStringOption, FailureCode::InvalidProfile,
                   "non-string provider option rejected");
  auto BadOptionJson = MakeRequest();
  BadOptionJson.ProviderOptions.push_back(RegisteredProviderOption{
      .Id = RegisteredOptionId{.Value = "thinking"},
      .Value = std::string("{bad"),
  });
  RejectBeforeSend(BadOptionJson, FailureCode::InvalidProfile,
                   "invalid provider option json rejected");
}

void TestSuccessfulRequestVariants() {
  FakeHttpTransport Transport;
  Transport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_ok\",\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":0}}}\r\n\r\n",
      "event: message_delta\r\ndata: {\"type\":\"message_delta\","
      "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n",
  };
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  auto Request = MakeRequest();
  Request.DeveloperInstructions = "Synthetic developer instruction.";
  Request.ProviderOptions.push_back(RegisteredProviderOption{
      .Id = RegisteredOptionId{.Value = "thinking"},
      .Value = std::string("{\"type\":\"enabled\",\"budget_tokens\":1024}"),
  });
  CanonicalTerminalEvent Terminal;
  static_cast<void>(RunFrames(Transport, Provider, Request, Terminal));
  const auto Body = json::parse(Transport.LastRequest->BodyUtf8).as_object();
  Check(Body.at("system").as_string() ==
            "Synthetic system instruction.\n\n"
            "Synthetic developer instruction.",
        "developer instructions appended");
  Check(Body.at("thinking").as_object().at("type").as_string() ==
            "enabled",
        "registered thinking option passes through");

  FakeHttpTransport ToolTransport;
  AnthropicMessagesProvider ToolProvider = MakeProvider(ToolTransport);
  auto ToolRequest = MakeRequest("req_tools", "turn_tools");
  ToolRequest.Messages.clear();
  ToolRequest.Messages.push_back(CanonicalMessage{
      .Role = MessageRole::User,
      .ContentParts = {TextContentPart{.Text = "Run synthetic checks."}},
      .FunctionCalls = {},
      .ToolCallResultId = std::nullopt,
  });
  ToolRequest.Messages.push_back(CanonicalMessage{
      .Role = MessageRole::Assistant,
      .ContentParts = {TextContentPart{.Text = "I will run it."}},
      .FunctionCalls = {FunctionCall{
          .Id = ToolCallId{.Value = "toolu_synthetic_lookup"},
          .Name = "SyntheticLookup",
          .Arguments = JsonValue{.JsonText = "{\"query\":\"alpha\"}"},
      }},
      .ToolCallResultId = std::nullopt,
  });
  ToolRequest.Messages.push_back(CanonicalMessage{
      .Role = MessageRole::Tool,
      .ContentParts = {TextContentPart{.Text = "Synthetic lookup result."}},
      .FunctionCalls = {},
      .ToolCallResultId = ToolCallId{.Value = "toolu_synthetic_lookup"},
  });
  ToolRequest.Messages.push_back(CanonicalMessage{
      .Role = MessageRole::User,
      .ContentParts = {TextContentPart{.Text = "Continue."}},
      .FunctionCalls = {},
      .ToolCallResultId = std::nullopt,
  });
  ToolRequest.ToolDefinitions.push_back(ToolDefinition{
      .Name = "SyntheticLookup",
      .Description = "Synthetic tool.",
      .Parameters = JsonSchema{
          .JsonText = "{\"type\":\"object\",\"properties\":{}}"},
      .Strict = false,
  });
  ToolTransport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_tools\",\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":0}}}\r\n\r\n",
      "event: message_delta\r\ndata: {\"type\":\"message_delta\","
      "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n",
  };
  CanonicalTerminalEvent ToolTerminal;
  static_cast<void>(
      RunFrames(ToolTransport, ToolProvider, ToolRequest, ToolTerminal));
  const auto ToolBody =
      json::parse(ToolTransport.LastRequest->BodyUtf8).as_object();
  const auto& Messages = ToolBody.at("messages").as_array();
  Check(Messages.size() == 3, "tool roundtrip message count");
  if (Messages.size() == 3) {
    const auto& ResultMessage = Messages.at(2).as_object();
    const auto& Blocks = ResultMessage.at("content").as_array();
    Check(Blocks.size() == 2 &&
              Blocks.at(0).at("type").as_string() == "tool_result" &&
              Blocks.at(1).at("type").as_string() == "text",
          "tool_result precedes text");
    Check(Blocks.size() == 2 &&
              Blocks.at(0).at("tool_use_id").as_string() ==
                  "toolu_synthetic_lookup",
          "tool_result id preserved");
  }
  Check(ToolBody.at("tools").as_array().size() == 1, "tool definitions sent");
}

void TestSseMalformedMatrix() {
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  const std::string Start =
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_m\",\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":0}}}\r\n\r\n";
  const std::string BlockStart =
      "event: content_block_start\r\ndata: {\"type\":"
      "\"content_block_start\",\"index\":0,\"content_block\":"
      "{\"type\":\"text\",\"text\":\"\"}}\r\n\r\n";
  const std::string BlockStop =
      "event: content_block_stop\r\ndata: {\"type\":\"content_block_stop\","
      "\"index\":0}\r\n\r\n";
  const std::string EndDelta =
      "event: message_delta\r\ndata: {\"type\":\"message_delta\","
      "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n";
  const std::string Stop =
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n";

  ExpectTerminalCode(Provider, Transport,
                     {"event: future\r\ndata: {\"type\":\"future\"}\r\n\r\n"},
                     FailureCode::InvalidStream, "unknown SSE event");
  ExpectTerminalCode(Provider, Transport, {Start, Start},
                     FailureCode::InvalidStream, "duplicate message_start");
  ExpectTerminalCode(
      Provider, Transport,
      {"event: content_block_start\r\ndata: {\"type\":"
       "\"content_block_start\",\"index\":0,\"content_block\":"
       "{\"type\":\"text\"}}\r\n\r\n"},
      FailureCode::InvalidStream, "block before message_start");
  ExpectTerminalCode(
      Provider, Transport,
      {"event: message_delta\r\ndata: {\"type\":\"message_delta\","
       "\"delta\":{}}\r\n\r\n"},
      FailureCode::InvalidStream, "delta before message_start");
  ExpectTerminalCode(
      Provider, Transport,
      {Start,
       "event: content_block_start\r\ndata: {\"type\":"
       "\"content_block_start\",\"index\":0}\r\n\r\n"},
      FailureCode::InvalidStream, "block start without block");
  ExpectTerminalCode(
      Provider, Transport,
      {Start,
       "event: content_block_start\r\ndata: {\"type\":"
       "\"content_block_start\",\"index\":0,\"content_block\":"
       "{\"type\":\"tool_use\",\"id\":\"toolu\"}}\r\n\r\n"},
      FailureCode::InvalidStream, "tool use without name");
  ExpectTerminalCode(
      Provider, Transport,
      {Start,
       "event: content_block_delta\r\ndata: {\"type\":"
       "\"content_block_delta\",\"index\":0,\"delta\":"
       "{\"type\":\"text_delta\",\"text\":\"x\"}}\r\n\r\n"},
      FailureCode::InvalidStream, "delta without block");
  ExpectTerminalCode(
      Provider, Transport,
      {Start, BlockStart,
       "event: content_block_delta\r\ndata: {\"type\":"
       "\"content_block_delta\",\"index\":0,\"delta\":"
       "{\"type\":\"input_json_delta\",\"partial_json\":\"{}\"}}\r\n\r\n"},
      FailureCode::InvalidStream, "delta type mismatch");
  ExpectTerminalCode(
      Provider, Transport,
      {Start, BlockStart,
       "event: content_block_delta\r\ndata: {\"type\":"
       "\"content_block_delta\",\"index\":0,\"delta\":"
       "{\"type\":\"text_delta\"}}\r\n\r\n"},
      FailureCode::InvalidStream, "text delta without text");
  ExpectTerminalCode(
      Provider, Transport,
      {Start,
       "event: content_block_stop\r\ndata: {\"type\":"
       "\"content_block_stop\",\"index\":0}\r\n\r\n"},
      FailureCode::InvalidStream, "stop unknown block");
  ExpectTerminalCode(Provider, Transport, {Start, BlockStart, BlockStop,
                                           BlockStop},
                     FailureCode::InvalidStream, "duplicate block stop");
  ExpectTerminalCode(Provider, Transport, {Start, Stop},
                     FailureCode::InvalidStream, "stop without stop reason");
  ExpectTerminalCode(
      Provider, Transport,
      {Start,
       "event: content_block_start\r\ndata: {\"type\":"
       "\"content_block_start\",\"index\":0,\"content_block\":"
       "{\"type\":\"tool_use\",\"id\":\"toolu\",\"name\":\"T\"}}\r\n\r\n",
       "event: content_block_delta\r\ndata: {\"type\":"
       "\"content_block_delta\",\"index\":0,\"delta\":"
       "{\"type\":\"input_json_delta\",\"partial_json\":\"{bad\"}}\r\n\r\n",
       BlockStop,
       "event: message_delta\r\ndata: {\"type\":\"message_delta\","
       "\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":"
       "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n",
       Stop},
      FailureCode::InvalidStream, "tool_use invalid json");
  ExpectTerminalCode(
      Provider, Transport,
      {Start,
       "event: content_block_start\r\ndata: {\"type\":"
       "\"content_block_start\",\"index\":0,\"content_block\":"
       "{\"type\":\"tool_use\",\"id\":\"toolu\",\"name\":\"T\"}}\r\n\r\n",
       "event: content_block_delta\r\ndata: {\"type\":"
       "\"content_block_delta\",\"index\":0,\"delta\":"
       "{\"type\":\"input_json_delta\"}}\r\n\r\n"},
      FailureCode::InvalidStream, "input json delta without text");
  auto UnknownStop = FixtureFrames(
      "AnthropicMessages/Fixtures/ErrorCases.json",
      "unknown_stop_reason_fails_closed");
  ExpectTerminalCode(Provider, Transport, UnknownStop,
                     FailureCode::InvalidStream, "unknown stop reason");
  ExpectTerminalCode(
      Provider, Transport,
      {Start,
       "event: content_block_start\r\ndata: {\"type\":"
       "\"content_block_start\",\"index\":0,\"content_block\":"
       "{\"type\":\"tool_use\",\"id\":\"toolu_open\",\"name\":\"T\"}}\r\n\r\n",
       "event: message_delta\r\ndata: {\"type\":\"message_delta\","
       "\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":"
       "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n",
       Stop},
      FailureCode::InvalidStream, "unclosed tool block at message_stop");
  const std::string Oversized =
      "event: message_start\r\ndata: " + std::string(70'000, 'x') +
      "\r\n\r\n";
  ExpectTerminalCode(Provider, Transport, {Oversized},
                     FailureCode::InvalidStream, "oversized SSE line rejected");
}

void TestTransportTerminalMatrix() {
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  CanonicalTerminalEvent Terminal;
  Transport.Result =
      FailedTransport{.Code = TransportFailureCode::DnsFailure};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(TerminalFailure(Terminal).has_value() &&
            TerminalFailure(Terminal)->Code == FailureCode::TransportFailure,
        "transport failure mapped");
  Transport.Result =
      EmulativeDialog::Platform::Network::TimedOutTransport{};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(TerminalFailure(Terminal).has_value() &&
            TerminalFailure(Terminal)->Retryable,
        "transport timeout retryable");
  Transport.Result =
      EmulativeDialog::Platform::Network::RedirectDeniedTransport{};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(TerminalFailure(Terminal).has_value() &&
            !TerminalFailure(Terminal)->Retryable,
        "redirect denied not retryable");
  Transport.Result =
      EmulativeDialog::Platform::Network::UnknownOutcomeTransport{};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(std::holds_alternative<ResponseOutcomeUnknownEvent>(Terminal),
        "unknown transport outcome");
  Transport.Result =
      CancelledTransport{.RequestMayHaveReachedEndpoint = false};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(std::holds_alternative<ResponseCancelledEvent>(Terminal),
        "transport cancelled before send");
  Transport.Result = CompletedTransport{.StatusCode = 500,
                                        .ReceivedBodyBytes = 0};
  Transport.Frames = {"not json"};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(TerminalFailure(Terminal).has_value() &&
            TerminalFailure(Terminal)->Code == FailureCode::TransportFailure,
        "http 500 without json body");
  Transport.Result = CompletedTransport{.StatusCode = 400,
                                        .ReceivedBodyBytes = 0};
  Transport.Frames = {
      "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
      "\"message\":\"Synthetic overload.\"}}"};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(TerminalFailure(Terminal).has_value() &&
            TerminalFailure(Terminal)->Code == FailureCode::RateLimited &&
            TerminalFailure(Terminal)->Retryable,
        "overloaded body maps rate limited");
  Transport.Result = FailedTransport{
      .Code = TransportFailureCode::TlsHandshakeFailure};
  Transport.Frames.clear();
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(TerminalFailure(Terminal).has_value() &&
            !TerminalFailure(Terminal)->Retryable,
        "tls failure is not retryable");
  Transport.Result =
      FailedTransport{.Code = TransportFailureCode::ResponseLimitExceeded};
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(TerminalFailure(Terminal).has_value() &&
            !TerminalFailure(Terminal)->Retryable,
        "response limit failure is not retryable");
  Transport.Result =
      FailedTransport{.Code = TransportFailureCode::DnsFailure};
  Transport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_precedence\",\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":0}}}\r\n\r\n",
      "event: message_delta\r\ndata: {\"type\":\"message_delta\","
      "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n",
  };
  static_cast<void>(RunFrames(Transport, Provider, MakeRequest(), Terminal));
  Check(std::holds_alternative<ResponseCompletedEvent>(Terminal),
        "parsed terminal takes precedence over transport failure");
}

void TestRefusalVariantMatrix() {
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  const auto RefusalFrame = [](std::string Details) {
    return std::string(
               "event: message_delta\r\ndata: {\"type\":\"message_delta\","
               "\"delta\":{\"stop_reason\":\"refusal\"},\"usage\":"
               "{\"input_tokens\":1,\"output_tokens\":0}") +
           Details + "}\r\n\r\n";
  };
  const std::string Start =
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_r\",\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":0}}}\r\n\r\n";
  const std::string Stop =
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n";

  const auto CheckRefusal =
      [&Provider, &Transport, &Start, &Stop,
       &RefusalFrame](std::string Details, bool ExpectRefusal,
                      std::string_view Name) {
        const auto Terminal =
            RunRaw(Provider, Transport,
                   {Start, RefusalFrame(std::move(Details)), Stop});
        if (ExpectRefusal) {
          const auto* Completed =
              std::get_if<ResponseCompletedEvent>(&Terminal);
          Check(Completed != nullptr &&
                    Completed->Kind == CompletionKind::Refusal &&
                    Completed->StopDetails.has_value(),
                Name);
        } else {
          const auto Failure = TerminalFailure(Terminal);
          Check(Failure.has_value() &&
                    Failure->Code == FailureCode::InvalidStream,
                Name);
        }
      };
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\",\"category\":null}",
      true, "refusal null category unclassified");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\",\"category\":\"future\"}",
      true, "refusal unknown category unclassified");
  CheckRefusal("", false, "refusal missing details");
  CheckRefusal(",\"stop_details\":null", false,
               "refusal null details");
  CheckRefusal(",\"stop_details\":\"text\"", false,
               "refusal non-object details");
  CheckRefusal(",\"stop_details\":{\"type\":\"other\"}", false,
               "refusal wrong details type");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\",\"category\":1}", false,
      "refusal category wrong type");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\",\"explanation\":1}", false,
      "refusal explanation wrong type");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\",\"explanation\":null}", true,
      "refusal null explanation accepted");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\","
      "\"recommended_model\":\"fixture-model-2\"}",
      true, "recommended_model ignored");
  {
    const auto Terminal =
        RunRaw(Provider, Transport,
               {Start,
                RefusalFrame(
                    ",\"stop_details\":{\"type\":\"refusal\","
                    "\"extra\":true}"),
                Stop});
    const auto Failure = TerminalFailure(Terminal);
    Check(Failure.has_value() &&
              Failure->Code == FailureCode::UnsupportedCapability,
          "refusal unknown field is unsupported");
  }
  {
    const std::string EndTurnWithDetails =
        "event: message_delta\r\ndata: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
        "{\"input_tokens\":1,\"output_tokens\":1},"
        "\"stop_details\":{\"type\":\"refusal\"}}\r\n\r\n";
    const auto Terminal =
        RunRaw(Provider, Transport, {Start, EndTurnWithDetails, Stop});
    const auto Failure = TerminalFailure(Terminal);
    Check(Failure.has_value() &&
              Failure->Code == FailureCode::InvalidStream,
          "non-refusal stop_reason with stop_details is invalid");
  }
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\",\"category\":\"cyber\"}",
      true, "refusal cyber category");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\",\"category\":\"bio\"}",
      true, "refusal bio category");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\","
      "\"category\":\"frontier_llm\"}",
      true, "refusal frontier category");
  CheckRefusal(
      ",\"stop_details\":{\"type\":\"refusal\","
      "\"category\":\"reasoning_extraction\"}",
      true, "refusal reasoning category");
}

void TestEndpointAndHealthMatrix() {
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  auto AnyEndpointProfile = MakeProfile();
  AnyEndpointProfile.Endpoint = "https://ignored.invalid/any-path";
  Check(std::holds_alternative<std::monostate>(
            Provider.ValidateProfile(AnyEndpointProfile)),
        "profile endpoint is not parsed by the adapter");
  auto MissingGrant = MakeBinding();
  MissingGrant.Grant.Value.clear();
  AnthropicMessagesProvider Unbound(Transport, MissingGrant);
  CanonicalTerminalEvent UnboundTerminal;
  Transport.CallCount = 0;
  static_cast<void>(
      RunFrames(Transport, Unbound, MakeRequest(), UnboundTerminal));
  const auto UnboundFailure = TerminalFailure(UnboundTerminal);
  Check(UnboundFailure.has_value() &&
            UnboundFailure->Code == FailureCode::TransportFailure &&
            !UnboundFailure->Retryable &&
            UnboundFailure->SanitizedDescription.find("EndpointNotGranted") !=
                std::string::npos &&
            Transport.CallCount == 0,
        "missing endpoint grant fails before network");
  auto BadDestination = MakeBinding();
  BadDestination.Destination.Path = "v1/messages";
  AnthropicMessagesProvider BadBinding(Transport, BadDestination);
  CanonicalTerminalEvent BadTerminal;
  static_cast<void>(
      RunFrames(Transport, BadBinding, MakeRequest(), BadTerminal));
  Check(TerminalFailure(BadTerminal).has_value() &&
            TerminalFailure(BadTerminal)->SanitizedDescription.find(
                "EndpointNotGranted") != std::string::npos,
        "invalid endpoint destination fails closed");
  auto MissingModel = MakeProfile();
  MissingModel.ModelName.clear();
  Check(std::get<ProviderError>(Provider.ValidateProfile(MissingModel)).Code ==
            FailureCode::InvalidProfile,
        "missing model rejected");
  auto MissingVersion = MakeProfile();
  MissingVersion.ProtocolVersion.clear();
  Check(
      std::get<ProviderError>(Provider.ValidateProfile(MissingVersion)).Code ==
          FailureCode::InvalidProfile,
      "missing anthropic version rejected");
  Check(std::get<ProviderError>(
            Provider.GetCapabilities(ProviderProfileId{})).Code ==
            FailureCode::InvalidProfile,
        "empty capability profile rejected");
  const auto Health = Provider.HealthCheck(MakeProfile());
  Check(std::holds_alternative<ProviderHealthStatus>(Health) &&
            std::get<ProviderHealthStatus>(Health).State ==
                ProviderHealthState::Available,
        "health check available");
  auto Invalid = MakeProfile();
  Invalid.Protocol = ProviderProtocol::OpenAIResponses;
  Check(std::holds_alternative<ProviderError>(Provider.HealthCheck(Invalid)),
        "health check invalid profile");
}

void TestSanitizationVersionAndSequences() {
  FakeHttpTransport HttpTransport;
  HttpTransport.Head.StatusCode = 401;
  HttpTransport.Result =
      CompletedTransport{.StatusCode = 401, .ReceivedBodyBytes = 0};
  HttpTransport.Frames = {
      "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
      "\"message\":\"SECRET-CANARY-http\"}}"};
  AnthropicMessagesProvider HttpProvider = MakeProvider(HttpTransport);
  CanonicalTerminalEvent HttpTerminal;
  static_cast<void>(
      RunFrames(HttpTransport, HttpProvider, MakeRequest(), HttpTerminal));
  const auto HttpFailure = TerminalFailure(HttpTerminal);
  Check(HttpFailure.has_value() &&
            HttpFailure->SanitizedDescription.find("SECRET-CANARY") ==
                std::string::npos &&
            HttpFailure->SanitizedDescription == "Authentication failed.",
        "http error body never leaks provider text");

  FakeHttpTransport InBandTransport;
  InBandTransport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_canary\",\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":0}}}\r\n\r\n",
      "event: content_block_start\r\ndata: {\"type\":"
      "\"content_block_start\",\"index\":0,\"content_block\":"
      "{\"type\":\"text\",\"text\":\"\"}}\r\n\r\n",
      "event: content_block_delta\r\ndata: {\"type\":"
      "\"content_block_delta\",\"index\":0,\"delta\":"
      "{\"type\":\"text_delta\",\"text\":\"partial\"}}\r\n\r\n",
      "event: error\r\ndata: {\"type\":\"error\",\"error\":"
      "{\"type\":\"overloaded_error\",\"message\":"
      "\"SECRET-CANARY-inband\"}}\r\n\r\n",
  };
  AnthropicMessagesProvider InBandProvider = MakeProvider(InBandTransport);
  CanonicalTerminalEvent InBandTerminal;
  static_cast<void>(RunFrames(InBandTransport, InBandProvider, MakeRequest(),
                              InBandTerminal));
  const auto InBandFailure = TerminalFailure(InBandTerminal);
  Check(InBandFailure.has_value() &&
            InBandFailure->Code == FailureCode::ProviderRejected &&
            !InBandFailure->Retryable &&
            InBandFailure->SanitizedDescription.find("SECRET-CANARY") ==
                std::string::npos,
        "in-band error sanitized and not retryable");

  FakeHttpTransport VersionTransport;
  AnthropicMessagesProvider VersionProvider = MakeProvider(VersionTransport);
  auto BadVersion = MakeProfile();
  BadVersion.ProtocolVersion = "2025-01-01";
  CanonicalTerminalEvent VersionTerminal;
  static_cast<void>(
      RunFrames(VersionTransport, VersionProvider, MakeRequest(),
                VersionTerminal, BadVersion));
  Check(TerminalFailure(VersionTerminal).has_value() &&
            TerminalFailure(VersionTerminal)->Code ==
                FailureCode::InvalidProfile &&
            VersionTransport.CallCount == 0,
        "unsupported Anthropic version rejected");

  FakeHttpTransport SequenceTransport;
  SequenceTransport.Frames = {
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_seq\",\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":0}}}\r\n\r\n",
      "event: content_block_start\r\ndata: {\"type\":"
      "\"content_block_start\",\"index\":0,\"content_block\":"
      "{\"type\":\"text\",\"text\":\"\"}}\r\n\r\n",
      "event: content_block_delta\r\ndata: {\"type\":"
      "\"content_block_delta\",\"index\":0,\"delta\":"
      "{\"type\":\"text_delta\",\"text\":\"partial\"}}\r\n\r\n",
  };
  SequenceTransport.Result =
      FailedTransport{.Code = TransportFailureCode::DnsFailure};
  AnthropicMessagesProvider SequenceProvider = MakeProvider(SequenceTransport);
  CanonicalTerminalEvent SequenceTerminal;
  static_cast<void>(RunFrames(SequenceTransport, SequenceProvider,
                              MakeRequest(), SequenceTerminal));
  const auto* SequenceFailure =
      std::get_if<ResponseFailedEvent>(&SequenceTerminal);
  Check(SequenceFailure != nullptr &&
            SequenceFailure->Metadata.SequenceNumber == 4 &&
            SequenceFailure->Failure.Retryable,
        "transport failure sequence follows emitted events");
}

FailureCode FailureCodeFromFixture(std::string_view Name) {
  if (Name == "InvalidProfile") return FailureCode::InvalidProfile;
  if (Name == "InvalidRequest") return FailureCode::InvalidRequest;
  if (Name == "UnsupportedCapability")
    return FailureCode::UnsupportedCapability;
  if (Name == "AuthenticationFailure")
    return FailureCode::AuthenticationFailure;
  if (Name == "RateLimited") return FailureCode::RateLimited;
  if (Name == "TransportFailure") return FailureCode::TransportFailure;
  if (Name == "ProviderRejected") return FailureCode::ProviderRejected;
  if (Name == "ContentFiltered") return FailureCode::ContentFiltered;
  return FailureCode::InvalidStream;
}

bool UsageMatches(const json::object& Expected,
                  const std::optional<UsageSnapshot>& Actual) {
  if (!Actual.has_value()) {
    return false;
  }
  const auto NumberEquals =
      [](const json::value& Value,
         const std::optional<std::uint64_t>& ActualValue) {
        if (!ActualValue.has_value()) {
          return false;
        }
        if (Value.is_uint64()) {
          return *ActualValue == Value.get_uint64();
        }
        if (Value.is_int64() && Value.get_int64() >= 0) {
          return *ActualValue ==
                 static_cast<std::uint64_t>(Value.get_int64());
        }
        return false;
      };
  if (const auto* Input = Expected.if_contains("InputTokens");
      Input != nullptr && !NumberEquals(*Input, Actual->InputTokens)) {
    return false;
  }
  if (const auto* Output = Expected.if_contains("OutputTokens");
      Output != nullptr && !NumberEquals(*Output, Actual->OutputTokens)) {
    return false;
  }
  if (const auto* CacheWrite = Expected.if_contains("CachedInputWriteTokens");
      CacheWrite != nullptr &&
      !NumberEquals(*CacheWrite, Actual->CachedInputWriteTokens)) {
    return false;
  }
  if (const auto* CacheRead = Expected.if_contains("CachedInputReadTokens");
      CacheRead != nullptr &&
      !NumberEquals(*CacheRead, Actual->CachedInputReadTokens)) {
    return false;
  }
  return true;
}

RefusalCategory RefusalCategoryFromFixture(std::string_view Name) {
  if (Name == "GeneralHarms" || Name == "general_harms")
    return RefusalCategory::GeneralHarms;
  if (Name == "Cyber" || Name == "cyber") return RefusalCategory::Cyber;
  if (Name == "Bio" || Name == "bio") return RefusalCategory::Bio;
  if (Name == "FrontierLlm" || Name == "frontier_llm")
    return RefusalCategory::FrontierLlm;
  if (Name == "ReasoningExtraction" || Name == "reasoning_extraction")
    return RefusalCategory::ReasoningExtraction;
  return RefusalCategory::Unclassified;
}

bool TerminalMatches(const json::object& Expected,
                     const CanonicalTerminalEvent& Terminal) {
  const auto& Type = Expected.at("EventType").as_string();
  if (Type == "ResponseFailed") {
    const auto* Failed = std::get_if<ResponseFailedEvent>(&Terminal);
    if (Failed == nullptr) {
      return false;
    }
    const auto& Payload = Expected.at("Payload").as_object();
    const auto* FailureCodeValue = Payload.if_contains("ErrorCode");
    if (FailureCodeValue == nullptr) {
      FailureCodeValue = Payload.if_contains("FailureCode");
    }
    if (FailureCodeValue == nullptr ||
        FailureCodeFromFixture(FailureCodeValue->as_string().c_str()) !=
        Failed->Failure.Code) {
      return false;
    }
    if (const auto* Retryable = Payload.if_contains("Retryable");
        Retryable != nullptr &&
        Retryable->as_bool() != Failed->Failure.Retryable) {
      return false;
    }
    return true;
  }
  if (Type == "ResponseCompleted") {
    const auto* Completed = std::get_if<ResponseCompletedEvent>(&Terminal);
    if (Completed == nullptr) {
      return false;
    }
    const auto& Payload = Expected.at("Payload").as_object();
    if (const auto Kind = Payload.if_contains("CompletionKind")) {
      const std::string_view KindName = Kind->as_string().c_str();
      const bool Match =
          (KindName == "FinalAnswer" &&
           Completed->Kind == CompletionKind::FinalAnswer) ||
          (KindName == "ToolHandoff" &&
           Completed->Kind == CompletionKind::ToolHandoff) ||
          (KindName == "Refusal" &&
           Completed->Kind == CompletionKind::Refusal);
      if (!Match) {
        return false;
      }
    }
    if (const auto* Usage = Payload.if_contains("FinalUsageSnapshot")) {
      if (!UsageMatches(Usage->as_object(), Completed->FinalUsage)) {
        return false;
      }
    }
    if (const auto* Sequence = Payload.if_contains("StopSequenceCode");
        Sequence != nullptr &&
        (!Completed->StopSequenceCode.has_value() ||
         Completed->StopSequenceCode->Value !=
             Sequence->as_string().c_str())) {
      return false;
    }
    if (const auto* StopDetails = Payload.if_contains("StopDetails");
        StopDetails != nullptr && StopDetails->is_object()) {
      if (!Completed->StopDetails.has_value()) {
        return false;
      }
      if (const auto& Category =
              StopDetails->as_object().at("Category").as_string();
          RefusalCategoryFromFixture(Category.c_str()) !=
          Completed->StopDetails->Category) {
        return false;
      }
    }
    return true;
  }
  if (Type == "ResponseOutputLimited") {
    const auto* Limited = std::get_if<ResponseOutputLimitedEvent>(&Terminal);
    if (Limited == nullptr) {
      return false;
    }
    const auto& Payload = Expected.at("Payload").as_object();
    if (const auto* Kind = Payload.if_contains("LimitKind")) {
      const std::string_view KindName = Kind->as_string().c_str();
      if ((KindName == "OutputTokens") !=
          (Limited->Kind == LimitKind::OutputTokens)) {
        return false;
      }
    }
    if (const auto* Usage = Payload.if_contains("UsageSnapshot")) {
      if (!UsageMatches(Usage->as_object(), Limited->Usage)) {
        return false;
      }
    }
    return true;
  }
  if (Type == "ResponseOutcomeUnknown") {
    const auto* Unknown = std::get_if<ResponseOutcomeUnknownEvent>(&Terminal);
    return Unknown != nullptr &&
           Unknown->Reason.Code.Value ==
               Expected.at("Payload")
                   .as_object()
                   .at("ReasonCode")
                   .as_string()
                   .c_str();
  }
  if (Type == "ResponseCancelled") {
    const auto* Cancelled = std::get_if<ResponseCancelledEvent>(&Terminal);
    return Cancelled != nullptr &&
           Cancelled->Reason.Code.Value ==
               Expected.at("Payload")
                   .as_object()
                   .at("ReasonCode")
                   .as_string()
                   .c_str();
  }
  return false;
}

const json::object* FindCaseObject(const json::value& Root,
                                   std::string_view CaseId) {
  for (const auto& Value : Root.as_object().at("Cases").as_array()) {
    const auto& Object = Value.as_object();
    if (Object.at("CaseId").as_string() == CaseId) {
      return &Object;
    }
  }
  return nullptr;
}

CanonicalTerminalEvent RunFixtureCase(
    FakeHttpTransport& Transport, AnthropicMessagesProvider& Provider,
    const json::object& Case) {
  const std::string CaseId = Case.at("CaseId").as_string().c_str();
  std::string RequestId = "req_" + CaseId;
  std::string TurnId = "turn_" + CaseId;
  if (const auto* Id = Case.if_contains("RequestId");
      Id != nullptr && Id->is_string()) {
    RequestId = Id->as_string().c_str();
  }
  if (const auto* Id = Case.if_contains("AgentTurnId");
      Id != nullptr && Id->is_string()) {
    TurnId = Id->as_string().c_str();
  }
  if (const auto* Frames = Case.if_contains("WireFrames");
      Frames != nullptr) {
    for (const auto& Frame : Frames->as_array()) {
      Transport.Frames.emplace_back(Frame.as_string().c_str());
    }
  }
  if (const auto* Response = Case.if_contains("HttpResponse");
      Response != nullptr) {
    const auto& ResponseObject = Response->as_object();
    const auto Status = static_cast<std::uint16_t>(
        ResponseObject.at("Status").as_int64());
    Transport.Head.StatusCode = Status;
    Transport.Result = CompletedTransport{.StatusCode = Status,
                                          .ReceivedBodyBytes = 0};
    Transport.Frames = {json::serialize(ResponseObject.at("Body"))};
  }
  std::stop_source StopSource;
  if (const auto* Trigger = Case.if_contains("Trigger");
      Trigger != nullptr && Trigger->is_string() &&
      Trigger->as_string() == "LocalCancellationBeforeNetworkSend") {
    StopSource.request_stop();
  }
  if (CaseId ==
      "cancellation_after_send_without_confirmation_is_unknown") {
    Transport.Result =
        CancelledTransport{.RequestMayHaveReachedEndpoint = true};
  }
  CanonicalTerminalEvent Terminal;
  static_cast<void>(RunFrames(Transport, Provider,
                              MakeRequest(RequestId, TurnId), Terminal,
                              MakeProfile(), StopSource.get_token()));
  return Terminal;
}

bool RunCanonicalRequestCase(const json::object& Case) {
  const std::string CaseId = Case.at("CaseId").as_string().c_str();
  FakeHttpTransport Transport;
  AnthropicMessagesProvider Provider = MakeProvider(Transport);
  auto Request = MakeRequest("req_" + CaseId, "turn_" + CaseId);
  const auto ValidFrames = std::vector<std::string>{
      "event: message_start\r\ndata: {\"type\":\"message_start\","
      "\"message\":{\"id\":\"msg_req\",\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":0}}}\r\n\r\n",
      "event: message_delta\r\ndata: {\"type\":\"message_delta\","
      "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
      "{\"input_tokens\":1,\"output_tokens\":1}}\r\n\r\n",
      "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n"};
  Transport.Frames = ValidFrames;
  bool ExpectReady = false;
  if (CaseId == "nondefault_temperature_rejected_before_send") {
    Request.Sampling.Temperature = 0.5;
  } else if (CaseId == "nondefault_top_p_rejected_before_send") {
    Request.Sampling.TopP = 0.5;
  } else if (CaseId == "nondefault_top_k_rejected_before_send") {
    Request.Sampling.TopK = 1.0;
  } else if (CaseId == "tool_use_tool_result_roundtrip_preserves_ids_and_order") {
    Request.Messages.clear();
    Request.Messages.push_back(CanonicalMessage{
        .Role = MessageRole::User,
        .ContentParts = {TextContentPart{.Text = "Run checks."}},
        .FunctionCalls = {},
        .ToolCallResultId = std::nullopt});
    Request.Messages.push_back(CanonicalMessage{
        .Role = MessageRole::Assistant,
        .ContentParts = {TextContentPart{.Text = "Running."}},
        .FunctionCalls = {FunctionCall{
            .Id = ToolCallId{.Value = "toolu_roundtrip"},
            .Name = "SyntheticLookup",
            .Arguments = JsonValue{.JsonText = "{\"query\":\"alpha\"}"}}},
        .ToolCallResultId = std::nullopt});
    Request.Messages.push_back(CanonicalMessage{
        .Role = MessageRole::Tool,
        .ContentParts = {TextContentPart{.Text = "Result."}},
        .FunctionCalls = {},
        .ToolCallResultId = ToolCallId{.Value = "toolu_roundtrip"}});
    Request.Messages.push_back(CanonicalMessage{
        .Role = MessageRole::User,
        .ContentParts = {TextContentPart{.Text = "Continue."}},
        .FunctionCalls = {},
        .ToolCallResultId = std::nullopt});
    ExpectReady = true;
  } else if (CaseId == "tool_result_after_text_rejected") {
    Request.Messages.clear();
    Request.Messages.push_back(CanonicalMessage{
        .Role = MessageRole::User,
        .ContentParts = {TextContentPart{.Text = "No tool call before this."}},
        .FunctionCalls = {},
        .ToolCallResultId = std::nullopt});
    Request.Messages.push_back(CanonicalMessage{
        .Role = MessageRole::Tool,
        .ContentParts = {TextContentPart{.Text = "Late result."}},
        .FunctionCalls = {},
        .ToolCallResultId = ToolCallId{.Value = "toolu_late"}});
  } else {
    ExpectReady = true;
  }
  CanonicalTerminalEvent Terminal;
  static_cast<void>(
      RunFrames(Transport, Provider, Request, Terminal));
  const auto& Expected = Case.at("Expected").as_object();
  const std::string Outcome = Expected.at("Outcome").as_string().c_str();
  if (Outcome == "RequestReady") {
    return ExpectReady && Transport.CallCount == 1;
  }
  const auto Failure = TerminalFailure(Terminal);
  return Failure.has_value() &&
         Failure->Code ==
             FailureCodeFromFixture(
                 Expected.at("ErrorCode").as_string().c_str()) &&
         (!Expected.if_contains("Retryable") ||
          Failure->Retryable ==
              Expected.at("Retryable").as_bool()) &&
         Transport.CallCount == 0;
}

void TestFixtureIndexConformance() {
  const auto Index = LoadFixture("AnthropicMessages/Fixtures/FixtureIndex.json")
                         .as_object();
  int Executed = 0;
  std::vector<std::string> Seen;
  for (const auto& Matrix : Index.at("CoverageMatrix").as_array()) {
    const auto& Entry = Matrix.as_object();
    const std::string File = Entry.at("File").as_string().c_str();
    if (File == "All four files") {
      continue;
    }
    const auto Root = LoadFixture("AnthropicMessages/Fixtures/" + File);
    for (const auto& CaseIdValue : Entry.at("CaseIds").as_array()) {
      const std::string CaseId = CaseIdValue.as_string().c_str();
      const std::string Key = File + "|" + CaseId;
      if (std::find(Seen.begin(), Seen.end(), Key) != Seen.end()) {
        continue;
      }
      Seen.push_back(Key);
      const auto* Case = FindCaseObject(Root, CaseId);
      Check(Case != nullptr, "fixture case exists " + CaseId);
      if (Case == nullptr) {
        continue;
      }
      ++Executed;
      if (File == "CanonicalRequest.json") {
        Check(RunCanonicalRequestCase(*Case),
              "fixture request outcome " + CaseId);
        continue;
      }
      FakeHttpTransport Transport;
      AnthropicMessagesProvider Provider = MakeProvider(Transport);
      const auto Terminal = RunFixtureCase(Transport, Provider, *Case);
      if (const auto* Expected = Case->if_contains("ExpectedTerminal");
          Expected != nullptr && !Expected->is_null()) {
        Check(TerminalMatches(Expected->as_object(), Terminal),
              "fixture terminal " + CaseId);
      } else {
        const auto* Completed =
            std::get_if<ResponseCompletedEvent>(&Terminal);
        const auto Kind = CaseId == "tool_use_partial_json_and_stable_ids"
                              ? CompletionKind::ToolHandoff
                              : CompletionKind::FinalAnswer;
        Check(Completed != nullptr && Completed->Kind == Kind,
              "fixture stream terminal " + CaseId);
      }
    }
  }
  Check(Executed == 24, "all 24 FixtureIndex cases executed");

  const auto Refusal = LoadFixture(
      "AnthropicMessages/Fixtures/Version1Point2/RefusalCases.json");
  int RefusalExecuted = 0;
  for (const auto& CaseValue : Refusal.as_object().at("Cases").as_array()) {
    const auto& Case = CaseValue.as_object();
    const std::string CaseId = Case.at("CaseId").as_string().c_str();
    FakeHttpTransport Transport;
    AnthropicMessagesProvider Provider = MakeProvider(Transport);
    const auto Terminal = RunFixtureCase(Transport, Provider, Case);
    ++RefusalExecuted;
    Check(TerminalMatches(Case.at("ExpectedTerminal").as_object(), Terminal),
          "V1.2 refusal fixture " + CaseId);
  }
  Check(RefusalExecuted == 12, "all 12 V1.2 refusal cases executed");
}

} // namespace

int main() {
  try {
    TestCapabilitiesAndProfile();
    TestWireRequestAndHeaders();
    TestSamplingRejectedBeforeSend();
    TestTextStreamFragmentation();
    TestToolStream();
    TestOutputLimits();
    TestStopSequence();
    TestEarlyFailures();
    TestUnexpectedEof();
    TestCancellation();
    TestHttpErrorMapping();
    TestRefusalV12();
    TestDuplicateTerminal();
    TestThinkingFiltered();
    TestRequestValidationMatrix();
    TestSuccessfulRequestVariants();
    TestSseMalformedMatrix();
    TestTransportTerminalMatrix();
    TestRefusalVariantMatrix();
    TestEndpointAndHealthMatrix();
    TestSanitizationVersionAndSequences();
    TestFixtureIndexConformance();
  } catch (const std::exception& Error) {
    std::cerr << "UNHANDLED: " << Error.what() << '\n';
    return 1;
  }
  if (Failures != 0) {
    std::cerr << Failures << " checks failed\n";
    return 1;
  }
  std::cout << "All AnthropicMessagesProvider checks passed\n";
  return 0;
}
