#include "Source/Core/EndpointGrantAuthority.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

namespace EmulativeDialog::Core {
namespace {

namespace Network = Platform::Network;
using Network::EndpointGrantId;
using Network::EndpointGrantStatus;
using Network::HttpsDestination;

constexpr auto GrantLifetime = std::chrono::minutes{5};
constexpr char HexDigits[] = "0123456789abcdef";

bool IsAsciiControlOrSpace(unsigned char Value) noexcept {
  return Value <= 0x20U || Value == 0x7fU;
}

bool IsHex(char Value) noexcept {
  return (Value >= '0' && Value <= '9') ||
         (Value >= 'a' && Value <= 'f') ||
         (Value >= 'A' && Value <= 'F');
}

unsigned char HexValue(char Value) noexcept {
  if (Value >= '0' && Value <= '9') {
    return static_cast<unsigned char>(Value - '0');
  }
  if (Value >= 'a' && Value <= 'f') {
    return static_cast<unsigned char>(Value - 'a' + 10);
  }
  return static_cast<unsigned char>(Value - 'A' + 10);
}

bool IsUnreserved(unsigned char Value) noexcept {
  return (Value >= 'a' && Value <= 'z') ||
         (Value >= 'A' && Value <= 'Z') ||
         (Value >= '0' && Value <= '9') || Value == '-' || Value == '.' ||
         Value == '_' || Value == '~';
}

bool IsSubDelimiter(char Value) noexcept {
  constexpr std::string_view Delimiters = "!$&'()*+,;=";
  return Delimiters.find(Value) != std::string_view::npos;
}

std::string NormalizeEscapes(std::string_view Input) {
  std::string Result;
  Result.reserve(Input.size());
  for (std::size_t Index = 0; Index < Input.size(); ++Index) {
    const char Value = Input[Index];
    if (Value != '%') {
      Result.push_back(Value);
      continue;
    }
    if (Index + 2 >= Input.size() || !IsHex(Input[Index + 1]) ||
        !IsHex(Input[Index + 2])) {
      throw std::invalid_argument("profile endpoint has malformed percent encoding");
    }
    const auto Decoded = static_cast<unsigned char>(
        (HexValue(Input[Index + 1]) << 4U) | HexValue(Input[Index + 2]));
    if (IsAsciiControlOrSpace(Decoded) || Decoded == '/' || Decoded == '\\') {
      throw std::invalid_argument("profile endpoint has an ambiguous escaped path byte");
    }
    if (IsUnreserved(Decoded)) {
      Result.push_back(static_cast<char>(Decoded));
    } else {
      Result.push_back('%');
      Result.push_back(HexDigits[Decoded >> 4U]);
      Result.push_back(HexDigits[Decoded & 0x0fU]);
    }
    Index += 2;
  }
  return Result;
}

void ValidatePathAndQuery(std::string_view PathAndQuery) {
  const std::size_t QueryOffset = PathAndQuery.find('?');
  const std::string_view Path = PathAndQuery.substr(0, QueryOffset);
  std::size_t SegmentStart = 0;
  while (SegmentStart <= Path.size()) {
    const std::size_t SegmentEnd = Path.find('/', SegmentStart);
    const std::string_view Segment = Path.substr(
        SegmentStart, SegmentEnd == std::string_view::npos
                          ? Path.size() - SegmentStart
                          : SegmentEnd - SegmentStart);
    if (Segment == "." || Segment == "..") {
      throw std::invalid_argument("profile endpoint path contains a dot segment");
    }
    if (SegmentEnd == std::string_view::npos) {
      break;
    }
    SegmentStart = SegmentEnd + 1;
  }

  bool InQuery = false;
  for (const char Value : PathAndQuery) {
    if (Value == '?') {
      InQuery = true;
      continue;
    }
    const auto Byte = static_cast<unsigned char>(Value);
    const bool Allowed = IsUnreserved(Byte) || IsSubDelimiter(Value) ||
                         Value == ':' || Value == '@' || Value == '/' ||
                         (InQuery && Value == '?') || Value == '%';
    if (!Allowed) {
      throw std::invalid_argument("profile endpoint path or query has invalid syntax");
    }
  }
}

std::uint16_t ParsePort(std::string_view PortText) {
  if (PortText.empty()) {
    throw std::invalid_argument("profile endpoint has an empty port");
  }
  unsigned int Parsed = 0;
  const auto [End, Error] =
      std::from_chars(PortText.data(), PortText.data() + PortText.size(), Parsed);
  if (Error != std::errc{} || End != PortText.data() + PortText.size() ||
      Parsed == 0 || Parsed > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("profile endpoint port is invalid");
  }
  return static_cast<std::uint16_t>(Parsed);
}

std::string NormalizeIpv4(std::string_view Host) {
  const std::string Owned{Host};
  IN_ADDR Address{};
  if (InetPtonA(AF_INET, Owned.c_str(), &Address) != 1) {
    return {};
  }
  std::array<char, INET_ADDRSTRLEN> Buffer{};
  if (InetNtopA(AF_INET, &Address, Buffer.data(),
                static_cast<DWORD>(Buffer.size())) == nullptr) {
    throw std::invalid_argument("profile endpoint IPv4 literal cannot be normalized");
  }
  return Buffer.data();
}

std::string NormalizeIpv6(std::string_view Host) {
  const std::string Owned{Host};
  IN6_ADDR Address{};
  if (InetPtonA(AF_INET6, Owned.c_str(), &Address) != 1) {
    throw std::invalid_argument("profile endpoint IPv6 literal is invalid");
  }
  std::array<char, INET6_ADDRSTRLEN> Buffer{};
  if (InetNtopA(AF_INET6, &Address, Buffer.data(),
                static_cast<DWORD>(Buffer.size())) == nullptr) {
    throw std::invalid_argument("profile endpoint IPv6 literal cannot be normalized");
  }
  return Buffer.data();
}

std::string NormalizeDnsName(std::string Host) {
  std::transform(Host.begin(), Host.end(), Host.begin(), [](unsigned char Value) {
    return static_cast<char>(std::tolower(Value));
  });
  if (!Host.empty() && Host.back() == '.') {
    Host.pop_back();
  }
  if (Host.empty() || Host.back() == '.' || Host.size() > 253) {
    throw std::invalid_argument("profile endpoint DNS host length is invalid");
  }
  bool NumericDotted = true;
  for (const char Value : Host) {
    NumericDotted = NumericDotted &&
                    ((Value >= '0' && Value <= '9') || Value == '.');
  }
  if (NumericDotted) {
    throw std::invalid_argument("profile endpoint numeric host is not a valid IPv4 literal");
  }

  std::size_t LabelStart = 0;
  while (LabelStart < Host.size()) {
    const std::size_t LabelEnd = Host.find('.', LabelStart);
    const std::size_t End = LabelEnd == std::string::npos ? Host.size() : LabelEnd;
    const std::size_t LabelLength = End - LabelStart;
    if (LabelLength == 0 || LabelLength > 63 ||
        Host[LabelStart] == '-' || Host[End - 1] == '-') {
      throw std::invalid_argument("profile endpoint DNS label is invalid");
    }
    for (std::size_t Index = LabelStart; Index < End; ++Index) {
      const char Value = Host[Index];
      if (!((Value >= 'a' && Value <= 'z') ||
            (Value >= '0' && Value <= '9') || Value == '-')) {
        throw std::invalid_argument("profile endpoint DNS host contains an invalid character");
      }
    }
    if (LabelEnd == std::string::npos) {
      break;
    }
    LabelStart = LabelEnd + 1;
  }
  return Host;
}

std::string EncodeToken(const std::array<std::byte, 32>& Bytes) {
  std::string Encoded;
  Encoded.resize(64);
  for (std::size_t Index = 0; Index < Bytes.size(); ++Index) {
    const auto Value = std::to_integer<unsigned char>(Bytes[Index]);
    Encoded[Index * 2] = HexDigits[Value >> 4U];
    Encoded[Index * 2 + 1] = HexDigits[Value & 0x0fU];
  }
  return Encoded;
}

} // namespace

EndpointGrantAuthority::EndpointGrantAuthority(
    const std::vector<Providers::ModelProviderProfile>& TrustedProfiles)
    : Entropy(SystemEntropy), Now([] { return Clock::now(); }) {
  InitializeProfiles(TrustedProfiles);
}

#if defined(EMULATIVE_DIALOG_ENABLE_TEST_SEAMS)
EndpointGrantAuthority::EndpointGrantAuthority(
    const std::vector<Providers::ModelProviderProfile>& TrustedProfiles,
    EntropyFunction EntropySource,
    NowFunction ClockSource)
    : Entropy(std::move(EntropySource)), Now(std::move(ClockSource)) {
  if (!Entropy || !Now) {
    throw std::invalid_argument("endpoint grant authority requires entropy and clock sources");
  }
  InitializeProfiles(TrustedProfiles);
}
#endif

void EndpointGrantAuthority::InitializeProfiles(
    const std::vector<Providers::ModelProviderProfile>& TrustedProfiles) {
  for (const Providers::ModelProviderProfile& Profile : TrustedProfiles) {
    if (Profile.Id.Value.empty()) {
      throw std::invalid_argument("provider profile identity must not be empty");
    }
    HttpsDestination Destination = ParseEndpoint(Profile.Endpoint);
    const auto [Iterator, Inserted] = Profiles.try_emplace(
        Profile.Id.Value,
        ProfileRecord{.Destination = std::move(Destination), .Revoked = false});
    (void)Iterator;
    if (!Inserted) {
      throw std::invalid_argument("provider profile identities must be unique");
    }
  }
}

EndpointGrantIssueResult EndpointGrantAuthority::IssueForProfile(
    const Providers::ProviderProfileId& ProfileId) {
  try {
    {
      const std::lock_guard Lock{Mutex};
      const auto Profile = Profiles.find(ProfileId.Value);
      if (Profile == Profiles.end()) {
        return EndpointGrantIssueFailure::UnknownProfile;
      }
      if (Profile->second.Revoked) {
        return EndpointGrantIssueFailure::RevokedProfile;
      }
    }

    std::array<std::byte, 32> RandomBytes{};
    if (!Entropy(RandomBytes)) {
      return EndpointGrantIssueFailure::EntropyUnavailable;
    }
    const std::string Token = EncodeToken(RandomBytes);
    const Clock::time_point IssuedAt = Now();
    if (IssuedAt > Clock::time_point::max() - GrantLifetime) {
      return EndpointGrantIssueFailure::InternalError;
    }
    const Clock::time_point ExpiresAt = IssuedAt + GrantLifetime;
    if (ExpiresAt <= IssuedAt) {
      return EndpointGrantIssueFailure::InternalError;
    }

    const std::lock_guard Lock{Mutex};
    const auto Profile = Profiles.find(ProfileId.Value);
    if (Profile == Profiles.end()) {
      return EndpointGrantIssueFailure::UnknownProfile;
    }
    if (Profile->second.Revoked) {
      return EndpointGrantIssueFailure::RevokedProfile;
    }
    const auto [Grant, Inserted] = Grants.try_emplace(
        Token, GrantRecord{.ProfileId = ProfileId.Value,
                           .Destination = Profile->second.Destination,
                           .IssuedAt = IssuedAt,
                           .ExpiresAt = ExpiresAt});
    (void)Grant;
    if (!Inserted) {
      return EndpointGrantIssueFailure::Collision;
    }
    return EndpointGrant{.Id = EndpointGrantId{.Value = Token},
                         .Destination = Profile->second.Destination};
  } catch (...) {
    return EndpointGrantIssueFailure::InternalError;
  }
}

EndpointGrantRevokeResult EndpointGrantAuthority::RevokeProfile(
    const Providers::ProviderProfileId& ProfileId) noexcept {
  try {
    const std::lock_guard Lock{Mutex};
    const auto Profile = Profiles.find(ProfileId.Value);
    if (Profile == Profiles.end()) {
      return EndpointGrantRevokeResult::UnknownProfile;
    }
    if (Profile->second.Revoked) {
      return EndpointGrantRevokeResult::AlreadyRevoked;
    }
    Profile->second.Revoked = true;
    return EndpointGrantRevokeResult::Revoked;
  } catch (...) {
    return EndpointGrantRevokeResult::UnknownProfile;
  }
}

Platform::Network::EndpointGrantStatus EndpointGrantAuthority::Validate(
    const Platform::Network::EndpointGrantId& Grant,
    const Platform::Network::HttpsDestination& Requested) const noexcept {
  try {
    const Clock::time_point CurrentTime = Now();
    const std::lock_guard Lock{Mutex};
    const auto GrantEntry = Grants.find(Grant.Value);
    if (GrantEntry == Grants.end()) {
      return EndpointGrantStatus::Missing;
    }
    const auto Profile = Profiles.find(GrantEntry->second.ProfileId);
    if (Profile == Profiles.end() || Profile->second.Revoked) {
      return EndpointGrantStatus::Missing;
    }
    if (CurrentTime >= GrantEntry->second.ExpiresAt) {
      return EndpointGrantStatus::Expired;
    }
    if (GrantEntry->second.Destination != Requested) {
      return EndpointGrantStatus::DestinationMismatch;
    }
    return EndpointGrantStatus::Valid;
  } catch (...) {
    return EndpointGrantStatus::Missing;
  }
}

bool EndpointGrantAuthority::SystemEntropy(
    std::array<std::byte, 32>& Bytes) noexcept {
  return BCryptGenRandom(nullptr,
                         reinterpret_cast<PUCHAR>(Bytes.data()),
                         static_cast<ULONG>(Bytes.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

HttpsDestination EndpointGrantAuthority::ParseEndpoint(
    const std::string& Endpoint) {
  if (Endpoint.empty()) {
    throw std::invalid_argument("provider profile endpoint must not be empty");
  }
  for (const unsigned char Value : Endpoint) {
    if (IsAsciiControlOrSpace(Value) || Value >= 0x80U || Value == '\\') {
      throw std::invalid_argument("provider profile endpoint contains an unsafe character");
    }
  }
  if (Endpoint.find('#') != std::string::npos) {
    throw std::invalid_argument("provider profile endpoint fragments are not allowed");
  }
  const std::size_t SchemeEnd = Endpoint.find("://");
  if (SchemeEnd == std::string::npos ||
      !Network::EqualsAsciiNoCase(std::string_view{Endpoint}.substr(0, SchemeEnd),
                                  "https")) {
    throw std::invalid_argument("provider profile endpoint must use HTTPS");
  }

  const std::size_t AuthorityStart = SchemeEnd + 3;
  const std::size_t AuthorityEnd = Endpoint.find_first_of("/?", AuthorityStart);
  const std::size_t AuthorityLength =
      (AuthorityEnd == std::string::npos ? Endpoint.size() : AuthorityEnd) -
      AuthorityStart;
  const std::string_view Authority{Endpoint.data() + AuthorityStart,
                                  AuthorityLength};
  if (Authority.empty() || Authority.find('@') != std::string_view::npos) {
    throw std::invalid_argument("provider profile endpoint authority is empty or contains userinfo");
  }

  std::string Host;
  std::string_view PortText;
  bool HasPort = false;
  if (Authority.front() == '[') {
    const std::size_t Close = Authority.find(']');
    if (Close == std::string_view::npos || Close == 1) {
      throw std::invalid_argument("provider profile endpoint IPv6 authority is malformed");
    }
    Host = NormalizeIpv6(Authority.substr(1, Close - 1));
    const std::string_view Tail = Authority.substr(Close + 1);
    if (!Tail.empty()) {
      if (Tail.front() != ':' || Tail.find(':', 1) != std::string_view::npos) {
        throw std::invalid_argument("provider profile endpoint IPv6 port syntax is invalid");
      }
      HasPort = true;
      PortText = Tail.substr(1);
    }
  } else {
    const std::size_t Colon = Authority.find(':');
    std::string_view HostText = Authority;
    if (Colon != std::string_view::npos) {
      if (Authority.find(':', Colon + 1) != std::string_view::npos) {
        throw std::invalid_argument("IPv6 profile endpoints must use brackets");
      }
      HostText = Authority.substr(0, Colon);
      PortText = Authority.substr(Colon + 1);
      HasPort = true;
    }
    if (HostText.empty() || HostText.find('[') != std::string_view::npos ||
        HostText.find(']') != std::string_view::npos ||
        HostText.find('%') != std::string_view::npos) {
      throw std::invalid_argument("provider profile endpoint host syntax is invalid");
    }
    Host = NormalizeIpv4(HostText);
    if (Host.empty()) {
      Host = NormalizeDnsName(std::string{HostText});
    }
  }

  const std::uint16_t Port = HasPort ? ParsePort(PortText) : 443;
  std::string PathAndQuery = "/";
  if (AuthorityEnd != std::string::npos) {
    const std::string_view Suffix{Endpoint.data() + AuthorityEnd,
                                  Endpoint.size() - AuthorityEnd};
    if (Suffix.front() == '?') {
      PathAndQuery = "/" + std::string{Suffix};
    } else {
      PathAndQuery.assign(Suffix);
    }
    PathAndQuery = NormalizeEscapes(PathAndQuery);
    ValidatePathAndQuery(PathAndQuery);
  }
  return HttpsDestination{.Host = std::move(Host),
                          .Port = Port,
                          .Path = std::move(PathAndQuery)};
}

} // namespace EmulativeDialog::Core
