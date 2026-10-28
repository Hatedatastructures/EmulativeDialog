#include "Source/Platform/Network/HttpTransportPort.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace EmulativeDialog::Platform::Network;

template <typename Type>
concept ExposesUrlField = requires(const Type& Value) {
    Value.Url;
};

template <typename Type>
concept ExposesAuthorizationHeaderField = requires(const Type& Value) {
    Value.AuthorizationHeader;
};

template <typename Type>
concept ExposesRawBodyField = requires(const Type& Value) {
    Value.RawBody;
};

// AC-3: API shape itself can be checked at compile time. No full URL, raw
// authorization, or raw response body is part of the public types.
static_assert(!ExposesUrlField<TransportRequest>);
static_assert(!ExposesAuthorizationHeaderField<TransportRequest>);
static_assert(!ExposesRawBodyField<CompletedTransport>);
static_assert(!ExposesRawBodyField<FailedTransport>);
static_assert(!ExposesRawBodyField<UnknownOutcomeTransport>);
static_assert(sizeof(TransportMethod) == 1);

TransportRequest MakeRequest(std::string Body = "{}") {
    return TransportRequest{
        .Id = TransportRequestId{std::string{"transport-request-1"}},
        .Grant = EndpointGrantId{std::string{"endpoint-grant-synthetic"}},
        .Method = TransportMethod::Post,
        .Destination =
            HttpsDestination{
                .Host = "provider.example",
                .Port = 443,
                .Path = "/v1/responses"},
        .Headers =
            {TransportHeader{.Name = "Content-Type",
                             .Value = "application/json"}},
        .BodyUtf8 = std::move(Body),
        .Limits = TransportRequestLimits{},
        .Timeouts = TransportTimeouts{}};
}

// Offline fake used to prove substitutability without any network access.
class ScriptedTransport final : public HttpTransportPort {
public:
    enum class Script {
        NormalTwoChunks,
        Redirect,
        TlsFailure,
        ResponseTimeout,
        CancelledBeforeSend,
        CancelledMidStream,
        StreamInterrupted,
        HeaderInjectionRejected,
        OversizedResponse
    };

    explicit ScriptedTransport(Script Selected) : Current(Selected) {}

    [[nodiscard]] TransportTerminal Stream(
        const TransportRequest& Request,
        const ResponseHeadSink& OnHead,
        const ResponseChunkSink& OnChunk,
        std::stop_token) override {
        ++CallCount;
        LastDestination = Request.Destination;
        LastBody = Request.BodyUtf8;

        const ResponseHeadMetadata Head{
            .StatusCode = 200,
            .ContentType = "text/event-stream",
            .RetryAfter = std::nullopt};

        switch (Current) {
        case Script::NormalTwoChunks: {
            if (OnHead) {
                OnHead(Head);
            }
            const std::string First = "data: {\"part\":1}\n\n";
            const std::string Second = "data: [DONE]\n\n";
            Emit(OnChunk, First);
            Emit(OnChunk, Second);
            ReceivedBodyBytes = First.size() + Second.size();
            return CompletedTransport{.StatusCode = 200,
                                      .ReceivedBodyBytes =
                                          ReceivedBodyBytes};
        }
        case Script::Redirect:
            return RedirectDeniedTransport{
                .StatusCode = 307,
                .RedactedLocationHost = "other.example"};
        case Script::TlsFailure:
            return FailedTransport{
                .Code = TransportFailureCode::TlsHandshakeFailure};
        case Script::ResponseTimeout:
            return TimedOutTransport{.Phase = TransportTimeoutPhase::Response};
        case Script::CancelledBeforeSend:
            return CancelledTransport{
                .RequestMayHaveReachedEndpoint = false};
        case Script::CancelledMidStream:
            if (OnHead) {
                OnHead(Head);
            }
            Emit(OnChunk, "data: {\"part\":1}\n\n");
            return CancelledTransport{.RequestMayHaveReachedEndpoint = true};
        case Script::StreamInterrupted:
            if (OnHead) {
                OnHead(Head);
            }
            Emit(OnChunk, "data: {\"part\":1}\n\n");
            return UnknownOutcomeTransport{
                .Reason =
                    UnknownOutcomeReason::StreamInterruptedAfterRequestSent,
                .RequestMayHaveReachedEndpoint = true};
        case Script::HeaderInjectionRejected: {
            bool Injected = false;
            for (const TransportHeader& Header : Request.Headers) {
                Injected = Injected ||
                           Header.Name.find('\r') != std::string::npos ||
                           Header.Name.find('\n') != std::string::npos ||
                           Header.Value.find('\r') != std::string::npos ||
                           Header.Value.find('\n') != std::string::npos;
            }
            if (Injected) {
                return FailedTransport{
                    .Code = TransportFailureCode::InvalidRequest};
            }
            return CompletedTransport{.StatusCode = 200,
                                      .ReceivedBodyBytes = 0};
        }
        case Script::OversizedResponse:
            return FailedTransport{
                .Code = TransportFailureCode::ResponseLimitExceeded};
        }
        return FailedTransport{.Code = TransportFailureCode::InternalError};
    }

