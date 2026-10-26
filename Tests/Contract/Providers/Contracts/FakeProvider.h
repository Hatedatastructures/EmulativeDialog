#pragma once

#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <cstddef>
#include <deque>
#include <stop_token>
#include <variant>
#include <vector>

namespace EmulativeDialog::Providers::Testing {

using namespace EmulativeDialog::Providers;

// Deterministic offline ModelProviderPort test double; never linked into product targets.
class FakeProvider final : public ModelProviderPort {
public:
    struct Script final {
        std::vector<CanonicalNonTerminalEvent> NonTerminalEvents;
        CanonicalTerminalEvent Terminal;
    };

    explicit FakeProvider(
        ProviderCapabilities Capabilities = ProviderCapabilities{true, true, true, true, true, true});

    void EnqueueScript(Script NewScript);

    [[nodiscard]] const std::vector<CanonicalModelRequest>& GetCapturedRequests() const noexcept;
    [[nodiscard]] std::size_t GetRemainingScriptCount() const noexcept;

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
    ProviderCapabilities Capabilities;
    std::deque<Script> Scripts;
    std::vector<CanonicalModelRequest> CapturedRequests;
};

} // namespace EmulativeDialog::Providers::Testing
