#include "Source/Platform/Network/WinHttpTransport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace EmulativeDialog::Platform::Network;

static_assert(!std::is_constructible_v<WinHttpTransport,
                                      EndpointGrantResolver&&, WinHttpApi&>);
static_assert(std::is_constructible_v<WinHttpTransport,
                                     const EndpointGrantResolver&,
                                     WinHttpApi&>);
static_assert(!std::is_constructible_v<WinHttpTransport,
                                      const EndpointGrantResolver&&,
                                      WinHttpApi&>);

template <typename Resolver>
constexpr bool CanCreateSystemTransport = requires(Resolver&& Grants) {
    CreateSystemWinHttpTransport(std::forward<Resolver>(Grants));
};

static_assert(CanCreateSystemTransport<EndpointGrantResolver&>);
static_assert(CanCreateSystemTransport<const EndpointGrantResolver&>);
static_assert(!CanCreateSystemTransport<EndpointGrantResolver>);
static_assert(!CanCreateSystemTransport<const EndpointGrantResolver>);

void Expect(bool Condition) {
    if (!Condition) {
        std::abort();
    }
}

class FixedGrantResolver final : public EndpointGrantResolver {
public:
    explicit FixedGrantResolver(EndpointGrantStatus Selected)
        : Current(Selected) {}

    [[nodiscard]] EndpointGrantStatus Validate(
        const EndpointGrantId&,
        const HttpsDestination&) const noexcept override {
        ++CallCount;
        return Current;
    }

    mutable int CallCount{0};

private:
    EndpointGrantStatus Current;
};

struct FakeScript final {
    std::uint32_t OpenSessionError{0};
    std::uint32_t SetTimeoutsError{0};
    std::uint32_t DisableRedirectsError{0};
    std::uint32_t ConnectError{0};
    std::uint32_t OpenRequestError{0};
    std::uint32_t AddHeaderError{0};
    std::uint32_t SendError{0};
    std::uint32_t ReceiveError{0};
    std::uint32_t StatusError{0};
    std::uint32_t QueryDataError{0};
    std::uint32_t ReadError{0};
    std::uint16_t StatusCode{200};
    std::optional<std::string> Location;
    std::optional<std::string> ContentType{"text/event-stream"};
    std::optional<std::string> ContentEncoding;
    std::optional<std::string> RetryAfter;
    std::vector<std::string> BodyChunks;
    bool HttpDateParses{true};
    std::int64_t HttpDateSecondsFromNow{0};
};

class FakeWinHttpApi final : public WinHttpApi {
public:
    explicit FakeWinHttpApi(FakeScript Selected) : Script(std::move(Selected)) {}

    [[nodiscard]] WinHttpHandle OpenSession(
        TransportProxyMode Proxy, std::uint32_t& ErrorCode) override {
        ++OpenSessionCalls;
        SeenProxy = Proxy;
        if (Script.OpenSessionError != 0) {
            ErrorCode = Script.OpenSessionError;
            return nullptr;
        }
        return Session;
    }

