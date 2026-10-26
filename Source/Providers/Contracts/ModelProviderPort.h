#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace EmulativeDialog::Providers {

template <typename Tag>
struct Identifier final {
    std::string Value;

    friend bool operator==(const Identifier&, const Identifier&) = default;
};

struct ProviderProfileIdTag final {};
struct CredentialRefTag final {};
struct CompatibilityProfileIdTag final {};
struct RegisteredOptionIdTag final {};
struct RequestIdTag final {};
struct AgentTurnIdTag final {};
struct ConversationScopeRefTag final {};
struct ProviderResponseIdTag final {};
struct ItemIdTag final {};
struct ToolCallIdTag final {};

using ProviderProfileId = Identifier<ProviderProfileIdTag>;
using CredentialRef = Identifier<CredentialRefTag>;
using CompatibilityProfileId = Identifier<CompatibilityProfileIdTag>;
using RegisteredOptionId = Identifier<RegisteredOptionIdTag>;
using RequestId = Identifier<RequestIdTag>;
using AgentTurnId = Identifier<AgentTurnIdTag>;
using ConversationScopeRef = Identifier<ConversationScopeRefTag>;
using ProviderResponseId = Identifier<ProviderResponseIdTag>;
using ItemId = Identifier<ItemIdTag>;
using ToolCallId = Identifier<ToolCallIdTag>;

enum class ProviderProtocol : std::uint8_t {
    OpenAIResponses,
    OpenAICompatibleChatCompletions,
    AnthropicMessages
};

enum class AuthenticationScheme : std::uint8_t {
    Bearer,
    XApiKeyHeader
};

struct ProviderCapabilities final {
    bool Streaming{};
    bool ToolCalls{};
    bool Vision{};
    bool Usage{};
    bool Cancellation{};
    bool OutputLimit{};
};

struct SamplingOptions final {
    std::optional<double> Temperature;
    std::optional<double> TopP;
    std::optional<double> TopK;
};

struct OutputLimit final {
    std::optional<std::uint32_t> MaximumOutputTokens;
};

struct TimeoutPolicy final {
    std::chrono::milliseconds ConnectionTimeout{};
    std::chrono::milliseconds RequestTimeout{};
};

struct RetryBudget final {
    std::uint32_t MaximumRetries{};
};

using RegisteredOptionValue =
    std::variant<bool, std::int64_t, double, std::string>;

struct RegisteredProviderOption final {
    RegisteredOptionId Id;
    RegisteredOptionValue Value;
};

struct ModelProviderProfile final {
    ProviderProfileId Id;
    ProviderProtocol Protocol{ProviderProtocol::OpenAIResponses};
    std::string Endpoint;
    std::string ModelName;
    CredentialRef Credential;
    AuthenticationScheme Authentication{AuthenticationScheme::Bearer};
    std::string ProtocolVersion;
    SamplingOptions Sampling;
    OutputLimit Limit;
    TimeoutPolicy Timeouts;
    RetryBudget Retries;
    ProviderCapabilities Capabilities;
    std::optional<CompatibilityProfileId> CompatibilityProfile;
};

// JSON used for a Core-approved tool schema or a complete tool-call value.
// This is canonical application data, never a provider wire object.
struct JsonSchema final {
    std::string JsonText;
};

struct JsonValue final {
    std::string JsonText;
};

struct TextContentPart final {
    std::string Text;
};

struct ImageContentReference final {
    std::string OpaqueReference;
};

using ContentPart = std::variant<TextContentPart, ImageContentReference>;

enum class MessageRole : std::uint8_t {
    System,
    Developer,
    User,
    Assistant,
    Tool
};

struct FunctionCall final {
    ToolCallId Id;
    std::string Name;
    JsonValue Arguments;
};

struct CanonicalMessage final {
    MessageRole Role{MessageRole::User};
    std::vector<ContentPart> ContentParts;
    std::vector<FunctionCall> FunctionCalls;
    std::optional<ToolCallId> ToolCallResultId;
};

struct ToolDefinition final {
    std::string Name;
    std::string Description;
    JsonSchema Parameters;
    bool Strict{};
};

