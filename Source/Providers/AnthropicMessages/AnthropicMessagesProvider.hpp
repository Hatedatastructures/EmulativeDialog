#pragma once

#include "Source/Platform/Network/HttpTransportPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Providers {

// Anthropic Messages adapter. Protocol details stay inside this module; the
// transport is injected so the adapter never opens sockets directly and tests
// can drive deterministic SSE fixtures.
class AnthropicMessagesProvider final : public ModelProviderPort {
public:
  explicit AnthropicMessagesProvider(
      Platform::Network::HttpTransportPort& Transport) noexcept;

  [[nodiscard]] ProviderResult<ProviderCapabilities> GetCapabilities(
      const ProviderProfileId& ProfileId) const override;

  [[nodiscard]] ProviderResult<std::monostate> ValidateProfile(
      const ModelProviderProfile& Profile) const override;

  [[nodiscard]] CanonicalTerminalEvent StreamTurn(
      const ModelProviderProfile& Profile, const CredentialLease& Lease,
      const CanonicalModelRequest& Request,
      std::stop_token CancellationToken, const EventSink& Sink) override;

  [[nodiscard]] ProviderResult<ProviderHealthStatus> HealthCheck(
      const ModelProviderProfile& Profile) override;

private:
  Platform::Network::HttpTransportPort& Transport_;
};

} // namespace EmulativeDialog::Providers