    [[nodiscard]] bool SetTimeouts(
        WinHttpHandle, const TransportTimeouts& Timeouts,
        std::uint32_t& ErrorCode) override {
        ++SetTimeoutCalls;
        SeenTimeouts = Timeouts;
        if (Script.SetTimeoutsError != 0) {
            ErrorCode = Script.SetTimeoutsError;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool DisableRedirects(
        WinHttpHandle, std::uint32_t& ErrorCode) override {
        ++DisableRedirectCalls;
        if (Script.DisableRedirectsError != 0) {
            ErrorCode = Script.DisableRedirectsError;
            return false;
        }
        return true;
    }

    [[nodiscard]] WinHttpHandle Connect(
        WinHttpHandle, std::string_view Host, std::uint16_t Port,
        std::uint32_t& ErrorCode) override {
        ++ConnectCalls;
        SeenHost = std::string(Host);
        SeenPort = Port;
        if (Script.ConnectError != 0) {
            ErrorCode = Script.ConnectError;
            return nullptr;
        }
        return Connection;
    }

    [[nodiscard]] WinHttpHandle OpenRequest(
        WinHttpHandle, std::string_view Path,
        std::uint32_t& ErrorCode) override {
        ++OpenRequestCalls;
        SeenPath = std::string(Path);
        if (Script.OpenRequestError != 0) {
            ErrorCode = Script.OpenRequestError;
            return nullptr;
        }
        return Request;
    }

    [[nodiscard]] bool AddHeader(
        WinHttpHandle, std::string_view Name, std::string_view Value,
        std::uint32_t& ErrorCode) override {
        ++AddHeaderCalls;
        SeenHeaders.emplace_back(std::string(Name), std::string(Value));
        if (Script.AddHeaderError != 0) {
            ErrorCode = Script.AddHeaderError;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool SendRequest(
        WinHttpHandle, std::string_view Body,
        std::uint32_t& ErrorCode) override {
        ++SendCalls;
        SeenBody = std::string(Body);
        if (Script.SendError != 0) {
            ErrorCode = Script.SendError;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ReceiveResponse(
        WinHttpHandle, std::uint32_t& ErrorCode) override {
        ++ReceiveCalls;
        if (Script.ReceiveError != 0) {
            ErrorCode = Script.ReceiveError;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool QueryStatusCode(
        WinHttpHandle, std::uint16_t& StatusCode,
        std::uint32_t& ErrorCode) override {
        ++StatusCalls;
        if (Script.StatusError != 0) {
            ErrorCode = Script.StatusError;
            return false;
        }
        StatusCode = Script.StatusCode;
        return true;
    }

    [[nodiscard]] bool QueryHeader(
        WinHttpHandle, std::string_view Name, std::string& Value,
        std::uint32_t& ErrorCode) override {
        ++QueryHeaderCalls;
        const std::string Lower = ToLower(Name);
        const std::optional<std::string>* Source = nullptr;
        if (Lower == "location") {
            Source = &Script.Location;
        } else if (Lower == "content-type") {
            Source = &Script.ContentType;
        } else if (Lower == "content-encoding") {
            Source = &Script.ContentEncoding;
        } else if (Lower == "retry-after") {
            Source = &Script.RetryAfter;
        }
        if (Source == nullptr || !Source->has_value()) {
            ErrorCode = WinHttpErrorHeaderNotFound;
            return false;
        }
        Value = **Source;
        return true;
    }

    [[nodiscard]] bool QueryDataAvailable(
        WinHttpHandle, std::uint32_t& AvailableBytes,
        std::uint32_t& ErrorCode) override {
        ++QueryDataCalls;
        if (Script.QueryDataError != 0) {
            ErrorCode = Script.QueryDataError;
            return false;
        }
        if (ChunkIndex >= Script.BodyChunks.size()) {
            AvailableBytes = 0;
            return true;
        }
        AvailableBytes = static_cast<std::uint32_t>(
            Script.BodyChunks[ChunkIndex].size() - ChunkOffset);
        return true;
    }

    [[nodiscard]] bool ReadData(
        WinHttpHandle, std::span<std::byte> Buffer,
        std::uint32_t& BytesRead, std::uint32_t& ErrorCode) override {
        ++ReadCalls;
        if (Script.ReadError != 0) {
            ErrorCode = Script.ReadError;
            return false;
        }
        if (ChunkIndex >= Script.BodyChunks.size()) {
            BytesRead = 0;
            return true;
        }
        const std::string& Chunk = Script.BodyChunks[ChunkIndex];
        const std::size_t Remaining = Chunk.size() - ChunkOffset;
        const std::size_t Count = std::min(Buffer.size(), Remaining);
        for (std::size_t Index = 0; Index < Count; ++Index) {
            Buffer[Index] = static_cast<std::byte>(
                static_cast<unsigned char>(Chunk[ChunkOffset + Index]));
        }
        ChunkOffset += Count;
        if (ChunkOffset == Chunk.size()) {
            ++ChunkIndex;
            ChunkOffset = 0;
        }
        BytesRead = static_cast<std::uint32_t>(Count);
        return true;
    }

    [[nodiscard]] bool HttpDateToUnixSeconds(
        std::string_view, std::int64_t& UnixSeconds) override {
        ++HttpDateCalls;
        if (!Script.HttpDateParses) {
            return false;
        }
        UnixSeconds = std::chrono::system_clock::to_time_t(
                          std::chrono::system_clock::now()) +
                      Script.HttpDateSecondsFromNow;
        return true;
    }

    void CloseHandle(WinHttpHandle Handle) noexcept override {
        ++CloseCalls;
        if (Handle == Session) {
            ++SessionCloseCalls;
        } else if (Handle == Connection) {
            ++ConnectionCloseCalls;
        } else if (Handle == Request) {
            ++RequestCloseCalls;
        }
    }

    int OpenSessionCalls{0};
    int SetTimeoutCalls{0};
    int DisableRedirectCalls{0};
    int ConnectCalls{0};
    int OpenRequestCalls{0};
    int AddHeaderCalls{0};
    int SendCalls{0};
    int ReceiveCalls{0};
    int StatusCalls{0};
    int QueryHeaderCalls{0};
    int QueryDataCalls{0};
    int ReadCalls{0};
    int HttpDateCalls{0};
    int CloseCalls{0};
    int SessionCloseCalls{0};
    int ConnectionCloseCalls{0};
    int RequestCloseCalls{0};
    TransportProxyMode SeenProxy{TransportProxyMode::System};
    TransportTimeouts SeenTimeouts;
    std::string SeenHost;
    std::uint16_t SeenPort{0};
    std::string SeenPath;
    std::string SeenBody;
    std::vector<std::pair<std::string, std::string>> SeenHeaders;

private:
    static std::string ToLower(std::string_view Text) {
        std::string Result;
        Result.reserve(Text.size());
        for (const char Value : Text) {
            Result.push_back(Value >= 'A' && Value <= 'Z'
                                 ? static_cast<char>(Value - 'A' + 'a')
                                 : Value);
        }
        return Result;
    }

    FakeScript Script;
    std::size_t ChunkIndex{0};
    std::size_t ChunkOffset{0};
    void* Session{reinterpret_cast<void*>(0x10)};
    void* Connection{reinterpret_cast<void*>(0x20)};
    void* Request{reinterpret_cast<void*>(0x30)};
};

TransportRequest MakeRequest(std::string Body = "{}") {
    return TransportRequest{
        .Id = TransportRequestId{std::string{"winhttp-request-1"}},
        .Grant = EndpointGrantId{std::string{"grant-1"}},
        .Method = TransportMethod::Post,
        .Destination =
            HttpsDestination{.Host = "provider.example",
                             .Port = 443,
                             .Path = "/v1/responses"},
        .Headers =
            {TransportHeader{.Name = "Content-Type",
                             .Value = "application/json"},
             TransportHeader{.Name = "Authorization",
                             .Value = "Bearer secret-token"}},
        .BodyUtf8 = std::move(Body),
        .Limits = TransportRequestLimits{},
        .Timeouts = TransportTimeouts{}};
}

struct Capture final {
    int HeadCalls{0};
    ResponseHeadMetadata Head;
    std::vector<std::string> Chunks;
};

TransportTerminal RunOkCase(FakeWinHttpApi& Api,
                            const FixedGrantResolver& Grants,
                            const TransportRequest& Request, Capture& Captured,
                            std::stop_token Token = {}) {
    WinHttpTransport Transport(Grants, Api);
    return Transport.Stream(
        Request,
        [&Captured](const ResponseHeadMetadata& Head) {
            ++Captured.HeadCalls;
            Captured.Head = Head;
        },
        [&Captured](std::span<const std::byte> Chunk) {
            Captured.Chunks.emplace_back(
                reinterpret_cast<const char*>(Chunk.data()), Chunk.size());
        },
        Token);
}

FakeScript SuccessScript() {
    FakeScript Script;
    Script.BodyChunks = {"data: {\"part\":1}\n\n", "data: [DONE]\n\n"};
    return Script;
}

void TestSuccessfulBoundedStream() {
    FakeWinHttpApi Api(SuccessScript());
    FixedGrantResolver Grants(EndpointGrantStatus::Valid);
    Capture Captured;
    const TransportRequest Request = MakeRequest("{\"input\":\"hello\"}");
    const TransportTerminal Terminal =
        RunOkCase(Api, Grants, Request, Captured);

    Expect(std::holds_alternative<CompletedTransport>(Terminal));
    const auto& Completed = std::get<CompletedTransport>(Terminal);
    Expect(Completed.StatusCode == 200);
    Expect(Completed.ReceivedBodyBytes ==
           Captured.Chunks.front().size() + Captured.Chunks.back().size());
    Expect(Captured.HeadCalls == 1);
    Expect(Captured.Head.StatusCode == 200);
    Expect(Captured.Head.ContentType == "text/event-stream");
    Expect(Api.OpenSessionCalls == 1);
    Expect(Api.SeenProxy == TransportProxyMode::System);
    Expect(Api.SeenHost == "provider.example");
    Expect(Api.SeenPort == 443);
    Expect(Api.SeenPath == "/v1/responses");
    Expect(Api.SeenBody == "{\"input\":\"hello\"}");
    Expect(Api.SessionCloseCalls == 1);
    Expect(Api.ConnectionCloseCalls == 1);
    Expect(Api.RequestCloseCalls == 1);
}

void TestDirectProxyModeReachesSystemApi() {
    FakeWinHttpApi Api(SuccessScript());
    FixedGrantResolver Grants(EndpointGrantStatus::Valid);
    TransportRequest Request = MakeRequest();
    Request.Proxy = TransportProxyMode::Direct;
    Capture Captured;
    const TransportTerminal Terminal =
        RunOkCase(Api, Grants, Request, Captured);
    Expect(std::holds_alternative<CompletedTransport>(Terminal));
    Expect(Api.SeenProxy == TransportProxyMode::Direct);
}

void TestGrantRejectionNeverTouchesWinHttp() {
    const EndpointGrantStatus Cases[] = {
        EndpointGrantStatus::Missing, EndpointGrantStatus::Expired,
        EndpointGrantStatus::DestinationMismatch};
    for (const EndpointGrantStatus Status : Cases) {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(Status);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::EndpointNotGranted);
        Expect(Grants.CallCount == 1);
        Expect(Api.OpenSessionCalls == 0);
        Expect(Api.SetTimeoutCalls == 0);
        Expect(Api.DisableRedirectCalls == 0);
        Expect(Api.ConnectCalls == 0);
        Expect(Api.OpenRequestCalls == 0);
        Expect(Api.AddHeaderCalls == 0);
        Expect(Api.SendCalls == 0);
        Expect(Api.ReceiveCalls == 0);
        Expect(Api.StatusCalls == 0);
        Expect(Api.QueryHeaderCalls == 0);
        Expect(Api.QueryDataCalls == 0);
        Expect(Api.ReadCalls == 0);
        Expect(Api.HttpDateCalls == 0);
        Expect(Api.CloseCalls == 0);
        Expect(Captured.HeadCalls == 0);
        Expect(Captured.Chunks.empty());
    }
}

void TestTlsFailureFailsClosed() {
    FakeScript Script = SuccessScript();
    Script.ReceiveError = WinHttpErrorSecureFailure;
    FakeWinHttpApi Api(Script);
    FixedGrantResolver Grants(EndpointGrantStatus::Valid);
    Capture Captured;
    const TransportTerminal Terminal =
        RunOkCase(Api, Grants, MakeRequest(), Captured);
    Expect(std::holds_alternative<FailedTransport>(Terminal));
    Expect(std::get<FailedTransport>(Terminal).Code ==
           TransportFailureCode::TlsHandshakeFailure);
    Expect(Captured.HeadCalls == 0);
}

void TestRedirectDeniedWithoutFollowing() {
    FakeScript Script;
    Script.StatusCode = 307;
    Script.Location = "https://other.example/redirected?secret=1";
    FakeWinHttpApi Api(Script);
    FixedGrantResolver Grants(EndpointGrantStatus::Valid);
    Capture Captured;
    const TransportTerminal Terminal =
        RunOkCase(Api, Grants, MakeRequest(), Captured);
    Expect(std::holds_alternative<RedirectDeniedTransport>(Terminal));
    const auto& Redirect = std::get<RedirectDeniedTransport>(Terminal);
    Expect(Redirect.StatusCode == 307);
    Expect(Redirect.RedactedLocationHost == "other.example");
    Expect(Api.QueryDataCalls == 0);
    Expect(Captured.HeadCalls == 0);
}

void TestTimeoutPhases() {
    {
        FakeScript Script = SuccessScript();
        Script.ConnectError = WinHttpErrorTimeout;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<TimedOutTransport>(Terminal));
        Expect(std::get<TimedOutTransport>(Terminal).Phase ==
               TransportTimeoutPhase::Connect);
    }
    {
        FakeScript Script = SuccessScript();
        Script.SendError = WinHttpErrorTimeout;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<TimedOutTransport>(Terminal));
        Expect(std::get<TimedOutTransport>(Terminal).Phase ==
               TransportTimeoutPhase::Send);
    }
    {
        FakeScript Script = SuccessScript();
        Script.ReceiveError = WinHttpErrorTimeout;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<TimedOutTransport>(Terminal));
        Expect(std::get<TimedOutTransport>(Terminal).Phase ==
               TransportTimeoutPhase::Response);
    }
    {
        FakeScript Script = SuccessScript();
        Script.QueryDataError = WinHttpErrorTimeout;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<TimedOutTransport>(Terminal));
        Expect(std::get<TimedOutTransport>(Terminal).Phase ==
               TransportTimeoutPhase::Response);
    }
}

void TestDnsAndConnectFailures() {
    {
        FakeScript Script = SuccessScript();
        Script.ConnectError = WinHttpErrorNameNotResolved;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::DnsFailure);
    }
    {
        FakeScript Script = SuccessScript();
        Script.ConnectError = WinHttpErrorCannotConnect;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::ConnectFailure);
    }
}

void TestCancellationBeforeAndDuringStream() {
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        std::stop_source Source;
        Source.request_stop();
        const TransportTerminal Terminal = RunOkCase(
            Api, Grants, MakeRequest(), Captured, Source.get_token());
        Expect(std::holds_alternative<CancelledTransport>(Terminal));
        Expect(!std::get<CancelledTransport>(Terminal)
                    .RequestMayHaveReachedEndpoint);
        Expect(Api.OpenSessionCalls == 0);
    }
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        std::stop_source Source;
        WinHttpTransport Transport(Grants, Api);
        std::vector<std::string> Chunks;
        const TransportTerminal Terminal = Transport.Stream(
            MakeRequest(),
            [](const ResponseHeadMetadata&) {},
            [&Chunks, &Source](std::span<const std::byte> Chunk) {
                Chunks.emplace_back(
                    reinterpret_cast<const char*>(Chunk.data()),
                    Chunk.size());
                Source.request_stop();
            },
            Source.get_token());
        Expect(std::holds_alternative<CancelledTransport>(Terminal));
        Expect(std::get<CancelledTransport>(Terminal)
                   .RequestMayHaveReachedEndpoint);
        Expect(Chunks.size() == 1);
    }
}

