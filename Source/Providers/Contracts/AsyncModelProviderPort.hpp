#pragma once

#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <boost/asio/awaitable.hpp>

namespace EmulativeDialog::Providers {

class AsyncModelProviderPort {
public:
  virtual ~AsyncModelProviderPort() = default;

  [[nodiscard]] virtual boost::asio::awaitable<
      ProviderResult<ProviderCapabilities>>
  GetCapabilitiesAsync(const ProviderProfileId& ProfileId) const = 0;

  [[nodiscard]] virtual boost::asio::awaitable<
      ProviderResult<std::monostate>>
  ValidateProfileAsync(const ModelProviderProfile& Profile) const = 0;

  [[nodiscard]] virtual boost::asio::awaitable<CanonicalTerminalEvent>
  StreamTurnAsync(const ModelProviderProfile& Profile,
                  const CredentialLease& Lease,
                  const CanonicalModelRequest& Request,
                  std::stop_token CancellationToken,
                  const EventSink& Sink) = 0;

  [[nodiscard]] virtual boost::asio::awaitable<
      ProviderResult<ProviderHealthStatus>>
  HealthCheckAsync(const ModelProviderProfile& Profile) = 0;
};

} // namespace EmulativeDialog::Providers