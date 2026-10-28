#include "Source/Agent/TurnCancellationCoordinator.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

using EmulativeDialog::Agent::CancelErrorCode;
using EmulativeDialog::Agent::CancelEvent;
using EmulativeDialog::Agent::CancelLifecycleState;
using EmulativeDialog::Agent::CancelReason;
using EmulativeDialog::Agent::CancelRequestStatus;
using EmulativeDialog::Agent::ProviderTerminalKind;
using EmulativeDialog::Agent::TurnCancellationCoordinator;
using EmulativeDialog::Agent::TurnOutcomeKind;

void Expect(bool Condition, const char* File = __builtin_FILE(),
            int Line = __builtin_LINE()) {
    if (!Condition) {
        std::fprintf(stderr, "EXPECT failed at %s:%d\n", File, Line);
        std::abort();
    }
}

class EventRecorder final {
public:
    [[nodiscard]] TurnCancellationCoordinator::EventSink Sink() {
        return [this](const CancelEvent& Event) {
            const std::lock_guard Lock(Mutex);
            Events.push_back(Event);
        };
    }

    [[nodiscard]] std::vector<CancelEvent> Snapshot() const {
        const std::lock_guard Lock(Mutex);
        return Events;
    }

private:
    mutable std::mutex Mutex;
    std::vector<CancelEvent> Events;
};

void ExpectMonotonic(const std::vector<CancelEvent>& Events) {
    std::uint64_t Expected = 1;
    for (const CancelEvent& Event : Events) {
        Expect(Event.SequenceNumber == Expected);
        ++Expected;
    }
}

void ExpectContiguousSequenceSet(const std::vector<CancelEvent>& Events) {
    std::vector<std::uint64_t> Sequence;
    Sequence.reserve(Events.size());
    for (const CancelEvent& Event : Events) {
        Sequence.push_back(Event.SequenceNumber);
    }
    std::sort(Sequence.begin(), Sequence.end());
    for (std::size_t Index = 0; Index < Sequence.size(); ++Index) {
        Expect(Sequence[Index] == Index + 1);
    }
}

void TestPreSendCancelIsTerminallyCancelled() {
    EventRecorder Recorder;
    TurnCancellationCoordinator Coordinator({}, Recorder.Sink());

    Expect(Coordinator.State() == CancelLifecycleState::Idle);
    Expect(Coordinator.ShouldAcceptNonTerminalEvent());
    const auto Result = Coordinator.RequestCancel(CancelReason::UserRequested);
    Expect(Result.Status == CancelRequestStatus::Requested);
    Expect(Result.Error == CancelErrorCode::None);
    Expect(Coordinator.State() == CancelLifecycleState::Terminal);
    Expect(Coordinator.IsTerminal());
    Expect(Coordinator.IsCancelRequested());
    Expect(!Coordinator.RequestMayHaveReachedEndpoint());
    Expect(!Coordinator.ShouldAcceptNonTerminalEvent());

    const auto Outcome = Coordinator.Outcome();
    Expect(Outcome.has_value());
    Expect(Outcome->Kind == TurnOutcomeKind::Cancelled);
    Expect(Outcome->StableCode == "Cancelled");
    Expect(!Outcome->RequestMayHaveReachedEndpoint);

    const auto Events = Recorder.Snapshot();
    Expect(Events.size() == 3);
    Expect(Events[0].State == CancelLifecycleState::Requested);
    Expect(Events[0].StableCode == "CancelRequested");
    Expect(Events[1].State == CancelLifecycleState::Acknowledged);
    Expect(Events[1].StableCode == "CancelAcknowledged");
    Expect(Events[2].State == CancelLifecycleState::Terminal);
    Expect(Events[2].StableCode == "Cancelled");
    ExpectMonotonic(Events);

    const auto Repeated = Coordinator.RequestCancel(CancelReason::Timeout);
    Expect(Repeated.Status == CancelRequestStatus::AlreadyRequested);
    Expect(Recorder.Snapshot().size() == 3);
}

