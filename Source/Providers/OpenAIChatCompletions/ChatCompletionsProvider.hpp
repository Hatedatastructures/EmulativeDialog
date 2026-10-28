#pragma once

#include "Source/Platform/Network/HttpTransportPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace EmulativeDialog::Providers::OpenAIChatCompletions {

// Core owns endpoint grants. The provider never mints one; construction injects
// a read-only resolver so tests can supply deterministic grants offline.
using EndpointGrantProvider =
    std::function<Platform::Network::EndpointGrantId(
        const ModelProviderProfile& Profile)>;

class ChatCompletionsProvider final : public ModelProviderPort {
public:
    ChatCompletionsProvider(
        Platform::Network::HttpTransportPort& Transport,
        EndpointGrantProvider ResolveGrant);

    ChatCompletionsProvider(const ChatCompletionsProvider&) = delete;
    ChatCompletionsProvider& operator=(const ChatCompletionsProvider&) = delete;
    ChatCompletionsProvider(ChatCompletionsProvider&&) = delete;
    ChatCompletionsProvider& operator=(ChatCompletionsProvider&&) = delete;
    ~ChatCompletionsProvider() override = default;

    [[nodiscard]] ProviderResult<ProviderCapabilities>
    GetCapabilities(const ProviderProfileId& ProfileId) const override;

    [[nodiscard]] ProviderResult<std::monostate>
    ValidateProfile(const ModelProviderProfile& Profile) const override;

    [[nodiscard]] CanonicalTerminalEvent
    StreamTurn(
        const ModelProviderProfile& Profile,
        const CredentialLease& Lease,
        const CanonicalModelRequest& Request,
        std::stop_token CancellationToken,
        const EventSink& Sink) override;

    [[nodiscard]] ProviderResult<ProviderHealthStatus>
    HealthCheck(const ModelProviderProfile& Profile) override;

private:
    struct ProfileState final {
        ModelProviderProfile Profile;
        Platform::Network::HttpsDestination Destination;
    };

    [[nodiscard]] ProviderResult<ProfileState> ValidateAndCacheProfile(
        const ModelProviderProfile& Profile) const;

    Platform::Network::HttpTransportPort& Transport;
    EndpointGrantProvider Grants;
    mutable std::mutex Mutex;
    mutable std::unordered_map<std::string, ProfileState> Profiles;
};

} // namespace EmulativeDialog::Providers::OpenAIChatCompletions