void TestUnknownOutcomesAreNotSuccess() {
    {
        FakeScript Script = SuccessScript();
        Script.QueryDataError = WinHttpErrorConnectionError;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<UnknownOutcomeTransport>(Terminal));
    }
    {
        FakeScript Script = SuccessScript();
        Script.SendError = WinHttpErrorConnectionError;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<UnknownOutcomeTransport>(Terminal));
    }
}

void TestInvalidHeadersAndRequestsAreRejected() {
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Headers.push_back(
            TransportHeader{.Name = "X-Test",
                            .Value = "value\r\nInjected: true"});
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InvalidRequest);
        Expect(Api.OpenSessionCalls == 0);
    }
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Headers.push_back(
            TransportHeader{.Name = "Proxy-Authorization",
                            .Value = "Basic c2VjcmV0"});
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InvalidRequest);
    }
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Destination.Host = "https://provider.example";
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InvalidRequest);
    }
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Destination.Path = "relative/path";
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InvalidRequest);
    }
}

void TestFramingHeadersNeverReachTheWire() {
    FakeWinHttpApi Api(SuccessScript());
    FixedGrantResolver Grants(EndpointGrantStatus::Valid);
    TransportRequest Request = MakeRequest("{\"framed\":true}");
    Request.Headers.push_back(
        TransportHeader{.Name = "Host", .Value = "spoofed.example"});
    Request.Headers.push_back(
        TransportHeader{.Name = "Content-Length", .Value = "999"});
    Capture Captured;
    const TransportTerminal Terminal =
        RunOkCase(Api, Grants, Request, Captured);
    Expect(std::holds_alternative<CompletedTransport>(Terminal));
    for (const auto& [Name, Value] : Api.SeenHeaders) {
        static_cast<void>(Value);
        Expect(Name != "Host");
        Expect(Name != "Content-Length");
    }
    // Anti-tampering: even when an Authorization value is present it never
    // appears in a terminal result.
    const auto& Completed = std::get<CompletedTransport>(Terminal);
    Expect(Completed.StatusCode == 200);
}

