#include "Source/Platform/Network/WinHttpTransport.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace EmulativeDialog::Platform::Network {
static TransportTerminal ExecuteWinHttpExchange(
    WinHttpApi& Api, const TransportRequest& Request,
    const ResponseHeadSink& OnHead, const ResponseChunkSink& OnChunk,
    std::stop_token CancellationToken);

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::int64_t MaximumRetryAfterSeconds = 24 * 60 * 60;

enum class RequestValidation {
    Ok,
    Invalid,
    HeaderLimitExceeded
};

enum class FailureStage {
    Session,
    Connect,
    Send,
    Receive,
    Read
};

struct HandleGuard final {
    HandleGuard(WinHttpApi& InApi, WinHttpHandle InHandle)
        : Api(InApi), Handle(InHandle) {}

    WinHttpApi& Api;
    WinHttpHandle Handle;

    ~HandleGuard() {
        if (Handle != nullptr) {
            Api.CloseHandle(Handle);
        }
    }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

bool ContainsCrLf(std::string_view Text) {
    return Text.find('\r') != std::string_view::npos ||
           Text.find('\n') != std::string_view::npos;
}

bool IsValidHost(std::string_view Host) {
    if (Host.empty() ||
        Host.find("://") != std::string_view::npos ||
        Host.find('@') != std::string_view::npos) {
        return false;
    }
    for (const char Value : Host) {
        const auto Byte = static_cast<unsigned char>(Value);
        if (Byte <= 0x20U || Byte == 0x7FU || Value == '/' || Value == '\\') {
            return false;
        }
    }
    return true;
}

bool HasPositiveTimeouts(const TransportTimeouts& Timeouts) {
    return Timeouts.Connect.count() > 0 && Timeouts.Send.count() > 0 &&
           Timeouts.Response.count() > 0 && Timeouts.Idle.count() > 0;
}

bool HasPositiveLimits(const TransportRequestLimits& Limits) {
    return Limits.MaximumRequestBytes > 0 &&
           Limits.MaximumResponseBytes > 0 &&
           Limits.MaximumChunkBytes > 0 &&
           Limits.MaximumHeaderBytes > 0 && Limits.MaximumHeaderCount > 0;
}

RequestValidation ValidateRequest(const TransportRequest& Request) {
    if (!IsValidHost(Request.Destination.Host) ||
        Request.Destination.Port == 0 ||
        Request.Destination.Path.empty() ||
        Request.Destination.Path.front() != '/' ||
        ContainsCrLf(Request.Destination.Path) ||
        Request.BodyUtf8.size() > Request.Limits.MaximumRequestBytes ||
        !HasPositiveLimits(Request.Limits) ||
        !HasPositiveTimeouts(Request.Timeouts)) {
        return RequestValidation::Invalid;
    }

    if (Request.Headers.size() > Request.Limits.MaximumHeaderCount) {
        return RequestValidation::HeaderLimitExceeded;
    }

    std::uint64_t HeaderBytes = 0;
    for (const TransportHeader& Header : Request.Headers) {
        if (Header.Name.empty() || ContainsCrLf(Header.Name) ||
            ContainsCrLf(Header.Value)) {
            return RequestValidation::Invalid;
        }
        // Hop-by-hop and proxy credential headers are never accepted. Host and
        // Content-Length are framing headers that the transport owns; they are
        // ignored below rather than rejected so adapters cannot spoof them.
        if (IsForbiddenHopByHopHeader(Header.Name)) {
            return RequestValidation::Invalid;
        }
        HeaderBytes += Header.Name.size() + Header.Value.size() + 2;
        if (HeaderBytes > Request.Limits.MaximumHeaderBytes) {
            return RequestValidation::HeaderLimitExceeded;
        }
    }
    return RequestValidation::Ok;
}

TransportTerminal Failed(TransportFailureCode Code) {
    return FailedTransport{.Code = Code};
}

TransportTerminal Unknown() {
    return UnknownOutcomeTransport{
        .Reason = UnknownOutcomeReason::StreamInterruptedAfterRequestSent,
        .RequestMayHaveReachedEndpoint = true};
}

TransportTerminal MapFailure(std::uint32_t ErrorCode, FailureStage Stage) {
    if (ErrorCode == WinHttpErrorOperationCancelled) {
        const bool MayHaveReached =
            Stage == FailureStage::Send || Stage == FailureStage::Receive ||
            Stage == FailureStage::Read;
        return CancelledTransport{
            .RequestMayHaveReachedEndpoint = MayHaveReached};
    }
    if (ErrorCode == WinHttpErrorTimeout) {
        if (Stage == FailureStage::Session || Stage == FailureStage::Connect) {
            return TimedOutTransport{.Phase = TransportTimeoutPhase::Connect};
        }
        if (Stage == FailureStage::Send) {
            return TimedOutTransport{.Phase = TransportTimeoutPhase::Send};
        }
        return TimedOutTransport{.Phase = TransportTimeoutPhase::Response};
    }
    if (ErrorCode == WinHttpErrorNameNotResolved) {
        return Failed(TransportFailureCode::DnsFailure);
    }
    if (ErrorCode == WinHttpErrorCannotConnect) {
        return Failed(TransportFailureCode::ConnectFailure);
    }
    if (ErrorCode == WinHttpErrorSecureFailure) {
        return Failed(TransportFailureCode::TlsHandshakeFailure);
    }
    if (Stage == FailureStage::Session) {
        return Failed(TransportFailureCode::InternalError);
    }
    if (Stage == FailureStage::Connect) {
        return Failed(TransportFailureCode::ConnectFailure);
    }
    if (Stage == FailureStage::Receive &&
        ErrorCode == WinHttpErrorInvalidServerResponse) {
        return Failed(TransportFailureCode::InvalidHttpResponse);
    }
    return Unknown();
}

std::string_view TrimAscii(std::string_view Text) {
    while (!Text.empty() &&
           (Text.front() == ' ' || Text.front() == '\t')) {
        Text.remove_prefix(1);
    }
    while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t')) {
        Text.remove_suffix(1);
    }
    return Text;
}

