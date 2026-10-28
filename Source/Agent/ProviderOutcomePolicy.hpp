#pragma once

#include "Source/Platform/Network/HttpTransportPort.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>

namespace EmulativeDialog::Agent {

// Closed classification of provider outcomes for retry decisions. The class is
// protocol neutral: adapters report canonical ProviderError values and/or the
// transport terminal; the policy owns retry safety.
enum class ProviderOutcomeClass : std::uint8_t {
    RetryableTransport,
    RetryableRateLimit,
    NonRetryableClient,
    Authentication,
    ContentFiltered,
    InvalidStream,
    UnknownOutcomeNoRetry,
};

// Facts about the attempt that just failed. Any unsafe fact permanently blocks
// automatic resubmission of the same request.
struct RetrySafety final {
    bool RequestIdempotent{true};
    bool ToolActionExecuted{false};
    bool PartialOutputDelivered{false};
    bool UnknownOutcome{false};
};

struct RetryPolicyConfig final {
    // Total attempts including the initial attempt.
    std::uint32_t MaximumAttempts{3};
    std::chrono::milliseconds BaseDelay{200};
    std::chrono::milliseconds MaximumDelay{5'000};
    // Fractional jitter in [0, 1]; clamped on construction.
    double JitterRatio{0.2};
    std::uint32_t JitterSeed{0x9E3779B9U};
};

struct ProviderOutcomeInput final {
    std::optional<Providers::ProviderError> Error;
    std::optional<Platform::Network::TransportFailureCode> TransportFailure;
    std::optional<Platform::Network::TransportTimeoutPhase> Timeout;
    // Honored only up to RetryPolicyConfig::MaximumDelay. A server-provided
    // delay never bypasses the local retry ceiling.
    std::optional<std::chrono::seconds> RetryAfter;
    bool CancelledBeforeSend{false};
    bool CancelledAfterSendUnconfirmed{false};
};

enum class RetryDecisionReason : std::uint8_t {
    RetryScheduled,
    Exhausted,
    NonRetryable,
    UnsafeSideEffects,
    Cancelled,
    TerminalAlreadyEmitted,
    InvalidAttempt,
};

[[nodiscard]] std::string_view RetryReasonCodeName(
    RetryDecisionReason Reason) noexcept;

struct RetryDecision final {
    ProviderOutcomeClass Classification{ProviderOutcomeClass::NonRetryableClient};
    bool Retry{false};
    // 1-based number of the attempt that just failed.
    std::uint32_t Attempt{0};
    std::chrono::milliseconds NextDelay{0};
    RetryDecisionReason Reason{RetryDecisionReason::NonRetryable};
    std::chrono::steady_clock::time_point DecidedAt{};
};

class ProviderOutcomePolicy final {
public:
    using Clock =
        std::function<std::chrono::steady_clock::time_point()>;
    // Returns false when the wait was interrupted by cancellation.
    using Sleeper = std::function<bool(std::chrono::milliseconds,
                                       std::stop_token)>;

    explicit ProviderOutcomePolicy(
        RetryPolicyConfig Config = {},
        Clock Now = {},
        Sleeper Sleep = {});

    [[nodiscard]] const RetryPolicyConfig& GetConfig() const noexcept {
        return Config;
    }

    // Protocol-neutral classification of a single failed attempt.
    [[nodiscard]] static ProviderOutcomeClass Classify(
        const ProviderOutcomeInput& Input) noexcept;

    // Decides whether the failed attempt may be retried and, if so, how long
    // to wait. Never reschedules when the turn was cancelled, a terminal event
    // already exists, or any unsafe side effect is known.
    [[nodiscard]] RetryDecision Decide(
        const ProviderOutcomeInput& Input,
        const RetrySafety& Safety,
        std::uint32_t Attempt,
        std::stop_token CancellationToken,
        bool TerminalAlreadyEmitted = false) const;

    // Honors the policy delay. A stop request before or during the wait makes
    // this return false so callers stop the retry loop immediately.
    [[nodiscard]] bool WaitForRetry(
        const RetryDecision& Decision,
        std::stop_token CancellationToken) const;

private:
    [[nodiscard]] std::chrono::milliseconds ComputeBackoff(
        std::uint32_t Attempt) const noexcept;

    RetryPolicyConfig Config;
    Clock Now;
    Sleeper Sleep;
};

} // namespace EmulativeDialog::Agent