void TestHeaderLimits() {
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Limits.MaximumHeaderCount = 1;
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::HeaderLimitExceeded);
    }
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Limits.MaximumHeaderBytes = 8;
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::HeaderLimitExceeded);
    }
    {
        FakeScript Script = SuccessScript();
        Script.ContentType = std::string(200, 'x');
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Limits.MaximumHeaderBytes = 16;
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::HeaderLimitExceeded);
    }
}

void TestResponseLimitsAndChunking() {
    {
        FakeScript Script = SuccessScript();
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Limits.MaximumResponseBytes = 8;
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::ResponseLimitExceeded);
    }
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        TransportRequest Request = MakeRequest();
        Request.Limits.MaximumChunkBytes = 4;
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, Request, Captured);
        Expect(std::holds_alternative<CompletedTransport>(Terminal));
        for (const std::string& Chunk : Captured.Chunks) {
            Expect(Chunk.size() <= 4);
        }
        Expect(Captured.Chunks.size() > 2);
    }
}

void TestCompressionIsRejected() {
    FakeScript Script = SuccessScript();
    Script.ContentEncoding = "gzip";
    FakeWinHttpApi Api(Script);
    FixedGrantResolver Grants(EndpointGrantStatus::Valid);
    Capture Captured;
    const TransportTerminal Terminal =
        RunOkCase(Api, Grants, MakeRequest(), Captured);
    Expect(std::holds_alternative<FailedTransport>(Terminal));
    Expect(std::get<FailedTransport>(Terminal).Code ==
           TransportFailureCode::InvalidHttpResponse);
    Expect(Captured.HeadCalls == 0);
    Expect(Api.QueryDataCalls == 0);
}

