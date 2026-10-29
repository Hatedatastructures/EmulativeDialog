#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Source/Platform/Network/HttpTransportPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Core {

struct EndpointGrant final {
  Platform::Network::EndpointGrantId Id;
  Platform::Network::HttpsDestination Destination;
};

enum class EndpointGrantIssueFailure : std::uint8_t {
  UnknownProfile,
  RevokedProfile,
  EntropyUnavailable,
  Collision,
  InternalError,
};

using EndpointGrantIssueResult =
    std::variant<EndpointGrant, EndpointGrantIssueFailure>;

enum class EndpointGrantRevokeResult : std::uint8_t {
  Revoked,
  AlreadyRevoked,
  UnknownProfile,
};

// Core-owned, process-local authority for the immutable provider profile
// snapshot supplied by trusted Core configuration. Grant IDs are short-lived
// bearer capabilities: callers must not log, persist, or expose them.
class EndpointGrantAuthority final
    : public Platform::Network::EndpointGrantResolver {
public:
  using Clock = std::chrono::steady_clock;

  explicit EndpointGrantAuthority(
      const std::vector<Providers::ModelProviderProfile>& TrustedProfiles);
  ~EndpointGrantAuthority() override = default;

  EndpointGrantAuthority(const EndpointGrantAuthority&) = delete;
  EndpointGrantAuthority& operator=(const EndpointGrantAuthority&) = delete;
  EndpointGrantAuthority(EndpointGrantAuthority&&) = delete;
  EndpointGrantAuthority& operator=(EndpointGrantAuthority&&) = delete;

  // The only issuance input is a trusted profile identity. The destination is
  // always copied from the constructor snapshot; arbitrary URLs are not
  // accepted by this API.
  [[nodiscard]] EndpointGrantIssueResult IssueForProfile(
      const Providers::ProviderProfileId& ProfileId);

  [[nodiscard]] EndpointGrantRevokeResult RevokeProfile(
      const Providers::ProviderProfileId& ProfileId) noexcept;

  [[nodiscard]] Platform::Network::EndpointGrantStatus Validate(
      const Platform::Network::EndpointGrantId& Grant,
      const Platform::Network::HttpsDestination& Requested) const noexcept
      override;

private:
  using EntropyFunction =
      std::function<bool(std::array<std::byte, 32>& Bytes)>;
  using NowFunction = std::function<Clock::time_point()>;

  struct ProfileRecord final {
    Platform::Network::HttpsDestination Destination;
    bool Revoked{};
  };

  struct GrantRecord final {
    std::string ProfileId;
    Platform::Network::HttpsDestination Destination;
    Clock::time_point IssuedAt;
    Clock::time_point ExpiresAt;
  };

#if defined(EMULATIVE_DIALOG_ENABLE_TEST_SEAMS)
  EndpointGrantAuthority(
      const std::vector<Providers::ModelProviderProfile>& TrustedProfiles,
      EntropyFunction Entropy,
      NowFunction Now);
#endif

  void InitializeProfiles(
      const std::vector<Providers::ModelProviderProfile>& TrustedProfiles);

  static bool SystemEntropy(std::array<std::byte, 32>& Bytes) noexcept;
  static Platform::Network::HttpsDestination ParseEndpoint(
      const std::string& Endpoint);

  mutable std::mutex Mutex;
  std::unordered_map<std::string, ProfileRecord> Profiles;
  // Expired and revoked IDs remain tombstoned here. They are never rebound.
  std::unordered_map<std::string, GrantRecord> Grants;
  EntropyFunction Entropy;
  NowFunction Now;

#if defined(EMULATIVE_DIALOG_ENABLE_TEST_SEAMS)
  friend class EndpointGrantAuthorityTestAccess;
#endif
};

} // namespace EmulativeDialog::Core
