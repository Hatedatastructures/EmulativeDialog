#include "Source/Agent/TurnCancellationCoordinator.hpp"

#include <utility>
#include <vector>

namespace EmulativeDialog::Agent {
namespace {

std::string OutcomeCode(TurnOutcomeKind Kind) {
    switch (Kind) {
    case TurnOutcomeKind::Cancelled:
        return "Cancelled";
    case TurnOutcomeKind::OutcomeUnknown:
        return "OutcomeUnknown";
    case TurnOutcomeKind::ProviderCompleted:
        return "ProviderCompleted";
    case TurnOutcomeKind::ProviderFailed:
        return "ProviderFailed";
    }
    return "OutcomeUnknown";
}

} // namespace

TurnCancellationCoordinator::TurnCancellationCoordinator(
    std::stop_token ExternalToken, EventSink InSink)
    : Sink(std::move(InSink)) {
    if (ExternalToken.stop_possible()) {
        StopRegistration.emplace(
            ExternalToken, [this] {
                static_cast<void>(
                    RequestCancel(CancelReason::HostShutdown));
            });
    }
    if (ExternalToken.stop_requested()) {
        static_cast<void>(RequestCancel(CancelReason::HostShutdown));
    }
}

TurnCancellationCoordinator::~TurnCancellationCoordinator() = default;

std::stop_token TurnCancellationCoordinator::Token() const noexcept {
    return Source.get_token();
}

void TurnCancellationCoordinator::MarkRequestDispatched() noexcept {
    const std::lock_guard Lock(Mutex);
    RequestDispatched = true;
}

CancelRequestResult TurnCancellationCoordinator::RequestCancel(
    CancelReason Reason) {
    std::vector<CancelEvent> Events;
    CancelRequestResult Result;
    bool SignalStop = false;
    {
        const std::lock_guard Lock(Mutex);
        if (StateValue == CancelLifecycleState::Terminal) {
            if (CancelEverRequested) {
                return CancelRequestResult{
                    .Status = CancelRequestStatus::AlreadyRequested,
                    .Error = CancelErrorCode::None};
            }
            return CancelRequestResult{
                .Status = CancelRequestStatus::RejectedAfterTerminal,
                .Error = CancelErrorCode::TurnAlreadyTerminal};
        }
        if (StateValue != CancelLifecycleState::Idle) {
            return CancelRequestResult{
                .Status = CancelRequestStatus::AlreadyRequested,
                .Error = CancelErrorCode::None};
        }

        Result = CancelRequestResult{.Status = CancelRequestStatus::Requested,
                                     .Error = CancelErrorCode::None};
        CancelEverRequested = true;
        ReasonValue = Reason;
        StateValue = CancelLifecycleState::Requested;
        Events.push_back(CancelEvent{
            .SequenceNumber = NextSequence++,
            .Timestamp = std::chrono::steady_clock::now(),
            .State = CancelLifecycleState::Requested,
            .Reason = ReasonValue,
            .RequestMayHaveReachedEndpoint = RequestDispatched,
            .StableCode = "CancelRequested"});

        if (RequestDispatched) {
            // Request bytes may already be at the endpoint; only an actual
            // provider terminal can resolve the turn.
            StateValue = CancelLifecycleState::Unconfirmed;
            Events.push_back(CancelEvent{
                .SequenceNumber = NextSequence++,
                .Timestamp = std::chrono::steady_clock::now(),
                .State = CancelLifecycleState::Unconfirmed,
                .Reason = ReasonValue,
                .RequestMayHaveReachedEndpoint = true,
                .StableCode = "CancelUnconfirmed"});
        } else {
            // Confirmed pre-send cancellation: no request bytes were sent and
            // no external side effect can exist.
            StateValue = CancelLifecycleState::Acknowledged;
            Events.push_back(CancelEvent{
                .SequenceNumber = NextSequence++,
                .Timestamp = std::chrono::steady_clock::now(),
                .State = CancelLifecycleState::Acknowledged,
                .Reason = ReasonValue,
                .RequestMayHaveReachedEndpoint = false,
                .StableCode = "CancelAcknowledged"});
            StateValue = CancelLifecycleState::Terminal;
            OutcomeValue = TurnOutcome{
                .Kind = TurnOutcomeKind::Cancelled,
                .Reason = ReasonValue,
                .RequestMayHaveReachedEndpoint = false,
                .StableCode = "Cancelled"};
            Events.push_back(CancelEvent{
                .SequenceNumber = NextSequence++,
                .Timestamp = std::chrono::steady_clock::now(),
                .State = CancelLifecycleState::Terminal,
                .Reason = ReasonValue,
                .RequestMayHaveReachedEndpoint = false,
                .StableCode = "Cancelled"});
        }
        SignalStop = true;
    }

    if (SignalStop) {
        Source.request_stop();
    }
    Emit(Events);
    return Result;
}

bool TurnCancellationCoordinator::ShouldAcceptNonTerminalEvent()
    const noexcept {
    const std::lock_guard Lock(Mutex);
    return StateValue == CancelLifecycleState::Idle;
}

void TurnCancellationCoordinator::OnProviderTerminal(
    ProviderTerminalKind Kind) {
    std::vector<CancelEvent> Events;
    {
        const std::lock_guard Lock(Mutex);
        if (StateValue == CancelLifecycleState::Terminal) {
            return; // Late provider terminal cannot override a terminal turn.
        }
        TurnOutcomeKind OutcomeKind = TurnOutcomeKind::OutcomeUnknown;
        switch (Kind) {
        case ProviderTerminalKind::Completed:
            OutcomeKind = TurnOutcomeKind::ProviderCompleted;
            break;
        case ProviderTerminalKind::Failed:
            OutcomeKind = TurnOutcomeKind::ProviderFailed;
            break;
        case ProviderTerminalKind::ConfirmedCancelled:
            OutcomeKind = TurnOutcomeKind::Cancelled;
            break;
        case ProviderTerminalKind::OutcomeUnknown:
            OutcomeKind = TurnOutcomeKind::OutcomeUnknown;
            break;
        }
        const std::string Code = OutcomeCode(OutcomeKind);
        StateValue = CancelLifecycleState::Terminal;
        OutcomeValue = TurnOutcome{
            .Kind = OutcomeKind,
            .Reason = ReasonValue,
            .RequestMayHaveReachedEndpoint = RequestDispatched,
            .StableCode = Code};
        Events.push_back(CancelEvent{
            .SequenceNumber = NextSequence++,
            .Timestamp = std::chrono::steady_clock::now(),
            .State = CancelLifecycleState::Terminal,
            .Reason = ReasonValue,
            .RequestMayHaveReachedEndpoint = RequestDispatched,
            .StableCode = Code});
    }
    Emit(Events);
}

void TurnCancellationCoordinator::FinalizeUnconfirmed() {
    std::vector<CancelEvent> Events;
    {
        const std::lock_guard Lock(Mutex);
        if (StateValue != CancelLifecycleState::Unconfirmed &&
            !(StateValue == CancelLifecycleState::Requested &&
              RequestDispatched)) {
            return;
        }
        StateValue = CancelLifecycleState::Terminal;
        OutcomeValue = TurnOutcome{
            .Kind = TurnOutcomeKind::OutcomeUnknown,
            .Reason = ReasonValue,
            .RequestMayHaveReachedEndpoint = true,
            .StableCode = "OutcomeUnknown"};
        Events.push_back(CancelEvent{
            .SequenceNumber = NextSequence++,
            .Timestamp = std::chrono::steady_clock::now(),
            .State = CancelLifecycleState::Terminal,
            .Reason = ReasonValue,
            .RequestMayHaveReachedEndpoint = true,
            .StableCode = "OutcomeUnknown"});
    }
    Emit(Events);
}

bool TurnCancellationCoordinator::IsCancelRequested() const noexcept {
    const std::lock_guard Lock(Mutex);
    return CancelEverRequested;
}

bool TurnCancellationCoordinator::IsTerminal() const noexcept {
    const std::lock_guard Lock(Mutex);
    return StateValue == CancelLifecycleState::Terminal;
}

bool TurnCancellationCoordinator::RequestMayHaveReachedEndpoint()
    const noexcept {
    const std::lock_guard Lock(Mutex);
    return RequestDispatched;
}

CancelLifecycleState TurnCancellationCoordinator::State() const noexcept {
    const std::lock_guard Lock(Mutex);
    return StateValue;
}

std::optional<TurnOutcome> TurnCancellationCoordinator::Outcome() const {
    const std::lock_guard Lock(Mutex);
    return OutcomeValue;
}

void TurnCancellationCoordinator::Emit(
    std::vector<CancelEvent>& Events) const {
    if (!Sink) {
        return;
    }
    for (const CancelEvent& Event : Events) {
        Sink(Event);
    }
}

} // namespace EmulativeDialog::Agent