std::string ToLowerAscii(std::string_view Text) {
    std::string Result;
    Result.reserve(Text.size());
    for (const char Value : Text) {
        Result.push_back(LowerAscii(Value));
    }
    return Result;
}

std::string ExtractLocationHost(std::string_view Location) {
    Location = TrimAscii(Location);
    if (Location.empty()) {
        return {};
    }
    if (const auto Scheme = Location.find("://");
        Scheme != std::string_view::npos) {
        Location.remove_prefix(Scheme + 3);
    } else if (Location.starts_with("//")) {
        Location.remove_prefix(2);
    } else {
        return {};
    }
    if (const auto End = Location.find_first_of("/?#");
        End != std::string_view::npos) {
        Location = Location.substr(0, End);
    }
    if (const auto UserInfo = Location.rfind('@');
        UserInfo != std::string_view::npos) {
        Location.remove_prefix(UserInfo + 1);
    }
    if (const auto PortSeparator = Location.rfind(':');
        PortSeparator != std::string_view::npos) {
        Location = Location.substr(0, PortSeparator);
    }
    if (!IsValidHost(Location)) {
        return {};
    }
    return std::string(Location);
}

std::optional<std::chrono::seconds> NormalizeRetryAfter(
    std::string_view Raw, WinHttpApi& Api,
    std::chrono::system_clock::time_point Now) {
    Raw = TrimAscii(Raw);
    if (Raw.empty() || Raw.size() > 128) {
        return std::nullopt;
    }

    bool AllDigits = true;
    for (const char Value : Raw) {
        if (Value < '0' || Value > '9') {
            AllDigits = false;
            break;
        }
    }
    if (AllDigits) {
        std::uint64_t Seconds = 0;
        for (const char Value : Raw) {
            const std::uint64_t Digit =
                static_cast<std::uint64_t>(Value - '0');
            if (Seconds >
                (std::numeric_limits<std::uint64_t>::max() - Digit) / 10U) {
                return std::chrono::seconds(MaximumRetryAfterSeconds);
            }
            Seconds = Seconds * 10U + Digit;
        }
        if (Seconds >
            static_cast<std::uint64_t>(MaximumRetryAfterSeconds)) {
            return std::chrono::seconds(MaximumRetryAfterSeconds);
        }
        return std::chrono::seconds(Seconds);
    }

    std::int64_t UnixSeconds = 0;
    if (!Api.HttpDateToUnixSeconds(Raw, UnixSeconds)) {
        return std::nullopt;
    }
    const std::int64_t NowSeconds =
        std::chrono::system_clock::to_time_t(Now);
    std::int64_t Delta = UnixSeconds - NowSeconds;
    if (Delta < 0) {
        Delta = 0;
    }
    if (Delta > MaximumRetryAfterSeconds) {
        Delta = MaximumRetryAfterSeconds;
    }
    return std::chrono::seconds(Delta);
}

