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

// Profile-level proxy policy. System uses the WinHTTP/system configuration;
// Direct explicitly bypasses any proxy. No per-request proxy URL, PAC source,
// SOCKS endpoint, or proxy credential is representable.
enum class TransportProxyMode : std::uint8_t {
    System,
    Direct
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
    TransportProxyMode Proxy{TransportProxyMode::System};
};

// Result of Core-side grant validation. Every value other than Valid maps to
// the single public transport failure EndpointNotGranted.
enum class EndpointGrantStatus : std::uint8_t {
    Valid,
    Missing,
    Expired,
    DestinationMismatch
};

// Read-only dependency injected into an HttpTransport implementation at
// construction. Core owns grants; the transport only asks whether a grant is
// still valid for the exact requested destination. Validation must complete
// before DNS resolution or connect is attempted.
class EndpointGrantResolver {
public:
    virtual ~EndpointGrantResolver() = default;

    EndpointGrantResolver(const EndpointGrantResolver&) = delete;
    EndpointGrantResolver& operator=(const EndpointGrantResolver&) = delete;

    [[nodiscard]] virtual EndpointGrantStatus Validate(
        const EndpointGrantId& Grant,
        const HttpsDestination& Requested) const noexcept = 0;

protected:
    EndpointGrantResolver() = default;
};

constexpr char LowerAscii(char Value) noexcept {
    return (Value >= 'A' && Value <= 'Z')
               ? static_cast<char>(Value - 'A' + 'a')
               : Value;
}

constexpr bool EqualsAsciiNoCase(std::string_view Left,
                                 std::string_view Right) noexcept {
    if (Left.size() != Right.size()) {
        return false;
    }
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        if (LowerAscii(Left[Index]) != LowerAscii(Right[Index])) {
            return false;
        }
    }
    return true;
}

// Adapter owns the protocol/profile header allowlist. The transport boundary
// additionally rejects hop-by-hop headers that would let callers influence
// connection management; CR/LF injection and size/count limits are separate
// transport responsibilities.
constexpr bool IsForbiddenHopByHopHeader(std::string_view Name) noexcept {
    return EqualsAsciiNoCase(Name, "connection") ||
           EqualsAsciiNoCase(Name, "keep-alive") ||
           EqualsAsciiNoCase(Name, "proxy-connection") ||
           EqualsAsciiNoCase(Name, "proxy-authorization") ||
           EqualsAsciiNoCase(Name, "te") ||
           EqualsAsciiNoCase(Name, "trailer") ||
           EqualsAsciiNoCase(Name, "transfer-encoding") ||
           EqualsAsciiNoCase(Name, "upgrade");
}

// Content framing is transport-owned. Adapters must not be able to spoof the
// authority or body length; the transport derives Host from HttpsDestination
// and Content-Length from BodyUtf8 and ignores adapter-supplied values.
constexpr bool IsAdapterForbiddenHeader(std::string_view Name) noexcept {
    return IsForbiddenHopByHopHeader(Name) ||
           EqualsAsciiNoCase(Name, "host") ||
           EqualsAsciiNoCase(Name, "content-length");
}

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

// Single public boundary used by provider adapters. Implementations are
// constructed with a Core-provided read-only EndpointGrantResolver and must
// validate the grant before DNS/connect. Implementations must never accept a
// full URL string, follow a redirect, load user certificates, or emit secrets;
// see Architecture/Contracts/HttpTransportContract.md V1.
class HttpTransportPort {
public:
    // GrantResolver is borrowed and must outlive this port. Taking an lvalue
    // reference rejects temporary resolver objects at every derived boundary.
    explicit HttpTransportPort(const EndpointGrantResolver& GrantResolver)
        : GrantResolver(GrantResolver) {}
    HttpTransportPort(EndpointGrantResolver&&) = delete;
    HttpTransportPort(const EndpointGrantResolver&&) = delete;

    virtual ~HttpTransportPort() = default;

    HttpTransportPort(const HttpTransportPort&) = delete;
    HttpTransportPort& operator=(const HttpTransportPort&) = delete;

    // Streams exactly one HTTPS exchange and returns exactly one terminal
    // result. The head sink runs at most once before body chunks. Sink
    // exceptions are treated as an implementation fault and never propagated
    // as transport success.
    // This non-virtual entry is the only public request path. Implementations
    // provide DoStream after the shared endpoint grant guard succeeds.
    [[nodiscard]] TransportTerminal Stream(
        const TransportRequest& Request,
        const ResponseHeadSink& OnHead,
        const ResponseChunkSink& OnChunk,
        std::stop_token CancellationToken) {
        if (GrantResolver.Validate(Request.Grant, Request.Destination) !=
            EndpointGrantStatus::Valid) {
            return FailedTransport{
                .Code = TransportFailureCode::EndpointNotGranted};
        }
        return DoStream(Request, OnHead, OnChunk, CancellationToken);
    }

    [[nodiscard]] virtual TransportHealthState Health(
        std::stop_token CancellationToken) noexcept = 0;

protected:
    [[nodiscard]] virtual TransportTerminal DoStream(
        const TransportRequest& Request,
        const ResponseHeadSink& OnHead,
        const ResponseChunkSink& OnChunk,
        std::stop_token CancellationToken) = 0;

private:
    const EndpointGrantResolver& GrantResolver;
};

} // namespace EmulativeDialog::Platform::Network