void TestRetryAfterNormalization() {
    {
        FakeScript Script = SuccessScript();
        Script.RetryAfter = "120";
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<CompletedTransport>(Terminal));
        Expect(Captured.Head.RetryAfter.has_value());
        Expect(Captured.Head.RetryAfter->count() == 120);
    }
    {
        FakeScript Script = SuccessScript();
        Script.RetryAfter = "999999999";
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<CompletedTransport>(Terminal));
        Expect(Captured.Head.RetryAfter.has_value());
        Expect(Captured.Head.RetryAfter->count() == 24 * 60 * 60);
    }
    {
        FakeScript Script = SuccessScript();
        Script.RetryAfter = "Wed, 21 Oct 2026 07:28:00 GMT";
        Script.HttpDateSecondsFromNow = 50;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<CompletedTransport>(Terminal));
        Expect(Captured.Head.RetryAfter.has_value());
        Expect(Captured.Head.RetryAfter->count() >= 49);
        Expect(Captured.Head.RetryAfter->count() <= 50);
        Expect(Api.HttpDateCalls == 1);
    }
    {
        FakeScript Script = SuccessScript();
        Script.RetryAfter = "invalid-date";
        Script.HttpDateParses = false;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<CompletedTransport>(Terminal));
        Expect(!Captured.Head.RetryAfter.has_value());
    }
}

