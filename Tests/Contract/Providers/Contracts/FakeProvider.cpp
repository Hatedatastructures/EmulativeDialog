#include "Tests/Contract/Providers/Contracts/FakeProvider.h"

#include <string>
#include <utility>

namespace EmulativeDialog::Providers::Testing {

namespace {

template <typename Event>
void SetEventMetadata(
    Event& ProviderEvent,
    const CanonicalModelRequest& Request,
    std::uint64_t SequenceNumber) {
    ProviderEvent.Metadata.Request = Request.Id;
    ProviderEvent.Metadata.TurnId = Request.TurnId;
    ProviderEvent.Metadata.SequenceNumber = SequenceNumber;
}

void CorrelateEvent(
    CanonicalNonTerminalEvent& Event,
    const CanonicalModelRequest& Request,
    std::uint64_t SequenceNumber) {
    std::visit(
        [&Request, SequenceNumber](auto& ProviderEvent) {
            SetEventMetadata(ProviderEvent, Request, SequenceNumber);
        },
        Event);
}

void CorrelateEvent(
    CanonicalTerminalEvent& Event,
    const CanonicalModelRequest& Request,
    std::uint64_t SequenceNumber) {
    std::visit(
        [&Request, SequenceNumber](auto& ProviderEvent) {
            SetEventMetadata(ProviderEvent, Request, SequenceNumber);
        },
        Event);
}

CanonicalTerminalEvent MakeFailureTerminal(
    const CanonicalModelRequest& Request,
    FailureCode Code,
    std::string Description) {
    return ResponseFailedEvent{
        ProviderEventMetadata{Request.Id, Request.TurnId, 1},
        ProviderError{Code, false, std::move(Description)}};
}

} // namespace

FakeProvider::FakeProvider(ProviderCapabilities Capabilities)
    : Capabilities(Capabilities) {}

void FakeProvider::EnqueueScript(Script NewScript) {
    Scripts.push_back(std::move(NewScript));
}

const std::vector<CanonicalModelRequest>& FakeProvider::GetCapturedRequests() const noexcept {
    return CapturedRequests;
}

std::size_t FakeProvider::GetRemainingScriptCount() const noexcept {
    return Scripts.size();
}

ProviderResult<ProviderCapabilities>
FakeProvider::GetCapabilities(const ProviderProfileId& ProfileId) const {
    if (ProfileId.Value.empty()) {
        return ProviderError{
            FailureCode::InvalidProfile,
            false,
            "FakeProvider requires a synthetic profile identifier."};
    }

    return Capabilities;
}

ProviderResult<std::monostate>
FakeProvider::ValidateProfile(const ModelProviderProfile& Profile) const {
    if (Profile.Id.Value.empty() || Profile.ModelName.empty()) {
        return ProviderError{
            FailureCode::InvalidProfile,
            false,
            "FakeProvider requires synthetic profile and model identifiers."};
    }

    return std::monostate{};
}

CanonicalTerminalEvent
FakeProvider::StreamTurn(
    const ModelProviderProfile&,
    const CredentialLease&,
    const CanonicalModelRequest& Request,
    std::stop_token,
    const EventSink& Sink) {
    CapturedRequests.push_back(Request);

    if (Scripts.empty()) {
        return MakeFailureTerminal(
            Request,
            FailureCode::InvalidRequest,
            "FakeProvider script queue is exhausted.");
    }

    Script NextScript = std::move(Scripts.front());
    Scripts.pop_front();

    std::uint64_t SequenceNumber = 1;
    for (CanonicalNonTerminalEvent& Event : NextScript.NonTerminalEvents) {
        CorrelateEvent(Event, Request, SequenceNumber);
        Sink(Event);
        ++SequenceNumber;
    }

    CorrelateEvent(NextScript.Terminal, Request, SequenceNumber);
    return std::move(NextScript.Terminal);
}

ProviderResult<ProviderHealthStatus>
FakeProvider::HealthCheck(const ModelProviderProfile&) {
    return ProviderHealthStatus{ProviderHealthState::Available};
}

} // namespace EmulativeDialog::Providers::Testing
