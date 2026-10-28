#include "Source/Agent/ProviderOutcomePolicy.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace {

using namespace EmulativeDialog;
using namespace EmulativeDialog::Agent;
using namespace EmulativeDialog::Platform::Network;
using namespace EmulativeDialog::Providers;

int Failures = 0;

void Check(bool Condition, std::string_view Description) {
    if (!Condition) {
        ++Failures;
        std::fprintf(stderr, "FAILED: %.*s\n",
                     static_cast<int>(Description.size()), Description.data());
    }
}

ProviderOutcomeInput WithError(FailureCode Code) {
    ProviderOutcomeInput Input;
    Input.Error = ProviderError{Code, false, "fixture"};
    return Input;
}

RetryPolicyConfig NoJitterConfig() {
    RetryPolicyConfig Config;
    Config.MaximumAttempts = 3;
    Config.BaseDelay = std::chrono::milliseconds(100);
    Config.MaximumDelay = std::chrono::milliseconds(500);
    Config.JitterRatio = 0.0;
    return Config;
}

void TestProviderErrorClassification() {
    const struct {
        FailureCode Code;
        ProviderOutcomeClass Expected;
    } Cases[] = {
        {FailureCode::AuthenticationFailure,
         ProviderOutcomeClass::Authentication},
        {FailureCode::RateLimited,
         ProviderOutcomeClass::RetryableRateLimit},
        {FailureCode::TransportFailure,
         ProviderOutcomeClass::RetryableTransport},
        {FailureCode::ContentFiltered,
         ProviderOutcomeClass::ContentFiltered},
        {FailureCode::InvalidStream,
         ProviderOutcomeClass::InvalidStream},
        {FailureCode::InvalidProfile,
         ProviderOutcomeClass::NonRetryableClient},
        {FailureCode::InvalidRequest,
         ProviderOutcomeClass::NonRetryableClient},
        {FailureCode::UnsupportedCapability,
         ProviderOutcomeClass::NonRetryableClient},
        {FailureCode::ProviderRejected,
         ProviderOutcomeClass::NonRetryableClient},
    };
    for (const auto& Current : Cases) {
        Check(ProviderOutcomePolicy::Classify(WithError(Current.Code)) ==
                  Current.Expected,
              "provider error classification");
    }
}

void TestTransportClassification() {
    const struct {
        TransportFailureCode Code;
        ProviderOutcomeClass Expected;
    } Cases[] = {
        {TransportFailureCode::DnsFailure,
         ProviderOutcomeClass::RetryableTransport},
        {TransportFailureCode::ConnectFailure,
         ProviderOutcomeClass::RetryableTransport},
        {TransportFailureCode::ProxyAuthenticationRequired,
         ProviderOutcomeClass::Authentication},
        {TransportFailureCode::RequestWriteFailure,
         ProviderOutcomeClass::UnknownOutcomeNoRetry},
        {TransportFailureCode::ResponseReadFailure,
         ProviderOutcomeClass::UnknownOutcomeNoRetry},
        {TransportFailureCode::InvalidRequest,
         ProviderOutcomeClass::NonRetryableClient},
        {TransportFailureCode::EndpointNotGranted,
         ProviderOutcomeClass::NonRetryableClient},
        {TransportFailureCode::TlsHandshakeFailure,
         ProviderOutcomeClass::NonRetryableClient},
        {TransportFailureCode::ResponseLimitExceeded,
         ProviderOutcomeClass::NonRetryableClient},
        {TransportFailureCode::HeaderLimitExceeded,
         ProviderOutcomeClass::NonRetryableClient},
        {TransportFailureCode::InvalidHttpResponse,
         ProviderOutcomeClass::NonRetryableClient},
        {TransportFailureCode::InternalError,
         ProviderOutcomeClass::NonRetryableClient},
    };
    for (const auto& Current : Cases) {
        ProviderOutcomeInput Input;
        Input.TransportFailure = Current.Code;
        Check(ProviderOutcomePolicy::Classify(Input) == Current.Expected,
              "transport failure classification");
    }
}