void TestSessionFailuresAreSanitized() {
    {
        FakeScript Script = SuccessScript();
        Script.OpenSessionError = 5;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InternalError);
    }
    {
        FakeScript Script = SuccessScript();
        Script.SetTimeoutsError = WinHttpErrorTimeout;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<TimedOutTransport>(Terminal));
        Expect(std::get<TimedOutTransport>(Terminal).Phase ==
               TransportTimeoutPhase::Connect);
    }
    {
        FakeScript Script = SuccessScript();
        Script.DisableRedirectsError = 5;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InternalError);
    }
    {
        FakeScript Script = SuccessScript();
        Script.OpenRequestError = 5;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InternalError);
    }
    {
        FakeScript Script = SuccessScript();
        Script.AddHeaderError = 5;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InternalError);
    }
}

void TestSinkExceptionsFailClosed() {
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        WinHttpTransport Transport(Grants, Api);
        const TransportTerminal Terminal = Transport.Stream(
            MakeRequest(),
            [](const ResponseHeadMetadata&) { throw 1; }, {},
            std::stop_token{});
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InternalError);
    }
    {
        FakeWinHttpApi Api(SuccessScript());
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        WinHttpTransport Transport(Grants, Api);
        const TransportTerminal Terminal = Transport.Stream(
            MakeRequest(), {},
            [](std::span<const std::byte>) { throw 1; },
            std::stop_token{});
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InternalError);
    }
}