struct CanonicalModelRequest final {
    RequestId Id;
    AgentTurnId TurnId;
    std::chrono::system_clock::time_point CreatedAt{};
    std::optional<ConversationScopeRef> ConversationScope;
    std::string SystemInstructions;
    std::string DeveloperInstructions;
    std::vector<CanonicalMessage> Messages;
    std::vector<ContentPart> ContentParts;
    std::vector<ToolDefinition> ToolDefinitions;
    std::string ModelName;
    OutputLimit Limit;
    SamplingOptions Sampling;
    std::vector<RegisteredProviderOption> ProviderOptions;
    ProviderCapabilities CapabilitySnapshot;
};

enum class FailureCode : std::uint8_t {
    InvalidProfile,
    InvalidRequest,
    UnsupportedCapability,
    AuthenticationFailure,
    RateLimited,
    TransportFailure,
    InvalidStream,
    ProviderRejected,
    ContentFiltered
};

struct ProviderError final {
    FailureCode Code{FailureCode::ProviderRejected};
    bool Retryable{};
    std::string SanitizedDescription;
};

template <typename Value>
using ProviderResult = std::variant<Value, ProviderError>;

enum class UsageCompleteness : std::uint8_t {
    Partial,
    Final,
    Unavailable
};

struct UsageSnapshot final {
    std::optional<std::uint64_t> InputTokens;
    std::optional<std::uint64_t> OutputTokens;
    std::optional<std::uint64_t> TotalTokens;
    std::optional<std::uint64_t> CachedInputReadTokens;
    std::optional<std::uint64_t> CachedInputWriteTokens;
    std::optional<std::uint64_t> ReasoningOutputTokens;
    UsageCompleteness Completeness{UsageCompleteness::Unavailable};
};

enum class EventType : std::uint8_t {
    ResponseStarted,
    TextDelta,
    RefusalDelta,
    ToolCallStarted,
    ToolArgumentDelta,
    ToolCallReady,
    UsageUpdated,
    ResponseCompleted,
    ResponseOutputLimited,
    ResponseFailed,
    ResponseCancelled,
    ResponseOutcomeUnknown
};

struct ProviderEventMetadata final {
    RequestId Request;
    AgentTurnId TurnId;
    std::uint64_t SequenceNumber{};
};

enum class RefusalCategory : std::uint8_t {
    Cyber,
    Bio,
    FrontierLlm,
    ReasoningExtraction,
    GeneralHarms,
    Unclassified
};

enum class RefusalStopDetailsType : std::uint8_t {
    Refusal
};

struct RefusalStopDetails final {
    RefusalStopDetailsType Type{RefusalStopDetailsType::Refusal};
    RefusalCategory Category{RefusalCategory::Unclassified};
};

struct ResponseStartedEvent final {
    static constexpr EventType Type = EventType::ResponseStarted;
    ProviderEventMetadata Metadata;
    std::optional<ProviderResponseId> ProviderResponse;
};

struct TextDeltaEvent final {
    static constexpr EventType Type = EventType::TextDelta;
    ProviderEventMetadata Metadata;
    ItemId Item;
    std::string Utf8Delta;
};

struct RefusalDeltaEvent final {
    static constexpr EventType Type = EventType::RefusalDelta;
    ProviderEventMetadata Metadata;
    ItemId Item;
    std::string Utf8Delta;
    std::optional<RefusalCategory> Category;
};

struct ToolCallStartedEvent final {
    static constexpr EventType Type = EventType::ToolCallStarted;
    ProviderEventMetadata Metadata;
    ItemId Item;
    ToolCallId Call;
    std::string ToolName;
};

struct ToolArgumentDeltaEvent final {
    static constexpr EventType Type = EventType::ToolArgumentDelta;
    ProviderEventMetadata Metadata;
    ItemId Item;
    ToolCallId Call;
    std::string JsonTextDelta;
};

struct ToolCallReadyEvent final {
    static constexpr EventType Type = EventType::ToolCallReady;
    ProviderEventMetadata Metadata;
    ItemId Item;
    ToolCallId Call;
    std::string ToolName;
    JsonValue Arguments;
};

struct UsageUpdatedEvent final {
    static constexpr EventType Type = EventType::UsageUpdated;
    ProviderEventMetadata Metadata;
    UsageSnapshot Usage;
};