void TestTimeoutAndFlagClassification() {
    {
        ProviderOutcomeInput Input;
        Input.Timeout = TransportTimeoutPhase::Connect;
        Check(ProviderOutcomePolicy::Classify(Input) ==
                  ProviderOutcomeClass::RetryableTransport,
              "connect timeout retryable");
    }
    for (const auto Phase : {TransportTimeoutPhase::Send,
                             TransportTimeoutPhase::Response,
                             TransportTimeoutPhase::Idle}) {
        ProviderOutcomeInput Input;
        Input.Timeout = Phase;
        Check(ProviderOutcomePolicy::Classify(Input) ==
                  ProviderOutcomeClass::UnknownOutcomeNoRetry,
              "post-send timeout is unknown outcome");
    }
    {
        ProviderOutcomeInput Input;
        Input.CancelledAfterSendUnconfirmed = true;
        Check(ProviderOutcomePolicy::Classify(Input) ==
                  ProviderOutcomeClass::UnknownOutcomeNoRetry,
              "unconfirmed cancellation is unknown outcome");
    }
    {
        ProviderOutcomeInput Input;
        Input.CancelledBeforeSend = true;
        Check(ProviderOutcomePolicy::Classify(Input) ==
                  ProviderOutcomeClass::NonRetryableClient,
              "cancelled before send is non-retryable");
    }
    {
        ProviderOutcomeInput Input;
        Input.RetryAfter = std::chrono::seconds(1);
        Check(ProviderOutcomePolicy::Classify(Input) ==
                  ProviderOutcomeClass::RetryableRateLimit,
              "retry-after implies rate limit");
    }
    Check(ProviderOutcomePolicy::Classify(ProviderOutcomeInput{}) ==
              ProviderOutcomeClass::NonRetryableClient,
          "empty input is non-retryable");
}

void TestRetryableBackoffSequence() {
    ProviderOutcomePolicy Policy(NoJitterConfig());
    const ProviderOutcomeInput RateLimited =
        WithError(FailureCode::RateLimited);
    RetrySafety Safety;

    const RetryDecision First =
        Policy.Decide(RateLimited, Safety, 1, std::stop_token{});
    Check(First.Retry && First.Reason == RetryDecisionReason::RetryScheduled,
          "first rate limit retry scheduled");
    Check(First.NextDelay == std::chrono::milliseconds(100),
          "first backoff uses base delay");

    const RetryDecision Second =
        Policy.Decide(RateLimited, Safety, 2, std::stop_token{});
    Check(Second.Retry &&
              Second.NextDelay == std::chrono::milliseconds(200),
          "second backoff doubles");

    const RetryDecision Third =
        Policy.Decide(RateLimited, Safety, 3, std::stop_token{});
    Check(!Third.Retry && Third.Reason == RetryDecisionReason::Exhausted,
          "attempt budget exhausted");

    RetryPolicyConfig ManyAttempts = NoJitterConfig();
    ManyAttempts.MaximumAttempts = 20;
    ManyAttempts.MaximumDelay = std::chrono::milliseconds(500);
    ProviderOutcomePolicy Capped(ManyAttempts);
    const RetryDecision CappedDecision =
        Capped.Decide(RateLimited, Safety, 10, std::stop_token{});
    Check(CappedDecision.Retry &&
              CappedDecision.NextDelay == std::chrono::milliseconds(500),
          "backoff honors maximum delay");
}

void TestJitterBounds() {
    RetryPolicyConfig Config = NoJitterConfig();
    Config.JitterRatio = 0.5;
    Config.JitterSeed = 0x12345678U;
    ProviderOutcomePolicy Policy(Config);
    const ProviderOutcomeInput TransportError =
        WithError(FailureCode::TransportFailure);
    const RetryDecision Decision =
        Policy.Decide(TransportError, RetrySafety{}, 1, std::stop_token{});
    Check(Decision.Retry, "jittered retry scheduled");
    Check(Decision.NextDelay >= std::chrono::milliseconds(50) &&
              Decision.NextDelay <= std::chrono::milliseconds(150),
          "jitter stays within configured ratio");

    ProviderOutcomePolicy SameConfig(Config);
    const RetryDecision Same =
        SameConfig.Decide(TransportError, RetrySafety{}, 1, std::stop_token{});
    Check(Same.NextDelay == Decision.NextDelay,
          "jitter is deterministic for a fixed seed");
}

