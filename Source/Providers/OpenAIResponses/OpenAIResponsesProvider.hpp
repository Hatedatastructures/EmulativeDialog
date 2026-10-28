#pragma once

#include "Source/Platform/Network/HttpTransportPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <stop_token>
#include <string>
#include <variant>

namespace EmulativeDialog::Providers::OpenAIResponses {

// OpenAI Responses adapter for ModelProviderPort V1.1/V1.2. All network work
// goes through the injected, Core-authorized HttpTransportPort; this adapter
// only builds Responses wire JSON and maps the SSE stream back to canonical
// events. It never holds credentials beyond the leased call and never exposes
// raw provider bodies, URLs, or secrets in canonical events.
class OpenAIResponsesProvider final : public ModelProviderPort {
public:
    OpenAIResponsesProvider(
        ProviderProfileId Profile,
        Platform::Network::HttpTransportPort& Transport,
        Platform::Network::EndpointGrantId Grant,
        Platform::Network::HttpsDestination Destination);
    ~OpenAIResponsesProvider() override;

    OpenAIResponsesProvider(const OpenAIResponsesProvider&) = delete;
    OpenAIResponsesProvider& operator=(const OpenAIResponsesProvider&) = delete;

    [[nodiscard]] ProviderResult<ProviderCapabilities> GetCapabilities(
        const ProviderProfileId& ProfileId) const override;

    [[nodiscard]] ProviderResult<std::monostate> ValidateProfile(
        const ModelProviderProfile& Profile) const override;

    [[nodiscard]] CanonicalTerminalEvent StreamTurn(
        const ModelProviderProfile& Profile, const CredentialLease& Lease,
        const CanonicalModelRequest& Request,
        std::stop_token CancellationToken,
        const EventSink& Sink) override;

    [[nodiscard]] ProviderResult<ProviderHealthStatus> HealthCheck(
        const ModelProviderProfile& Profile) override;

private:
    ProviderProfileId BoundProfile;
    Platform::Network::HttpTransportPort& Transport;
    Platform::Network::EndpointGrantId Grant;
    Platform::Network::HttpsDestination Destination;
};

} // namespace EmulativeDialog::Providers::OpenAIResponses
