#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace EmulativeDialog::Platform::Network {

template <typename Tag>
struct Identifier final {
    std::string Value;

    friend bool operator==(const Identifier&, const Identifier&) = default;
};

struct EndpointGrantIdTag final {};
struct TransportRequestIdTag final {};

// Identifies an immutable Core endpoint grant. Only Core endpoint policy may
// mint grants; resolvers never accept arbitrary user-supplied URLs.
using EndpointGrantId = Identifier<EndpointGrantIdTag>;
using TransportRequestId = Identifier<TransportRequestIdTag>;

// Only the methods needed by registered model providers are modeled. Adding
// verbs requires a contract revision.
enum class TransportMethod : std::uint8_t {
    Post
};

// A fully resolved HTTPS destination derived from a Core endpoint grant.
// Host carries a DNS name or IP literal without scheme or userinfo; Path is an
// absolute path with an optional query. Redirects never change either field.
struct HttpsDestination final {
    std::string Host;
    std::uint16_t Port{443};
    std::string Path;

    friend bool operator==(const HttpsDestination&, const HttpsDestination&) =
        default;
};

// Wire header values are bounded request data. They are never surfaced in
// terminal results, diagnostics, events, or task logs.
struct TransportHeader final {
    std::string Name;
    std::string Value;
};

struct TransportRequestLimits final {
    std::uint64_t MaximumRequestBytes{8U * 1024U * 1024U};
    std::uint64_t MaximumResponseBytes{64U * 1024U * 1024U};
    std::uint64_t MaximumChunkBytes{256U * 1024U};
    std::uint64_t MaximumHeaderBytes{64U * 1024U};
    std::uint32_t MaximumHeaderCount{64U};
};

struct TransportTimeouts final {
    std::chrono::milliseconds Connect{10'000};
    std::chrono::milliseconds Send{30'000};
    std::chrono::milliseconds Response{300'000};
    std::chrono::milliseconds Idle{60'000};
};

struct TransportRequest final {
    TransportRequestId Id;
    EndpointGrantId Grant;
    TransportMethod Method{TransportMethod::Post};
    HttpsDestination Destination;
    std::vector<TransportHeader> Headers;
    std::string BodyUtf8;
    TransportRequestLimits Limits;
    TransportTimeouts Timeouts;
};

enum class TransportFailureCode : std::uint8_t {
    InvalidRequest,
    EndpointNotGranted,
    DnsFailure,
    ConnectFailure,
    TlsHandshakeFailure,
    ProxyAuthenticationRequired,
    RequestWriteFailure,
    ResponseReadFailure,
    ResponseLimitExceeded,
    HeaderLimitExceeded,
    InvalidHttpResponse,
    InternalError
};

enum class TransportTimeoutPhase : std::uint8_t {
    Connect,
    Send,
    Response,
    Idle
};

// Allowlisted response metadata only. Raw headers and bodies never leave the
// transport boundary.
struct ResponseHeadMetadata final {
    std::uint16_t StatusCode{0};
    std::string ContentType;
    std::optional<std::chrono::seconds> RetryAfter;
};

struct CompletedTransport final {
    std::uint16_t StatusCode{0};
    std::uint64_t ReceivedBodyBytes{0};
};

struct FailedTransport final {
    TransportFailureCode Code{TransportFailureCode::InternalError};
};

struct RedirectDeniedTransport final {
    std::uint16_t StatusCode{0};
    std::string RedactedLocationHost;
};

struct TimedOutTransport final {
    TransportTimeoutPhase Phase{TransportTimeoutPhase::Response};
};

// Cancellation before any request byte is written is confirmed not sent.
// Cancellation after request bytes may have reached the endpoint cannot claim
// that remote work stopped; callers must not blind-retry.
struct CancelledTransport final {
    bool RequestMayHaveReachedEndpoint{false};
};

enum class UnknownOutcomeReason : std::uint8_t {
    StreamInterruptedAfterRequestSent
};

struct UnknownOutcomeTransport final {
    UnknownOutcomeReason Reason{UnknownOutcomeReason::StreamInterruptedAfterRequestSent};
    bool RequestMayHaveReachedEndpoint{true};
};

using TransportTerminal =
    std::variant<CompletedTransport, FailedTransport, RedirectDeniedTransport,
                 TimedOutTransport, CancelledTransport, UnknownOutcomeTransport>;

// Chunk contents are only valid for the duration of the callback; consumers
// must copy any bytes they retain.
using ResponseChunkSink =
    std::function<void(std::span<const std::byte> Chunk)>;
using ResponseHeadSink =
    std::function<void(const ResponseHeadMetadata& Head)>;

enum class TransportHealthState : std::uint8_t {
    Ready,
    Degraded,
    Unavailable
};

// Single public boundary used by provider adapters. Implementations must never
// accept a full URL string, follow a redirect, load user certificates, or emit
// secrets; see Architecture/Contracts/HttpTransportContract.md V1.
class HttpTransportPort {
public:
    virtual ~HttpTransportPort() = default;

    HttpTransportPort(const HttpTransportPort&) = delete;
    HttpTransportPort& operator=(const HttpTransportPort&) = delete;

    // Streams exactly one HTTPS exchange and returns exactly one terminal
    // result. The head sink runs at most once before body chunks. Sink
    // exceptions are treated as an implementation fault and never propagated
    // as transport success.
    [[nodiscard]] virtual TransportTerminal Stream(
        const TransportRequest& Request,
        const ResponseHeadSink& OnHead,
        const ResponseChunkSink& OnChunk,
        std::stop_token CancellationToken) = 0;

    [[nodiscard]] virtual TransportHealthState Health(
        std::stop_token CancellationToken) noexcept = 0;

protected:
    HttpTransportPort() = default;
};

} // namespace EmulativeDialog::Platform::Network
