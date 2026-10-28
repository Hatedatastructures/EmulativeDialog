#include "Source/Agent/ProviderOutcomePolicy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>

namespace EmulativeDialog::Agent {
namespace {

using Platform::Network::TransportFailureCode;
using Platform::Network::TransportTimeoutPhase;
using Providers::FailureCode;

std::uint32_t Mix(std::uint32_t Seed, std::uint32_t Attempt) noexcept {
    std::uint32_t Value = Seed + Attempt * 0x9E3779B9U;
    Value ^= Value >> 16U;
    Value *= 0x7FEB352DU;
    Value ^= Value >> 15U;
    Value *= 0x846CA68BU;
    Value ^= Value >> 16U;
    return Value;
}

std::chrono::milliseconds Clamp(std::chrono::milliseconds Value,
                                std::chrono::milliseconds Upper) noexcept {
    if (Value < std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds::zero();
    }
    if (Value > Upper) {
        return Upper;
    }
    return Value;
}

} // namespace

std::string_view RetryReasonCodeName(RetryDecisionReason Reason) noexcept {
    switch (Reason) {
    case RetryDecisionReason::RetryScheduled:
        return "RetryScheduled";
    case RetryDecisionReason::Exhausted:
        return "RetryBudgetExhausted";
    case RetryDecisionReason::NonRetryable:
        return "NonRetryable";
    case RetryDecisionReason::UnsafeSideEffects:
        return "UnsafeSideEffects";
    case RetryDecisionReason::Cancelled:
        return "Cancelled";
    case RetryDecisionReason::TerminalAlreadyEmitted:
        return "TerminalAlreadyEmitted";
    }
    return "Unknown";
}

ProviderOutcomePolicy::ProviderOutcomePolicy(RetryPolicyConfig InConfig,
                                             Clock InNow, Sleeper InSleep)
    : Config(std::move(InConfig)),
      Now(InNow ? std::move(InNow)
                : [] {
                      return std::chrono::steady_clock::now();
                  }),
      Sleep(InSleep ? std::move(InSleep)
                    : [](std::chrono::milliseconds Delay,
                         std::stop_token Token) {
                          if (Token.stop_requested()) {
                              return false;
                          }
                          std::this_thread::sleep_for(Delay);
                          return !Token.stop_requested();
                      }) {
    Config.MaximumAttempts = std::max<std::uint32_t>(1, Config.MaximumAttempts);
    Config.MaximumDelay =
        std::max(Config.MaximumDelay, std::chrono::milliseconds::zero());
    Config.BaseDelay = Clamp(Config.BaseDelay, Config.MaximumDelay);
    Config.JitterRatio = std::clamp(Config.JitterRatio, 0.0, 1.0);
}

ProviderOutcomeClass ProviderOutcomePolicy::Classify(
    const ProviderOutcomeInput& Input) noexcept {
    if (Input.CancelledAfterSendUnconfirmed) {
        return ProviderOutcomeClass::UnknownOutcomeNoRetry;
    }
    if (Input.Error.has_value()) {
        switch (Input.Error->Code) {
        case FailureCode::AuthenticationFailure:
            return ProviderOutcomeClass::Authentication;
        case FailureCode::RateLimited:
            return ProviderOutcomeClass::RetryableRateLimit;
        case FailureCode::TransportFailure:
            return ProviderOutcomeClass::RetryableTransport;
        case FailureCode::ContentFiltered:
            return ProviderOutcomeClass::ContentFiltered;
        case FailureCode::InvalidStream:
            return ProviderOutcomeClass::InvalidStream;
        case FailureCode::InvalidProfile:
        case FailureCode::InvalidRequest:
        case FailureCode::UnsupportedCapability:
        case FailureCode::ProviderRejected:
            return ProviderOutcomeClass::NonRetryableClient;
        }
    }
    if (Input.TransportFailure.has_value()) {
        switch (*Input.TransportFailure) {
        case TransportFailureCode::DnsFailure:
        case TransportFailureCode::ConnectFailure:
            return ProviderOutcomeClass::RetryableTransport;
        case TransportFailureCode::ProxyAuthenticationRequired:
            return ProviderOutcomeClass::Authentication;
        case TransportFailureCode::RequestWriteFailure:
        case TransportFailureCode::ResponseReadFailure:
            return ProviderOutcomeClass::UnknownOutcomeNoRetry;
        case TransportFailureCode::InvalidRequest:
        case TransportFailureCode::EndpointNotGranted:
        case TransportFailureCode::TlsHandshakeFailure:
        case TransportFailureCode::ResponseLimitExceeded:
        case TransportFailureCode::HeaderLimitExceeded:
        case TransportFailureCode::InvalidHttpResponse:
        case TransportFailureCode::InternalError:
            return ProviderOutcomeClass::NonRetryableClient;
        }
    }
    if (Input.Timeout.has_value()) {
        switch (*Input.Timeout) {
        case TransportTimeoutPhase::Connect:
            return ProviderOutcomeClass::RetryableTransport;
        case TransportTimeoutPhase::Send:
        case TransportTimeoutPhase::Response:
        case TransportTimeoutPhase::Idle:
            return ProviderOutcomeClass::UnknownOutcomeNoRetry;
        }
    }
    if (Input.CancelledBeforeSend) {
        return ProviderOutcomeClass::NonRetryableClient;
    }
    if (Input.RetryAfter.has_value()) {
        return ProviderOutcomeClass::RetryableRateLimit;
    }
    return ProviderOutcomeClass::NonRetryableClient;
}

std::chrono::milliseconds ProviderOutcomePolicy::ComputeBackoff(
    std::uint32_t Attempt) const noexcept {
    const std::uint32_t Shift = std::min<std::uint32_t>(
        Attempt == 0 ? 0 : Attempt - 1, 20);
    const auto Multiplier = static_cast<std::int64_t>(
        std::uint64_t{1} << Shift);
    const auto Base = Config.BaseDelay.count();
    const std::int64_t Doubled = Base * Multiplier;
    std::int64_t Raw = Doubled;
    const auto Maximum = std::max<std::int64_t>(
        0, Config.MaximumDelay.count());
    if (Raw > Maximum) {
        Raw = Maximum;
    }

    if (Config.JitterRatio > 0.0 && Raw > 0) {
        const std::uint32_t Hash = Mix(Config.JitterSeed, Attempt + 1U);
        const double Unit =
            static_cast<double>(Hash) / 4294967295.0;
        const double Factor =
            1.0 - Config.JitterRatio + 2.0 * Config.JitterRatio * Unit;
        Raw = static_cast<std::int64_t>(
            std::llround(static_cast<double>(Raw) * Factor));
        if (Raw < 0) {
            Raw = 0;
        }
        if (Raw > Maximum) {
            Raw = Maximum;
        }
    }
    return std::chrono::milliseconds(Raw);
}

RetryDecision ProviderOutcomePolicy::Decide(
    const ProviderOutcomeInput& Input, const RetrySafety& Safety,
    std::uint32_t Attempt, std::stop_token CancellationToken,
    bool TerminalAlreadyEmitted) const {
    RetryDecision Decision;
    Decision.Classification = Classify(Input);
    Decision.Attempt = Attempt;
    Decision.DecidedAt = Now();

    const auto Stop = [&Decision](RetryDecisionReason Reason) {
        Decision.Retry = false;
        Decision.Reason = Reason;
        Decision.NextDelay = std::chrono::milliseconds::zero();
        return Decision;
    };

    if (CancellationToken.stop_requested() || Input.CancelledBeforeSend) {
        return Stop(RetryDecisionReason::Cancelled);
    }
    if (TerminalAlreadyEmitted) {
        return Stop(RetryDecisionReason::TerminalAlreadyEmitted);
    }
    if (!Safety.RequestIdempotent || Safety.ToolActionExecuted ||
        Safety.PartialOutputDelivered || Safety.UnknownOutcome) {
        return Stop(RetryDecisionReason::UnsafeSideEffects);
    }

    const bool ClassRetryable =
        Decision.Classification == ProviderOutcomeClass::RetryableTransport ||
        Decision.Classification == ProviderOutcomeClass::RetryableRateLimit;
    if (!ClassRetryable) {
        return Stop(RetryDecisionReason::NonRetryable);
    }
    if (Attempt >= Config.MaximumAttempts) {
        return Stop(RetryDecisionReason::Exhausted);
    }

    std::chrono::milliseconds Delay = ComputeBackoff(Attempt);
    if (Input.RetryAfter.has_value()) {
        const auto ServerDelay =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                *Input.RetryAfter);
        if (ServerDelay > Delay) {
            Delay = ServerDelay;
        }
    }
    Decision.Retry = true;
    Decision.Reason = RetryDecisionReason::RetryScheduled;
    Decision.NextDelay = std::max(Delay, std::chrono::milliseconds::zero());
    return Decision;
}

bool ProviderOutcomePolicy::WaitForRetry(
    const RetryDecision& Decision, std::stop_token CancellationToken) const {
    if (!Decision.Retry || Decision.NextDelay <= std::chrono::milliseconds(0)) {
        return Decision.Retry && !CancellationToken.stop_requested();
    }
    if (CancellationToken.stop_requested()) {
        return false;
    }
    return Sleep(Decision.NextDelay, CancellationToken) &&
           !CancellationToken.stop_requested();
}

} // namespace EmulativeDialog::Agent