void TestPostSendCancelNeedsOutcomeUnknown() {
    EventRecorder Recorder;
    TurnCancellationCoordinator Coordinator({}, Recorder.Sink());
    Coordinator.MarkRequestDispatched();
    Expect(Coordinator.RequestMayHaveReachedEndpoint());

    const auto Result = Coordinator.RequestCancel(CancelReason::HostShutdown);
    Expect(Result.Status == CancelRequestStatus::Requested);
    Expect(Coordinator.State() == CancelLifecycleState::Unconfirmed);
    Expect(!Coordinator.IsTerminal());
    Expect(Coordinator.IsCancelRequested());
    Expect(!Coordinator.Outcome().has_value());
    Expect(!Coordinator.ShouldAcceptNonTerminalEvent());

    const auto Events = Recorder.Snapshot();
    Expect(Events.size() == 2);
    Expect(Events[0].State == CancelLifecycleState::Requested);
    Expect(Events[0].RequestMayHaveReachedEndpoint);
    Expect(Events[1].State == CancelLifecycleState::Unconfirmed);
    Expect(Events[1].StableCode == "CancelUnconfirmed");
    ExpectMonotonic(Events);

    Coordinator.FinalizeUnconfirmed();
    Expect(Coordinator.IsTerminal());
    const auto Outcome = Coordinator.Outcome();
    Expect(Outcome.has_value());
    Expect(Outcome->Kind == TurnOutcomeKind::OutcomeUnknown);
    Expect(Outcome->StableCode == "OutcomeUnknown");
    Expect(Outcome->RequestMayHaveReachedEndpoint);
    const auto FinalEvents = Recorder.Snapshot();
    Expect(FinalEvents.size() == 3);
    Expect(FinalEvents.back().StableCode == "OutcomeUnknown");
    ExpectMonotonic(FinalEvents);

    Coordinator.FinalizeUnconfirmed();
    Expect(Recorder.Snapshot().size() == 3);
    const auto Repeated = Coordinator.RequestCancel(CancelReason::Superseded);
    Expect(Repeated.Status == CancelRequestStatus::AlreadyRequested);
}

void TestProviderTerminalResolvesPendingCancellation() {
    {
        EventRecorder Recorder;
        TurnCancellationCoordinator Coordinator({}, Recorder.Sink());
        Coordinator.MarkRequestDispatched();
        static_cast<void>(
            Coordinator.RequestCancel(CancelReason::UserRequested));
        Coordinator.OnProviderTerminal(
            ProviderTerminalKind::ConfirmedCancelled);
        Expect(Coordinator.IsTerminal());
        const auto Outcome = Coordinator.Outcome();
        Expect(Outcome->Kind == TurnOutcomeKind::Cancelled);
        Expect(Outcome->StableCode == "Cancelled");
        Expect(Recorder.Snapshot().size() == 3);
        ExpectMonotonic(Recorder.Snapshot());
    }
    {
        EventRecorder Recorder;
        TurnCancellationCoordinator Coordinator({}, Recorder.Sink());
        Coordinator.MarkRequestDispatched();
        static_cast<void>(
            Coordinator.RequestCancel(CancelReason::Timeout));
        Coordinator.OnProviderTerminal(ProviderTerminalKind::Completed);
        const auto Outcome = Coordinator.Outcome();
        Expect(Outcome->Kind == TurnOutcomeKind::ProviderCompleted);
        Expect(Outcome->StableCode == "ProviderCompleted");
        Expect(Outcome->RequestMayHaveReachedEndpoint);
    }
    {
        EventRecorder Recorder;
        TurnCancellationCoordinator Coordinator({}, Recorder.Sink());
        Coordinator.OnProviderTerminal(ProviderTerminalKind::OutcomeUnknown);
        const auto Outcome = Coordinator.Outcome();
        Expect(Outcome->Kind == TurnOutcomeKind::OutcomeUnknown);
    }
}

void TestCancelAfterProviderTerminalIsRejected() {
    EventRecorder Recorder;
    TurnCancellationCoordinator Coordinator({}, Recorder.Sink());
    Coordinator.OnProviderTerminal(ProviderTerminalKind::Failed);
    Expect(Coordinator.IsTerminal());
    Expect(!Coordinator.IsCancelRequested());
    Expect(!Coordinator.ShouldAcceptNonTerminalEvent());
    Expect(Coordinator.Outcome()->Kind == TurnOutcomeKind::ProviderFailed);

    const auto Result = Coordinator.RequestCancel(CancelReason::UserRequested);
    Expect(Result.Status == CancelRequestStatus::RejectedAfterTerminal);
    Expect(Result.Error == CancelErrorCode::TurnAlreadyTerminal);
    Expect(Recorder.Snapshot().size() == 1);

    Coordinator.OnProviderTerminal(ProviderTerminalKind::Completed);
    Expect(Coordinator.Outcome()->Kind == TurnOutcomeKind::ProviderFailed);
    Expect(Recorder.Snapshot().size() == 1);
}