    [[nodiscard]] TransportHealthState Health(std::stop_token) noexcept override {
        return TransportHealthState::Ready;
    }

    int CallCount{0};
    HttpsDestination LastDestination;
    std::string LastBody;
    std::uint64_t ReceivedBodyBytes{0};

private:
    static void Emit(const ResponseChunkSink& Sink, std::string_view Text) {
        if (!Sink) {
            return;
        }
        Sink(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(Text.data()), Text.size()));
    }

    Script Current;
};

struct CapturedBody final {
    std::vector<std::string> Chunks;
    int HeadCalls{0};
    ResponseHeadMetadata Head;

    [[nodiscard]] std::size_t TotalBytes() const {
        std::size_t Total = 0;
        for (const std::string& Chunk : Chunks) {
            Total += Chunk.size();
        }
        return Total;
    }
};

CapturedBody Capture(ScriptedTransport& Transport,
                     const TransportRequest& Request,
                     std::stop_token Token = {}) {
    CapturedBody Captured;
    const TransportTerminal Terminal = Transport.Stream(
        Request,
        [](const ResponseHeadMetadata&) {},
        [&Captured](std::span<const std::byte> Chunk) {
            Captured.Chunks.emplace_back(
                reinterpret_cast<const char*>(Chunk.data()), Chunk.size());
        },
        Token);
    static_cast<void>(Terminal);
    return Captured;
}

void TestOfflineFakeSubstitution() {
    ScriptedTransport Transport(ScriptedTransport::Script::NormalTwoChunks);
    const TransportRequest Request = MakeRequest();
    std::vector<std::string> Chunks;
    ResponseHeadMetadata CapturedHead;
    int HeadCalls = 0;

    const TransportTerminal Terminal = Transport.Stream(
        Request,
        [&CapturedHead, &HeadCalls](const ResponseHeadMetadata& Head) {
            CapturedHead = Head;
            ++HeadCalls;
        },
        [&Chunks](std::span<const std::byte> Chunk) {
            Chunks.emplace_back(
                reinterpret_cast<const char*>(Chunk.data()), Chunk.size());
        },
        std::stop_token{});

    assert(Transport.CallCount == 1);
    assert(HeadCalls == 1);
    assert(CapturedHead.StatusCode == 200);
    assert(CapturedHead.ContentType == "text/event-stream");
    assert(Chunks.size() == 2);
    assert(std::holds_alternative<CompletedTransport>(Terminal));
    const auto& Completed = std::get<CompletedTransport>(Terminal);
    assert(Completed.StatusCode == 200);
    assert(Completed.ReceivedBodyBytes ==
           Chunks.front().size() + Chunks.back().size());
    assert(Transport.LastDestination.Host == "provider.example");
    assert(Transport.LastDestination.Path == "/v1/responses");
    assert(Transport.Health(std::stop_token{}) ==
           TransportHealthState::Ready);
}

void TestRedirectDenied() {
    ScriptedTransport Transport(ScriptedTransport::Script::Redirect);
    const TransportTerminal Terminal =
        Transport.Stream(MakeRequest(), {}, {}, std::stop_token{});
    assert(std::holds_alternative<RedirectDeniedTransport>(Terminal));
    const auto& Redirect = std::get<RedirectDeniedTransport>(Terminal);
    assert(Redirect.StatusCode == 307);
    assert(Redirect.RedactedLocationHost == "other.example");
}