using CanonicalNonTerminalEvent = std::variant<
    ResponseStartedEvent,
    TextDeltaEvent,
    RefusalDeltaEvent,
    ToolCallStartedEvent,
    ToolArgumentDeltaEvent,
    ToolCallReadyEvent,
    UsageUpdatedEvent>;

enum class CompletionKind : std::uint8_t {
    FinalAnswer,
    ToolHandoff,
    Refusal
};

enum class LimitKind : std::uint8_t {
    OutputTokens,
    ContextWindow
};

struct StableReasonCode final {
    std::string Value;
};

struct CancelReasonCode final {
    StableReasonCode Code;
};

struct OutcomeUnknownReasonCode final {
    StableReasonCode Code;
};

struct ResponseCompletedEvent final {
    static constexpr EventType Type = EventType::ResponseCompleted;
    ProviderEventMetadata Metadata;
    CompletionKind Kind{CompletionKind::FinalAnswer};
    std::optional<StableReasonCode> StopSequenceCode;
    std::optional<UsageSnapshot> FinalUsage;
    std::optional<RefusalStopDetails> StopDetails;
};

struct ResponseOutputLimitedEvent final {
    static constexpr EventType Type = EventType::ResponseOutputLimited;
    ProviderEventMetadata Metadata;
    LimitKind Kind{LimitKind::OutputTokens};
    UsageSnapshot Usage;
    std::optional<std::string> SafeGeneratedText;
};

struct ResponseFailedEvent final {
    static constexpr EventType Type = EventType::ResponseFailed;
    ProviderEventMetadata Metadata;
    ProviderError Failure;
};

struct ResponseCancelledEvent final {
    static constexpr EventType Type = EventType::ResponseCancelled;
    ProviderEventMetadata Metadata;
    CancelReasonCode Reason;
};

struct ResponseOutcomeUnknownEvent final {
    static constexpr EventType Type = EventType::ResponseOutcomeUnknown;
    ProviderEventMetadata Metadata;
    OutcomeUnknownReasonCode Reason;
};

using CanonicalTerminalEvent = std::variant<
    ResponseCompletedEvent,
    ResponseOutputLimitedEvent,
    ResponseFailedEvent,
    ResponseCancelledEvent,
    ResponseOutcomeUnknownEvent>;

using CanonicalModelEvent =
    std::variant<CanonicalNonTerminalEvent, CanonicalTerminalEvent>;

// The sink accepts only non-terminal events. StreamTurn returns one terminal
// event, so the public seam cannot submit a second terminal through the sink.
using EventSink = std::function<void(const CanonicalNonTerminalEvent&)>;

enum class ProviderHealthState : std::uint8_t {
    Available,
    Unavailable
};

struct ProviderHealthStatus final {
    ProviderHealthState State{ProviderHealthState::Unavailable};
};

class CredentialLease {
protected:
    CredentialLease() = default;

public:
    CredentialLease(const CredentialLease&) = delete;
    CredentialLease& operator=(const CredentialLease&) = delete;
    CredentialLease(CredentialLease&&) = delete;
    CredentialLease& operator=(CredentialLease&&) = delete;
    virtual ~CredentialLease() = default;

    // Returns a borrowed secret view valid only during the leased call.
    [[nodiscard]] virtual std::string_view GetAuthenticationSecret() const noexcept = 0;
};

class ModelProviderPort {
public:
    virtual ~ModelProviderPort() = default;

    [[nodiscard]] virtual ProviderResult<ProviderCapabilities>
    GetCapabilities(const ProviderProfileId& ProfileId) const = 0;

    [[nodiscard]] virtual ProviderResult<std::monostate>
    ValidateProfile(const ModelProviderProfile& Profile) const = 0;

    [[nodiscard]] virtual CanonicalTerminalEvent
    StreamTurn(
        const ModelProviderProfile& Profile,
        const CredentialLease& Lease,
        const CanonicalModelRequest& Request,
        std::stop_token CancellationToken,
        const EventSink& Sink) = 0;

    [[nodiscard]] virtual ProviderResult<ProviderHealthStatus>
    HealthCheck(const ModelProviderProfile& Profile) = 0;
};

} // namespace EmulativeDialog::Providers