std::size_t BoundedReadSize(std::uint32_t Available,
                            std::uint64_t ChunkLimit,
                            std::uint64_t RemainingBudget) {
    const auto Chunk =
        static_cast<std::uint64_t>(Available) < ChunkLimit
            ? static_cast<std::uint64_t>(Available)
            : ChunkLimit;
    const std::uint64_t Bounded = std::min(Chunk, RemainingBudget);
    return static_cast<std::size_t>(Bounded);
}

// The system binding is compiled out of the offline contract build so the
// measured production translation unit covers transport semantics only. The
// production build keeps it and uses WinHTTP with the system Schannel store.
#if defined(_WIN32) && !defined(EMULATIVE_DIALOG_WINHTTP_OFFLINE_TESTS)

std::wstring Utf8ToWide(std::string_view Text) {
    if (Text.empty()) {
        return {};
    }
    const int Size = MultiByteToWideChar(
        CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
    if (Size <= 0) {
        return {};
    }
    std::wstring Result(static_cast<std::size_t>(Size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, Text.data(),
                        static_cast<int>(Text.size()), Result.data(), Size);
    return Result;
}

std::string WideToUtf8(std::wstring_view Text) {
    if (Text.empty()) {
        return {};
    }
    const int Size = WideCharToMultiByte(
        CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0,
        nullptr, nullptr);
    if (Size <= 0) {
        return {};
    }
    std::string Result(static_cast<std::size_t>(Size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, Text.data(),
                        static_cast<int>(Text.size()), Result.data(), Size,
                        nullptr, nullptr);
    return Result;
}

int TimeoutMilliseconds(std::chrono::milliseconds Value) {
    const auto Count = Value.count();
    if (Count <= 0) {
        return 1;
    }
    if (Count > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(Count);
}

class SystemWinHttpApi final : public WinHttpApi {
public:
    [[nodiscard]] WinHttpHandle OpenSession(
        TransportProxyMode Proxy, std::uint32_t& ErrorCode) override {
        const DWORD AccessType =
            Proxy == TransportProxyMode::Direct
                ? WINHTTP_ACCESS_TYPE_NO_PROXY
                : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
        const HINTERNET Session =
            WinHttpOpen(L"EmulativeDialog/1.0", AccessType,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (Session == nullptr) {
            ErrorCode = GetLastError();
            return nullptr;
        }
        return Session;
    }

    [[nodiscard]] bool SetTimeouts(
        WinHttpHandle Session, const TransportTimeouts& Timeouts,
        std::uint32_t& ErrorCode) override {
        const int Resolve = TimeoutMilliseconds(Timeouts.Connect);
        const int Connect = TimeoutMilliseconds(Timeouts.Connect);
        const int Send = TimeoutMilliseconds(Timeouts.Send);
        const int Receive = TimeoutMilliseconds(Timeouts.Idle);
        if (!WinHttpSetTimeouts(Session, Resolve, Connect, Send, Receive)) {
            ErrorCode = GetLastError();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool DisableRedirects(
        WinHttpHandle Session, std::uint32_t& ErrorCode) override {
        DWORD Policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(Session, WINHTTP_OPTION_REDIRECT_POLICY, &Policy,
                              sizeof(Policy))) {
            ErrorCode = GetLastError();
            return false;
        }
        return true;
    }

    [[nodiscard]] WinHttpHandle Connect(
        WinHttpHandle Session, std::string_view Host, std::uint16_t Port,
        std::uint32_t& ErrorCode) override {
        const std::wstring WideHost = Utf8ToWide(Host);
        if (WideHost.empty()) {
            ErrorCode = ERROR_INVALID_PARAMETER;
            return nullptr;
        }
        const HINTERNET Connection =
            WinHttpConnect(Session, WideHost.c_str(), Port, 0);
        if (Connection == nullptr) {
            ErrorCode = GetLastError();
            return nullptr;
        }
        return Connection;
    }

    [[nodiscard]] WinHttpHandle OpenRequest(
        WinHttpHandle Connection, std::string_view Path,
        std::uint32_t& ErrorCode) override {
        const std::wstring WidePath = Utf8ToWide(Path);
        if (WidePath.empty()) {
            ErrorCode = ERROR_INVALID_PARAMETER;
            return nullptr;
        }
        const HINTERNET Request = WinHttpOpenRequest(
            Connection, L"POST", WidePath.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (Request == nullptr) {
            ErrorCode = GetLastError();
            return nullptr;
        }
        return Request;
    }

    [[nodiscard]] bool AddHeader(
        WinHttpHandle Request, std::string_view Name, std::string_view Value,
        std::uint32_t& ErrorCode) override {
        std::string Line;
        Line.reserve(Name.size() + Value.size() + 2);
        Line.append(Name);
        Line.append(": ");
        Line.append(Value);
        const std::wstring Wide = Utf8ToWide(Line);
        if (Wide.empty()) {
            ErrorCode = ERROR_INVALID_PARAMETER;
            return false;
        }
        if (!WinHttpAddRequestHeaders(
                Request, Wide.c_str(), static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            ErrorCode = GetLastError();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool SendRequest(
        WinHttpHandle Request, std::string_view Body,
        std::uint32_t& ErrorCode) override {
        LPVOID BodyPointer = WINHTTP_NO_REQUEST_DATA;
        DWORD BodyLength = 0;
        if (!Body.empty()) {
            BodyPointer = const_cast<char*>(Body.data());
            BodyLength = static_cast<DWORD>(Body.size());
        }
        if (!WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                BodyPointer, BodyLength, BodyLength, 0)) {
            ErrorCode = GetLastError();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ReceiveResponse(
        WinHttpHandle Request, std::uint32_t& ErrorCode) override {
        if (!WinHttpReceiveResponse(Request, nullptr)) {
            ErrorCode = GetLastError();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool QueryStatusCode(
        WinHttpHandle Request, std::uint16_t& StatusCode,
        std::uint32_t& ErrorCode) override {
        DWORD Status = 0;
        DWORD Size = sizeof(Status);
        if (!WinHttpQueryHeaders(
                Request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &Status, &Size,
                WINHTTP_NO_HEADER_INDEX)) {
            ErrorCode = GetLastError();
            return false;
        }
        StatusCode = static_cast<std::uint16_t>(Status);
        return true;
    }

    [[nodiscard]] bool QueryHeader(
        WinHttpHandle Request, std::string_view Name, std::string& Value,
        std::uint32_t& ErrorCode) override {
        const std::wstring WideName = Utf8ToWide(Name);
        if (WideName.empty()) {
            ErrorCode = ERROR_INVALID_PARAMETER;
            return false;
        }
        DWORD Size = 0;
        if (!WinHttpQueryHeaders(Request, WINHTTP_QUERY_CUSTOM,
                                 WideName.c_str(), WINHTTP_NO_OUTPUT_BUFFER,
                                 &Size, WINHTTP_NO_HEADER_INDEX)) {
            const DWORD Error = GetLastError();
            if (Error != ERROR_INSUFFICIENT_BUFFER) {
                ErrorCode = Error;
                return false;
            }
        }
        if (Size == 0) {
            ErrorCode = WinHttpErrorHeaderNotFound;
            return false;
        }
        std::vector<wchar_t> Buffer(
            Size / sizeof(wchar_t) + 1, L'\0');
        if (!WinHttpQueryHeaders(
                Request, WINHTTP_QUERY_CUSTOM, WideName.c_str(),
                Buffer.data(), &Size, WINHTTP_NO_HEADER_INDEX)) {
            ErrorCode = GetLastError();
            return false;
        }
        Value = WideToUtf8(Buffer.data());
        return true;
    }

    [[nodiscard]] bool QueryDataAvailable(
        WinHttpHandle Request, std::uint32_t& AvailableBytes,
        std::uint32_t& ErrorCode) override {
        DWORD Available = 0;
        if (!WinHttpQueryDataAvailable(Request, &Available)) {
            ErrorCode = GetLastError();
            return false;
        }
        AvailableBytes = Available;
        return true;
    }

    [[nodiscard]] bool ReadData(
        WinHttpHandle Request, std::span<std::byte> Buffer,
        std::uint32_t& BytesRead, std::uint32_t& ErrorCode) override {
        const DWORD Requested = static_cast<DWORD>(
            std::min<std::size_t>(Buffer.size(),
                                  std::numeric_limits<DWORD>::max()));
        DWORD Read = 0;
        if (!WinHttpReadData(Request, Buffer.data(), Requested, &Read)) {
            ErrorCode = GetLastError();
            return false;
        }
        BytesRead = Read;
        return true;
    }

    [[nodiscard]] bool HttpDateToUnixSeconds(
        std::string_view HttpDate, std::int64_t& UnixSeconds) override {
        const std::wstring Wide = Utf8ToWide(HttpDate);
        if (Wide.empty()) {
            return false;
        }
        SYSTEMTIME SystemTime{};
        if (!WinHttpTimeToSystemTime(Wide.c_str(), &SystemTime)) {
            return false;
        }
        FILETIME FileTime{};
        if (!SystemTimeToFileTime(&SystemTime, &FileTime)) {
            return false;
        }
        ULARGE_INTEGER Value{};
        Value.LowPart = FileTime.dwLowDateTime;
        Value.HighPart = FileTime.dwHighDateTime;
        constexpr std::uint64_t WindowsEpochOffset = 116444736000000000ULL;
        if (Value.QuadPart < WindowsEpochOffset) {
            return false;
        }
        UnixSeconds = static_cast<std::int64_t>(
            (Value.QuadPart - WindowsEpochOffset) / 10000000ULL);
        return true;
    }

    void CloseHandle(WinHttpHandle Handle) noexcept override {
        if (Handle != nullptr) {
            WinHttpCloseHandle(Handle);
        }
    }
};

class OwnedSystemWinHttpTransport final : public HttpTransportPort {
public:
    explicit OwnedSystemWinHttpTransport(const EndpointGrantResolver& Grants)
        : HttpTransportPort(Grants) {}
    OwnedSystemWinHttpTransport(EndpointGrantResolver&&) = delete;
    OwnedSystemWinHttpTransport(const EndpointGrantResolver&&) = delete;

    [[nodiscard]] TransportHealthState Health(
        std::stop_token CancellationToken) noexcept override {
        return CancellationToken.stop_requested()
                   ? TransportHealthState::Unavailable
                   : TransportHealthState::Ready;
    }

protected:
    [[nodiscard]] TransportTerminal DoStream(
        const TransportRequest& Request, const ResponseHeadSink& OnHead,
        const ResponseChunkSink& OnChunk,
        std::stop_token CancellationToken) override {
        return ExecuteWinHttpExchange(Api, Request, OnHead, OnChunk,
                                      CancellationToken);
    }

private:
    SystemWinHttpApi Api;
};

#endif // _WIN32 && !EMULATIVE_DIALOG_WINHTTP_OFFLINE_TESTS

} // namespace

WinHttpTransport::WinHttpTransport(const EndpointGrantResolver& Grants,
                                   WinHttpApi& Api)
    : HttpTransportPort(Grants), Api(Api) {}

WinHttpTransport::~WinHttpTransport() = default;

static TransportTerminal ExecuteWinHttpExchange(
    WinHttpApi& Api, const TransportRequest& Request,
    const ResponseHeadSink& OnHead, const ResponseChunkSink& OnChunk,
    std::stop_token CancellationToken) {
    switch (ValidateRequest(Request)) {
    case RequestValidation::Ok:
        break;
    case RequestValidation::Invalid:
        return Failed(TransportFailureCode::InvalidRequest);
    case RequestValidation::HeaderLimitExceeded:
        return Failed(TransportFailureCode::HeaderLimitExceeded);
    }
    if (CancellationToken.stop_requested()) {
        return CancelledTransport{.RequestMayHaveReachedEndpoint = false};
    }

    std::uint32_t Error = 0;
    WinHttpHandle Session = Api.OpenSession(Request.Proxy, Error);
    if (Session == nullptr) {
        return MapFailure(Error, FailureStage::Session);
    }
    HandleGuard SessionGuard(Api, Session);

    if (!Api.SetTimeouts(Session, Request.Timeouts, Error)) {
        return MapFailure(Error, FailureStage::Session);
    }
    if (!Api.DisableRedirects(Session, Error)) {
        return MapFailure(Error, FailureStage::Session);
    }

    WinHttpHandle Connection =
        Api.Connect(Session, Request.Destination.Host,
                    Request.Destination.Port, Error);
    if (Connection == nullptr) {
        return MapFailure(Error, FailureStage::Connect);
    }
    HandleGuard ConnectionGuard(Api, Connection);

    WinHttpHandle HttpRequest =
        Api.OpenRequest(Connection, Request.Destination.Path, Error);
    if (HttpRequest == nullptr) {
        return Failed(TransportFailureCode::InternalError);
    }
    HandleGuard RequestGuard(Api, HttpRequest);

    for (const TransportHeader& Header : Request.Headers) {
        if (IsAdapterForbiddenHeader(Header.Name)) {
            continue; // Host/Content-Length are transport-owned and ignored.
        }
        if (!Api.AddHeader(HttpRequest, Header.Name, Header.Value, Error)) {
            return Failed(TransportFailureCode::InternalError);
        }
    }

    if (CancellationToken.stop_requested()) {
        return CancelledTransport{.RequestMayHaveReachedEndpoint = false};
    }
    if (!Api.SendRequest(HttpRequest, Request.BodyUtf8, Error)) {
        return MapFailure(Error, FailureStage::Send);
    }
    if (CancellationToken.stop_requested()) {
        return CancelledTransport{.RequestMayHaveReachedEndpoint = true};
    }
    if (!Api.ReceiveResponse(HttpRequest, Error)) {
        return MapFailure(Error, FailureStage::Receive);
    }

    std::uint16_t StatusCode = 0;
    if (!Api.QueryStatusCode(HttpRequest, StatusCode, Error)) {
        return MapFailure(Error, FailureStage::Receive);
    }

    if (StatusCode >= 300 && StatusCode < 400) {
        std::string Location;
        std::uint32_t LocationError = 0;
        if (Api.QueryHeader(HttpRequest, "Location", Location,
                            LocationError)) {
            return RedirectDeniedTransport{
                .StatusCode = StatusCode,
                .RedactedLocationHost = ExtractLocationHost(Location)};
        }
        return RedirectDeniedTransport{.StatusCode = StatusCode,
                                       .RedactedLocationHost = {}};
    }

    std::uint64_t ResponseHeaderBytes = 0;
    std::string ContentEncoding;
    Error = 0;
    if (Api.QueryHeader(HttpRequest, "Content-Encoding", ContentEncoding,
                        Error)) {
        ResponseHeaderBytes += ContentEncoding.size() + 19;
    }
    const std::string NormalizedEncoding =
        ToLowerAscii(TrimAscii(ContentEncoding));
    if (!NormalizedEncoding.empty() && NormalizedEncoding != "identity") {
        return Failed(TransportFailureCode::InvalidHttpResponse);
    }

    ResponseHeadMetadata Head;
    Head.StatusCode = StatusCode;
    Error = 0;
    if (Api.QueryHeader(HttpRequest, "Content-Type", Head.ContentType, Error)) {
        ResponseHeaderBytes += Head.ContentType.size() + 15;
    }
    Error = 0;
    std::string RetryAfterRaw;
    if (Api.QueryHeader(HttpRequest, "Retry-After", RetryAfterRaw, Error)) {
        ResponseHeaderBytes += RetryAfterRaw.size() + 13;
        Head.RetryAfter = NormalizeRetryAfter(
            RetryAfterRaw, Api, std::chrono::system_clock::now());
    }
    if (ResponseHeaderBytes > Request.Limits.MaximumHeaderBytes) {
        return Failed(TransportFailureCode::HeaderLimitExceeded);
    }

    if (OnHead) {
        try {
            OnHead(Head);
        } catch (...) {
            return Failed(TransportFailureCode::InternalError);
        }
    }

    const Clock::time_point Deadline =
        Clock::now() + Request.Timeouts.Response;
    std::uint64_t TotalBytes = 0;
    std::vector<std::byte> Buffer;
    while (true) {
        if (CancellationToken.stop_requested()) {
            return CancelledTransport{
                .RequestMayHaveReachedEndpoint = true};
        }
        if (Clock::now() > Deadline) {
            return TimedOutTransport{.Phase = TransportTimeoutPhase::Response};
        }

        std::uint32_t Available = 0;
        if (!Api.QueryDataAvailable(HttpRequest, Available, Error)) {
            return MapFailure(Error, FailureStage::Read);
        }
        if (Available == 0) {
            break;
        }
        const std::uint64_t Remaining =
            Request.Limits.MaximumResponseBytes - TotalBytes;
        const std::size_t ToRead = BoundedReadSize(
            Available, Request.Limits.MaximumChunkBytes, Remaining);
        if (ToRead == 0) {
            return Failed(TransportFailureCode::ResponseLimitExceeded);
        }
        Buffer.resize(ToRead);
        std::uint32_t BytesRead = 0;
        if (!Api.ReadData(HttpRequest, Buffer, BytesRead, Error)) {
            return MapFailure(Error, FailureStage::Read);
        }
        if (BytesRead == 0 || BytesRead > ToRead) {
            return Unknown();
        }
        TotalBytes += BytesRead;
        if (OnChunk) {
            try {
                OnChunk(std::span<const std::byte>(
                    Buffer.data(), static_cast<std::size_t>(BytesRead)));
            } catch (...) {
                return Failed(TransportFailureCode::InternalError);
            }
        }
    }

    return CompletedTransport{.StatusCode = StatusCode,
                              .ReceivedBodyBytes = TotalBytes};
}

TransportTerminal WinHttpTransport::DoStream(
    const TransportRequest& Request, const ResponseHeadSink& OnHead,
    const ResponseChunkSink& OnChunk, std::stop_token CancellationToken) {
    return ExecuteWinHttpExchange(Api, Request, OnHead, OnChunk,
                                  CancellationToken);
}

TransportHealthState WinHttpTransport::Health(
    std::stop_token CancellationToken) noexcept {
    return CancellationToken.stop_requested()
               ? TransportHealthState::Unavailable
               : TransportHealthState::Ready;
}

std::unique_ptr<HttpTransportPort> CreateSystemWinHttpTransport(
    const EndpointGrantResolver& Grants) {
#if defined(_WIN32) && !defined(EMULATIVE_DIALOG_WINHTTP_OFFLINE_TESTS)
    return std::make_unique<OwnedSystemWinHttpTransport>(Grants);
#else
    static_cast<void>(Grants);
    return nullptr;
#endif
}

} // namespace EmulativeDialog::Platform::Network