void TestExternalStopTokenDrivesCancellation() {
    {
        EventRecorder Recorder;
        std::stop_source External;
        TurnCancellationCoordinator Coordinator(External.get_token(),
                                                Recorder.Sink());
        External.request_stop();
        Expect(Coordinator.IsTerminal());
        Expect(Coordinator.Outcome()->Kind == TurnOutcomeKind::Cancelled);
        Expect(!Coordinator.RequestMayHaveReachedEndpoint());
    }
    {
        EventRecorder Recorder;
        std::stop_source External;
        TurnCancellationCoordinator Coordinator(External.get_token(),
                                                Recorder.Sink());
        Coordinator.MarkRequestDispatched();
        External.request_stop();
        Expect(Coordinator.State() == CancelLifecycleState::Unconfirmed);
        Coordinator.FinalizeUnconfirmed();
        Expect(Coordinator.Outcome()->Kind == TurnOutcomeKind::OutcomeUnknown);
    }
    {
        std::stop_source External;
        External.request_stop();
        TurnCancellationCoordinator Coordinator(External.get_token());
        Expect(Coordinator.IsTerminal());
        Expect(Coordinator.Outcome()->Kind == TurnOutcomeKind::Cancelled);
    }
}

void TestCoordinatorTokenSignalsTransport() {
    TurnCancellationCoordinator Coordinator;
    std::atomic<bool> Stopped{false};
    std::stop_callback Callback(Coordinator.Token(),
                                [&Stopped] { Stopped = true; });
    static_cast<void>(
        Coordinator.RequestCancel(CancelReason::UserRequested));
    Expect(Stopped.load());
}

void TestConcurrentCancelAndTerminalRace() {
    for (int Iteration = 0; Iteration < 400; ++Iteration) {
        EventRecorder Recorder;
        TurnCancellationCoordinator Coordinator({}, Recorder.Sink());
        Coordinator.MarkRequestDispatched();

        std::thread Canceller([&Coordinator] {
            static_cast<void>(
                Coordinator.RequestCancel(CancelReason::UserRequested));
            Coordinator.FinalizeUnconfirmed();
        });
        std::thread Terminal([&Coordinator] {
            Coordinator.OnProviderTerminal(ProviderTerminalKind::Completed);
        });
        Canceller.join();
        Terminal.join();

        Expect(Coordinator.IsTerminal());
        const auto Outcome = Coordinator.Outcome();
        Expect(Outcome.has_value());
        Expect(Outcome->Kind == TurnOutcomeKind::OutcomeUnknown ||
               Outcome->Kind == TurnOutcomeKind::ProviderCompleted);
        const auto Events = Recorder.Snapshot();
        Expect(!Events.empty());
        // Concurrent emitters may deliver out of wall-clock order; sequence
        // numbers remain unique and contiguous as the ordering authority.
        ExpectContiguousSequenceSet(Events);
        int TerminalCount = 0;
        for (const CancelEvent& Event : Events) {
            if (Event.State == CancelLifecycleState::Terminal) {
                ++TerminalCount;
            }
        }
        Expect(TerminalCount == 1);
        Expect(!Coordinator.ShouldAcceptNonTerminalEvent());
    }
}

void TestSinkReentrancyDoesNotDeadlock() {
    TurnCancellationCoordinator* CoordinatorPtr = nullptr;
    int Calls = 0;
    TurnCancellationCoordinator Coordinator(
        {}, [&CoordinatorPtr, &Calls](const CancelEvent&) {
            ++Calls;
            if (CoordinatorPtr != nullptr) {
                static_cast<void>(
                    CoordinatorPtr->RequestCancel(CancelReason::Superseded));
            }
        });
    CoordinatorPtr = &Coordinator;
    static_cast<void>(
        Coordinator.RequestCancel(CancelReason::UserRequested));
    Expect(Calls >= 1);
    Expect(Coordinator.IsTerminal());
}

void TestDispatchAfterTerminalKeepsAcceptedEventsClosed() {
    TurnCancellationCoordinator Coordinator;
    static_cast<void>(
        Coordinator.RequestCancel(CancelReason::UserRequested));
    Expect(Coordinator.IsTerminal());
    Coordinator.MarkRequestDispatched();
    Expect(!Coordinator.ShouldAcceptNonTerminalEvent());
    Expect(!Coordinator.Outcome()->RequestMayHaveReachedEndpoint);
}

} // namespace

int main() {
    TestPreSendCancelIsTerminallyCancelled();
    TestPostSendCancelNeedsOutcomeUnknown();
    TestProviderTerminalResolvesPendingCancellation();
    TestCancelAfterProviderTerminalIsRejected();
    TestExternalStopTokenDrivesCancellation();
    TestCoordinatorTokenSignalsTransport();
    TestConcurrentCancelAndTerminalRace();
    TestSinkReentrancyDoesNotDeadlock();
    TestDispatchAfterTerminalKeepsAcceptedEventsClosed();
    return 0;
}