void TestTlsFailureFailsClosed() {
    ScriptedTransport Transport(ScriptedTransport::Script::TlsFailure);
    const TransportTerminal Terminal =
        Transport.Stream(MakeRequest(), {}, {}, std::stop_token{});
    assert(std::holds_alternative<FailedTransport>(Terminal));
    assert(std::get<FailedTransport>(Terminal).Code ==
           TransportFailureCode::TlsHandshakeFailure);
}

void TestTimeoutIsDistinctFromUnknown() {
    ScriptedTransport Timeout(ScriptedTransport::Script::ResponseTimeout);
    const TransportTerminal Timed =
        Timeout.Stream(MakeRequest(), {}, {}, std::stop_token{});
    assert(std::holds_alternative<TimedOutTransport>(Timed));
    assert(std::get<TimedOutTransport>(Timed).Phase ==
           TransportTimeoutPhase::Response);

    ScriptedTransport Interrupted(
        ScriptedTransport::Script::StreamInterrupted);
    const TransportTerminal Unknown =
        Interrupted.Stream(MakeRequest(), {}, {}, std::stop_token{});
    assert(std::holds_alternative<UnknownOutcomeTransport>(Unknown));
    assert(std::get<UnknownOutcomeTransport>(Unknown)
               .RequestMayHaveReachedEndpoint);
}

void TestCancellationBeforeSendIsConfirmedUnsent() {
    ScriptedTransport Transport(
        ScriptedTransport::Script::CancelledBeforeSend);
    const TransportTerminal Terminal =
        Transport.Stream(MakeRequest(), {}, {}, std::stop_token{});
    assert(std::holds_alternative<CancelledTransport>(Terminal));
    assert(
        !std::get<CancelledTransport>(Terminal)
             .RequestMayHaveReachedEndpoint);
}

void TestCancellationMidStreamCannotClaimRemoteStop() {
    ScriptedTransport Transport(
        ScriptedTransport::Script::CancelledMidStream);
    const TransportTerminal Terminal =
        Transport.Stream(MakeRequest(), {}, {}, std::stop_token{});
    assert(std::holds_alternative<CancelledTransport>(Terminal));
    assert(std::get<CancelledTransport>(Terminal)
               .RequestMayHaveReachedEndpoint);
}

void TestHeaderInjectionRejected() {
    ScriptedTransport Transport(
        ScriptedTransport::Script::HeaderInjectionRejected);
    TransportRequest Request = MakeRequest();
    Request.Headers.push_back(
        TransportHeader{.Name = "X-Test",
                        .Value = "value\r\nX-Injected: true"});
    const TransportTerminal Terminal =
        Transport.Stream(Request, {}, {}, std::stop_token{});
    assert(std::holds_alternative<FailedTransport>(Terminal));
    assert(std::get<FailedTransport>(Terminal).Code ==
           TransportFailureCode::InvalidRequest);
}

void TestOversizedResponseMapsToLimitFailure() {
    ScriptedTransport Transport(
        ScriptedTransport::Script::OversizedResponse);
    TransportRequest Request = MakeRequest();
    Request.Limits.MaximumResponseBytes = 16;
    const TransportTerminal Terminal =
        Transport.Stream(Request, {}, {}, std::stop_token{});
    assert(std::holds_alternative<FailedTransport>(Terminal));
    assert(std::get<FailedTransport>(Terminal).Code ==
           TransportFailureCode::ResponseLimitExceeded);
}

void TestChunkCaptureContract() {
    ScriptedTransport Transport(ScriptedTransport::Script::NormalTwoChunks);
    CapturedBody Captured = Capture(Transport, MakeRequest("{\"input\":1}"));
    assert(Captured.Chunks.size() == 2);
    assert(Captured.TotalBytes() > 0);
}

} // namespace

int main() {
    TestOfflineFakeSubstitution();
    TestRedirectDenied();
    TestTlsFailureFailsClosed();
    TestTimeoutIsDistinctFromUnknown();
    TestCancellationBeforeSendIsConfirmedUnsent();
    TestCancellationMidStreamCannotClaimRemoteStop();
    TestHeaderInjectionRejected();
    TestOversizedResponseMapsToLimitFailure();
    TestChunkCaptureContract();
    return 0;
}
