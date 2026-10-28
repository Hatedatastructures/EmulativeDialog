#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace EmulativeDialog::Agent {

enum class CancelReason : std::uint8_t {
    UserRequested,
    HostShutdown,
    Timeout,
    Superseded
};

enum class CancelLifecycleState : std::uint8_t {
    Idle,
    Requested,
    Acknowledged,
    Unconfirmed,
    Terminal
};

enum class CancelRequestStatus : std::uint8_t {
    Requested,
    AlreadyRequested,
    RejectedAfterTerminal
};

enum class CancelErrorCode : std::uint8_t {
    None,
    TurnAlreadyTerminal
};

enum class ProviderTerminalKind : std::uint8_t {
    Completed,
    Failed,
    ConfirmedCancelled,
    OutcomeUnknown
};

enum class TurnOutcomeKind : std::uint8_t {
    Cancelled,
    OutcomeUnknown,
    ProviderCompleted,
    ProviderFailed
};

struct CancelRequestResult final {
    CancelRequestStatus Status{CancelRequestStatus::Requested};
    CancelErrorCode Error{CancelErrorCode::None};
};

struct CancelEvent final {
    std::uint64_t SequenceNumber{0};
    std::chrono::steady_clock::time_point Timestamp{};
    CancelLifecycleState State{CancelLifecycleState::Idle};
    CancelReason Reason{CancelReason::UserRequested};
    bool RequestMayHaveReachedEndpoint{false};
    std::string StableCode;
};

struct TurnOutcome final {
    TurnOutcomeKind Kind{TurnOutcomeKind::Cancelled};
    CancelReason Reason{CancelReason::UserRequested};
    bool RequestMayHaveReachedEndpoint{false};
    std::string StableCode;
};

// Coordinates one AgentTurn's cancellation lifecycle between a stop_token
// source, an explicit Core cancel call and provider terminal events.
//
// Two-phase semantics:
//  - cancel before MarkRequestDispatched(): Requested -> Acknowledged ->
//    Terminal(Cancelled); no request bytes were sent.
//  - cancel after MarkRequestDispatched(): Requested -> Unconfirmed; the only
//    terminal without a provider answer is OutcomeUnknown, and retry/replay is
//    forbidden.
// Repeated cancel is idempotent; cancel after Terminal is rejected with the
// stable code TurnAlreadyTerminal.
class TurnCancellationCoordinator final {
public:
    using EventSink = std::function<void(const CancelEvent&)>;

    explicit TurnCancellationCoordinator(
        std::stop_token ExternalToken = {}, EventSink Sink = {});
    ~TurnCancellationCoordinator();

    TurnCancellationCoordinator(const TurnCancellationCoordinator&) = delete;
    TurnCancellationCoordinator& operator=(
        const TurnCancellationCoordinator&) = delete;
    TurnCancellationCoordinator(TurnCancellationCoordinator&&) = delete;
    TurnCancellationCoordinator& operator=(
        TurnCancellationCoordinator&&) = delete;

    // Token to hand to the transport/provider call. Explicit RequestCancel
    // signals this source; an external stop also signals this source.
    [[nodiscard]] std::stop_token Token() const noexcept;

    // Marks the point where request bytes may have reached the endpoint.
    void MarkRequestDispatched() noexcept;

    [[nodiscard]] CancelRequestResult RequestCancel(CancelReason Reason);

    // Provider stream hooks. Non-terminal events must be dropped once
    // cancellation was requested or the turn is terminal.
    [[nodiscard]] bool ShouldAcceptNonTerminalEvent() const noexcept;
    void OnProviderTerminal(ProviderTerminalKind Kind);

    // No provider terminal arrived (EOF/drop after dispatch); finalizes the
    // already-unconfirmed cancellation as OutcomeUnknown.
    void FinalizeUnconfirmed();

    [[nodiscard]] bool IsCancelRequested() const noexcept;
    [[nodiscard]] bool IsTerminal() const noexcept;
    [[nodiscard]] bool RequestMayHaveReachedEndpoint() const noexcept;
    [[nodiscard]] CancelLifecycleState State() const noexcept;
    [[nodiscard]] std::optional<TurnOutcome> Outcome() const;

private:
    void Emit(std::vector<CancelEvent>& Events) const;

    mutable std::mutex Mutex;
    std::stop_source Source;
    CancelLifecycleState StateValue{CancelLifecycleState::Idle};
    CancelReason ReasonValue{CancelReason::UserRequested};
    bool CancelEverRequested{false};
    bool RequestDispatched{false};
    std::uint64_t NextSequence{1};
    std::optional<TurnOutcome> OutcomeValue;
    EventSink Sink;
    std::optional<std::stop_callback<std::function<void()>>> StopRegistration;
};

} // namespace EmulativeDialog::Agent