void TestRetryAfterHandling() {
    ProviderOutcomePolicy Policy(NoJitterConfig());
    ProviderOutcomeInput RateLimited =
        WithError(FailureCode::RateLimited);
    RateLimited.RetryAfter = std::chrono::seconds(7);
    const RetryDecision Decision =
        Policy.Decide(RateLimited, RetrySafety{}, 1, std::stop_token{});
    Check(Decision.Retry, "retry-after schedules a retry");
    Check(Decision.NextDelay == std::chrono::seconds(7),
          "retry-after overrides smaller backoff");

    RateLimited.RetryAfter = std::chrono::seconds(0);
    const RetryDecision Small =
        Policy.Decide(RateLimited, RetrySafety{}, 1, std::stop_token{});
    Check(Small.NextDelay == std::chrono::milliseconds(100),
          "small retry-after keeps computed backoff");
}

void TestSafetyGates() {
    ProviderOutcomePolicy Policy(NoJitterConfig());
    const ProviderOutcomeInput Input = WithError(FailureCode::TransportFailure);
    const struct {
        RetrySafety Safety;
        std::string_view Description;
    } Cases[] = {
        {RetrySafety{false, false, false, false}, "non-idempotent"},
        {RetrySafety{true, true, false, false}, "tool action executed"},
        {RetrySafety{true, false, true, false}, "partial output delivered"},
        {RetrySafety{true, false, false, true}, "unknown outcome"},
    };
    for (const auto& Current : Cases) {
        const RetryDecision Decision =
            Policy.Decide(Input, Current.Safety, 1, std::stop_token{});
        Check(!Decision.Retry &&
                  Decision.Reason == RetryDecisionReason::UnsafeSideEffects,
              Current.Description);
    }
}

void TestCancellationAndTerminalInteraction() {
    ProviderOutcomePolicy Policy(NoJitterConfig());
    const ProviderOutcomeInput Input = WithError(FailureCode::TransportFailure);
    {
        std::stop_source Source;
        Source.request_stop();
        const RetryDecision Decision =
            Policy.Decide(Input, RetrySafety{}, 1, Source.get_token());
        Check(!Decision.Retry &&
                  Decision.Reason == RetryDecisionReason::Cancelled,
              "stop token blocks retries");
    }
    {
        ProviderOutcomeInput Cancelled = Input;
        Cancelled.CancelledBeforeSend = true;
        const RetryDecision Decision =
            Policy.Decide(Cancelled, RetrySafety{}, 1, std::stop_token{});
        Check(!Decision.Retry &&
                  Decision.Reason == RetryDecisionReason::Cancelled,
              "cancelled-before-send blocks retries");
    }
    {
        const RetryDecision Decision =
            Policy.Decide(Input, RetrySafety{}, 1, std::stop_token{}, true);
        Check(!Decision.Retry &&
                  Decision.Reason ==
                      RetryDecisionReason::TerminalAlreadyEmitted,
              "terminal event blocks retries");
    }
}

void TestNonRetryableOutcomes() {
    ProviderOutcomePolicy Policy(NoJitterConfig());
    for (const auto Code :
         {FailureCode::AuthenticationFailure, FailureCode::ContentFiltered,
          FailureCode::InvalidStream, FailureCode::InvalidRequest,
          FailureCode::ProviderRejected}) {
        const RetryDecision Decision =
            Policy.Decide(WithError(Code), RetrySafety{}, 1,
                          std::stop_token{});
        Check(!Decision.Retry &&
                  Decision.Reason == RetryDecisionReason::NonRetryable,
              "non-retryable error schedules no retry");
    }
    ProviderOutcomeInput Unknown;
    Unknown.TransportFailure = TransportFailureCode::RequestWriteFailure;
    const RetryDecision Decision =
        Policy.Decide(Unknown, RetrySafety{}, 1, std::stop_token{});
    Check(!Decision.Retry &&
              Decision.Classification ==
                  ProviderOutcomeClass::UnknownOutcomeNoRetry,
          "unknown outcome never retries");
}

