#pragma once

#include "Source/Platform/Network/HttpTransportPort.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace EmulativeDialog::Platform::Network {

// Opaque WinHTTP handle. The concrete value is HINTERNET in the system
// implementation; tests substitute a fake without including <winhttp.h>.
using WinHttpHandle = void*;

// Mirrors of the winhttp.h error values that cross the injection boundary.
// Keeping them here lets offline fakes stay platform-header free.
inline constexpr std::uint32_t WinHttpErrorOperationCancelled = 12017U;
inline constexpr std::uint32_t WinHttpErrorTimeout = 12002U;
inline constexpr std::uint32_t WinHttpErrorNameNotResolved = 12007U;
inline constexpr std::uint32_t WinHttpErrorCannotConnect = 12029U;
inline constexpr std::uint32_t WinHttpErrorConnectionError = 12030U;
inline constexpr std::uint32_t WinHttpErrorInvalidServerResponse = 12152U;
inline constexpr std::uint32_t WinHttpErrorHeaderNotFound = 12150U;
inline constexpr std::uint32_t WinHttpErrorSecureFailure = 12175U;

// Narrow WinHTTP function table. Every production call the transport needs is
// represented here so tests can inject scripted failures without any network
// access. Implementations own handle lifetime; the transport only calls
// CloseHandle for handles it received from this table.
class WinHttpApi {
public:
    virtual ~WinHttpApi() = default;

    WinHttpApi(const WinHttpApi&) = delete;
    WinHttpApi& operator=(const WinHttpApi&) = delete;

    [[nodiscard]] virtual WinHttpHandle OpenSession(
        TransportProxyMode Proxy, std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool SetTimeouts(
        WinHttpHandle Session, const TransportTimeouts& Timeouts,
        std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool DisableRedirects(
        WinHttpHandle Session, std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual WinHttpHandle Connect(
        WinHttpHandle Session, std::string_view Host, std::uint16_t Port,
        std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual WinHttpHandle OpenRequest(
        WinHttpHandle Connection, std::string_view Path,
        std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool AddHeader(
        WinHttpHandle Request, std::string_view Name, std::string_view Value,
        std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool SendRequest(
        WinHttpHandle Request, std::string_view Body,
        std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool ReceiveResponse(
        WinHttpHandle Request, std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool QueryStatusCode(
        WinHttpHandle Request, std::uint16_t& StatusCode,
        std::uint32_t& ErrorCode) = 0;
    // Returns false and sets ErrorCode to WinHttpErrorHeaderNotFound when the
    // header is absent; other failures are transport errors.
    [[nodiscard]] virtual bool QueryHeader(
        WinHttpHandle Request, std::string_view Name, std::string& Value,
        std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool QueryDataAvailable(
        WinHttpHandle Request, std::uint32_t& AvailableBytes,
        std::uint32_t& ErrorCode) = 0;
    [[nodiscard]] virtual bool ReadData(
        WinHttpHandle Request, std::span<std::byte> Buffer,
        std::uint32_t& BytesRead, std::uint32_t& ErrorCode) = 0;
    // Parses an RFC 7231 HTTP-date into Unix seconds.
    [[nodiscard]] virtual bool HttpDateToUnixSeconds(
        std::string_view HttpDate, std::int64_t& UnixSeconds) = 0;
    virtual void CloseHandle(WinHttpHandle Handle) noexcept = 0;

protected:
    WinHttpApi() = default;
};

// Windows-native HttpTransportPort implementation. It uses WinHTTP with the
// system Schannel trust store; no OpenSSL, curl, loopback listener, automatic
// redirect or unregistered URL is reachable through this class.
class WinHttpTransport final : public HttpTransportPort {
public:
    WinHttpTransport(const EndpointGrantResolver& Grants, WinHttpApi& Api);
    ~WinHttpTransport() override;

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    [[nodiscard]] TransportTerminal Stream(
        const TransportRequest& Request, const ResponseHeadSink& OnHead,
        const ResponseChunkSink& OnChunk,
        std::stop_token CancellationToken) override;

    [[nodiscard]] TransportHealthState Health(
        std::stop_token CancellationToken) noexcept override;

private:
    const EndpointGrantResolver& GrantResolver;
    WinHttpApi& Api;
};

// Production factory wiring the system WinHTTP implementation. The caller owns
// the returned port and must keep Grants alive for its lifetime.
[[nodiscard]] std::unique_ptr<HttpTransportPort> CreateSystemWinHttpTransport(
    const EndpointGrantResolver& Grants);

} // namespace EmulativeDialog::Platform::Network