void TestInvalidResponseShapeAndShortRead() {
    {
        FakeScript Script = SuccessScript();
        Script.ReceiveError = WinHttpErrorInvalidServerResponse;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<FailedTransport>(Terminal));
        Expect(std::get<FailedTransport>(Terminal).Code ==
               TransportFailureCode::InvalidHttpResponse);
    }
    {
        FakeScript Script = SuccessScript();
        Script.ReadError = WinHttpErrorConnectionError;
        FakeWinHttpApi Api(Script);
        FixedGrantResolver Grants(EndpointGrantStatus::Valid);
        Capture Captured;
        const TransportTerminal Terminal =
            RunOkCase(Api, Grants, MakeRequest(), Captured);
        Expect(std::holds_alternative<UnknownOutcomeTransport>(Terminal));
    }
}

void TestHealthReportsAvailability() {
    FakeWinHttpApi Api(SuccessScript());
    FixedGrantResolver Grants(EndpointGrantStatus::Valid);
    WinHttpTransport Transport(Grants, Api);
    Expect(Transport.Health(std::stop_token{}) ==
           TransportHealthState::Ready);
    std::stop_source Source;
    Source.request_stop();
    Expect(Transport.Health(Source.get_token()) ==
           TransportHealthState::Unavailable);
}

} // namespace

int main() {
    TestSuccessfulBoundedStream();
    TestDirectProxyModeReachesSystemApi();
    TestGrantRejectionNeverTouchesWinHttp();
    TestTlsFailureFailsClosed();
    TestRedirectDeniedWithoutFollowing();
    TestTimeoutPhases();
    TestDnsAndConnectFailures();
    TestCancellationBeforeAndDuringStream();
    TestUnknownOutcomesAreNotSuccess();
    TestInvalidHeadersAndRequestsAreRejected();
    TestFramingHeadersNeverReachTheWire();
    TestHeaderLimits();
    TestResponseLimitsAndChunking();
    TestCompressionIsRejected();
    TestRetryAfterNormalization();
    TestSessionFailuresAreSanitized();
    TestSinkExceptionsFailClosed();
    TestInvalidResponseShapeAndShortRead();
    TestHealthReportsAvailability();
    return 0;
}