void TestWaitForRetry() {
    std::chrono::milliseconds Observed{0};
    ProviderOutcomePolicy Policy(
        NoJitterConfig(),
        [] {
            return std::chrono::steady_clock::time_point{
                std::chrono::seconds(42)};
        },
        [&Observed](std::chrono::milliseconds Delay, std::stop_token) {
            Observed = Delay;
            return true;
        });
    const RetryDecision Decision =
        Policy.Decide(WithError(FailureCode::RateLimited), RetrySafety{}, 1,
                      std::stop_token{});
    Check(Policy.WaitForRetry(Decision, std::stop_token{}),
          "wait completes when not cancelled");
    Check(Observed == Decision.NextDelay, "wait uses the decided delay");

    std::stop_source Source;
    ProviderOutcomePolicy CancellingPolicy(
        NoJitterConfig(), {},
        [&Source](std::chrono::milliseconds, std::stop_token) {
            Source.request_stop();
            return true;
        });
    const RetryDecision Pending =
        CancellingPolicy.Decide(WithError(FailureCode::RateLimited),
                                RetrySafety{}, 1, Source.get_token());
    Check(!CancellingPolicy.WaitForRetry(Pending, Source.get_token()),
          "cancellation during the wait stops retries");

    RetryDecision NoRetry;
    NoRetry.Retry = false;
    Check(!Policy.WaitForRetry(NoRetry, std::stop_token{}),
          "non-retry decision does not wait");

    RetryDecision ZeroDelay;
    ZeroDelay.Retry = true;
    ZeroDelay.NextDelay = std::chrono::milliseconds(0);
    Check(Policy.WaitForRetry(ZeroDelay, std::stop_token{}),
          "zero-delay retry succeeds without sleeping");
}

void TestClockAndConfigurationNormalization() {
    const auto Fixed = std::chrono::steady_clock::time_point{
        std::chrono::seconds(1234)};
    RetryPolicyConfig Config = NoJitterConfig();
    Config.MaximumAttempts = 0;
    Config.JitterRatio = 5.0;
    Config.MaximumDelay = std::chrono::milliseconds(-10);
    ProviderOutcomePolicy Policy(
        Config, [Fixed] { return Fixed; });
    Check(Policy.GetConfig().MaximumAttempts == 1,
          "maximum attempts normalized to at least one");
    Check(Policy.GetConfig().JitterRatio == 1.0,
          "jitter ratio clamped");
    Check(Policy.GetConfig().MaximumDelay == std::chrono::milliseconds(0),
          "negative maximum delay normalized");
    const RetryDecision Decision =
        Policy.Decide(WithError(FailureCode::TransportFailure), RetrySafety{},
                      1, std::stop_token{});
    Check(Decision.DecidedAt == Fixed, "injected clock is used");
    Check(!Decision.Retry &&
              Decision.Reason == RetryDecisionReason::Exhausted,
          "single attempt budget exhausted");

    RetryPolicyConfig NegativeJitter = NoJitterConfig();
    NegativeJitter.JitterRatio = -2.0;
    ProviderOutcomePolicy Clamped(NegativeJitter);
    Check(Clamped.GetConfig().JitterRatio == 0.0,
          "negative jitter clamped to zero");
}

void TestReasonCodes() {
    const struct {
        RetryDecisionReason Reason;
        std::string_view Code;
    } Cases[] = {
        {RetryDecisionReason::RetryScheduled, "RetryScheduled"},
        {RetryDecisionReason::Exhausted, "RetryBudgetExhausted"},
        {RetryDecisionReason::NonRetryable, "NonRetryable"},
        {RetryDecisionReason::UnsafeSideEffects, "UnsafeSideEffects"},
        {RetryDecisionReason::Cancelled, "Cancelled"},
        {RetryDecisionReason::TerminalAlreadyEmitted,
         "TerminalAlreadyEmitted"},
    };
    for (const auto& Current : Cases) {
        Check(RetryReasonCodeName(Current.Reason) == Current.Code,
              "stable reason code");
    }
}

} // namespace

int main() {
    TestProviderErrorClassification();
    TestTransportClassification();
    TestTimeoutAndFlagClassification();
    TestRetryableBackoffSequence();
    TestJitterBounds();
    TestRetryAfterHandling();
    TestSafetyGates();
    TestCancellationAndTerminalInteraction();
    TestNonRetryableOutcomes();
    TestWaitForRetry();
    TestClockAndConfigurationNormalization();
    TestReasonCodes();

    if (Failures != 0) {
        std::fprintf(stderr, "%d assertion(s) failed\n", Failures);
        return 1;
    }
    std::puts("ProviderOutcomePolicy tests passed");
    return 0;
}
