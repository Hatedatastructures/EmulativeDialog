#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace EmulativeDialog::Providers;

template <typename Type>
concept HasCredentialField = requires(const Type& Value) {
    Value.Credential;
};

template <typename Type>
concept HasCoreAuthorizationField = requires(const Type& Value) {
    Value.CoreAuthorized;
};

template <typename Type>
concept HasExplanationField = requires(const Type& Value) {
    Value.Explanation;
};

ProviderEventMetadata MakeMetadata(std::uint64_t SequenceNumber) {
    return ProviderEventMetadata{
        RequestId{std::string{"request-synthetic"}},
        AgentTurnId{std::string{"turn-synthetic"}},
        SequenceNumber};
}

class SyntheticCredentialLease final : public CredentialLease {
public:
    [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept override {
        return {};
    }
};

class SyntheticProvider final : public ModelProviderPort {
public:
    [[nodiscard]] ProviderResult<ProviderCapabilities>
    GetCapabilities(const ProviderProfileId&) const override {
        return ProviderCapabilities{true, true, false, true, true, true};
    }

    [[nodiscard]] ProviderResult<std::monostate>
    ValidateProfile(const ModelProviderProfile&) const override {
        return std::monostate{};
    }

    [[nodiscard]] CanonicalTerminalEvent
    StreamTurn(
        const ModelProviderProfile&,
        const CredentialLease&,
        const CanonicalModelRequest& Request,
        std::stop_token,
        const EventSink& Sink) override {
        ++CallCount;

        if (Sink) {
            const CanonicalNonTerminalEvent Started = ResponseStartedEvent{
                MakeMetadata(1),
                ProviderResponseId{std::string{"response-synthetic"}}};
            Sink(Started);

            const CanonicalNonTerminalEvent ToolCandidate = ToolCallReadyEvent{
                MakeMetadata(2),
                ItemId{std::string{"item-synthetic-tool"}},
                ToolCallId{std::string{"call-synthetic-tool"}},
                "lookup_order",
                JsonValue{R"({"order_id":"ORDER-SYNTHETIC"})"}};
            Sink(ToolCandidate);
        }

        return ResponseCompletedEvent{
            ProviderEventMetadata{
                Request.Id,
                Request.TurnId,
                3},
            CompletionKind::ToolHandoff,
            std::nullopt,
            std::nullopt,
            std::nullopt};
    }

    [[nodiscard]] ProviderResult<ProviderHealthStatus>
    HealthCheck(const ModelProviderProfile&) override {
        return ProviderHealthStatus{ProviderHealthState::Available};
    }

    std::size_t CallCount{};
};

void TestV1EventTypeCoverage() {
    const std::vector<CanonicalNonTerminalEvent> NonTerminalEvents{
        ResponseStartedEvent{MakeMetadata(1), std::nullopt},
        TextDeltaEvent{
            MakeMetadata(2),
            ItemId{std::string{"item-text"}},
            "synthetic text"},
        RefusalDeltaEvent{
            MakeMetadata(3),
            ItemId{std::string{"item-refusal"}},
            "synthetic refusal",
            std::nullopt},
        ToolCallStartedEvent{
            MakeMetadata(4),
            ItemId{std::string{"item-tool"}},
            ToolCallId{std::string{"call-tool"}},
            "synthetic_action"},
        ToolArgumentDeltaEvent{
            MakeMetadata(5),
            ItemId{std::string{"item-tool"}},
            ToolCallId{std::string{"call-tool"}},
            R"({"key":)"},
        ToolCallReadyEvent{
            MakeMetadata(6),
            ItemId{std::string{"item-tool"}},
            ToolCallId{std::string{"call-tool"}},
            "synthetic_action",
            JsonValue{R"({"key":"value"})"}},
        UsageUpdatedEvent{
            MakeMetadata(7),
            UsageSnapshot{
                5,
                3,
                8,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                UsageCompleteness::Final}}};

    assert(NonTerminalEvents.size() == std::variant_size_v<CanonicalNonTerminalEvent>);
    for (std::size_t Index = 0; Index < NonTerminalEvents.size(); ++Index) {
        const std::uint64_t SequenceNumber = std::visit(
            [](const auto& Event) { return Event.Metadata.SequenceNumber; },
            NonTerminalEvents[Index]);
        assert(SequenceNumber == Index + 1);
    }

    const std::vector<CanonicalTerminalEvent> TerminalEvents{
        ResponseCompletedEvent{
            MakeMetadata(8),
            CompletionKind::FinalAnswer,
            std::nullopt,
            UsageSnapshot{5, 3, 8, std::nullopt, std::nullopt, 1, UsageCompleteness::Final},
            std::nullopt},
        ResponseOutputLimitedEvent{
            MakeMetadata(9),
            LimitKind::OutputTokens,
            UsageSnapshot{5, 10, 15, std::nullopt, std::nullopt, 2, UsageCompleteness::Partial},
            std::string{"safe synthetic partial text"}},
        ResponseFailedEvent{
            MakeMetadata(10),
            ProviderError{
                FailureCode::InvalidStream,
                false,
                "Synthetic invalid stream."}},
        ResponseCancelledEvent{
            MakeMetadata(11),
            CancelReasonCode{StableReasonCode{"CancelledBeforeSend"}}},
        ResponseOutcomeUnknownEvent{
            MakeMetadata(12),
            OutcomeUnknownReasonCode{StableReasonCode{"StreamEndedBeforeTerminal"}}}};

    assert(TerminalEvents.size() == std::variant_size_v<CanonicalTerminalEvent>);
    for (std::size_t Index = 0; Index < TerminalEvents.size(); ++Index) {
        const std::uint64_t SequenceNumber = std::visit(
            [](const auto& Event) { return Event.Metadata.SequenceNumber; },
            TerminalEvents[Index]);
        assert(SequenceNumber == Index + 8);
    }

    const CanonicalModelEvent NonTerminalStreamItem{NonTerminalEvents.front()};
    const CanonicalModelEvent TerminalStreamItem{TerminalEvents.front()};
    assert(std::holds_alternative<CanonicalNonTerminalEvent>(NonTerminalStreamItem));
    assert(std::holds_alternative<CanonicalTerminalEvent>(TerminalStreamItem));
}

RefusalCategory NormalizeFixtureCategory(std::optional<std::string_view> WireCategory) {
    if (!WireCategory) {
        return RefusalCategory::Unclassified;
    }
    if (*WireCategory == "cyber") {
        return RefusalCategory::Cyber;
    }
    if (*WireCategory == "bio") {
        return RefusalCategory::Bio;
    }
    if (*WireCategory == "frontier_llm") {
        return RefusalCategory::FrontierLlm;
    }
    if (*WireCategory == "reasoning_extraction") {
        return RefusalCategory::ReasoningExtraction;
    }
    if (*WireCategory == "general_harms") {
        return RefusalCategory::GeneralHarms;
    }
    return RefusalCategory::Unclassified;
}

void TestRefusalMetadataWithoutVisibleBody() {
    static_assert(HasExplanationField<RefusalStopDetails> == false);
    static_assert(std::is_same_v<
        decltype(RefusalStopDetails{}.Category),
        RefusalCategory>);
    static_assert(std::is_same_v<
        decltype(ResponseCompletedEvent{}.StopDetails),
        std::optional<RefusalStopDetails>>);

    static_assert(std::is_same_v<
        decltype(RefusalDeltaEvent{}.Category),
        std::optional<RefusalCategory>>);

    const RefusalDeltaEvent DeltaWithoutCategory{
        MakeMetadata(1),
        ItemId{std::string{"item-refusal-empty-category"}},
        "synthetic refusal delta",
        std::nullopt};
    const RefusalDeltaEvent DeltaWithCategory{
        MakeMetadata(2),
        ItemId{std::string{"item-refusal-categorized"}},
        "synthetic categorized refusal delta",
        RefusalCategory::Cyber};
    assert(!DeltaWithoutCategory.Category.has_value());
    assert(DeltaWithCategory.Category == RefusalCategory::Cyber);

    const std::vector<CanonicalNonTerminalEvent> EmptyRefusalBody{
        ResponseStartedEvent{MakeMetadata(1), std::nullopt}};
    const ResponseCompletedEvent EmptyBodyRefusal{
        MakeMetadata(2),
        CompletionKind::Refusal,
        std::nullopt,
        UsageSnapshot{10, 0, 10, std::nullopt, std::nullopt, 0, UsageCompleteness::Final},
        RefusalStopDetails{
            RefusalStopDetailsType::Refusal,
            NormalizeFixtureCategory(std::nullopt)}};

    assert(EmptyRefusalBody.size() == 1);
    assert(std::holds_alternative<ResponseStartedEvent>(EmptyRefusalBody.front()));
    assert(EmptyBodyRefusal.Kind == CompletionKind::Refusal);
    assert(EmptyBodyRefusal.StopDetails.has_value());
    assert(EmptyBodyRefusal.StopDetails->Type == RefusalStopDetailsType::Refusal);
    assert(EmptyBodyRefusal.StopDetails->Category == RefusalCategory::Unclassified);

    const ResponseCompletedEvent KnownCategoryRefusal{
        MakeMetadata(3),
        CompletionKind::Refusal,
        std::nullopt,
        std::nullopt,
        RefusalStopDetails{
            RefusalStopDetailsType::Refusal,
            NormalizeFixtureCategory(std::string_view{"general_harms"})}};
    assert(KnownCategoryRefusal.StopDetails->Category == RefusalCategory::GeneralHarms);

    const ResponseCompletedEvent NonRefusal{
        MakeMetadata(4),
        CompletionKind::FinalAnswer,
        std::nullopt,
        std::nullopt,
        std::nullopt};
    assert(!NonRefusal.StopDetails.has_value());

    assert(NormalizeFixtureCategory(std::string_view{"future_category"}) ==
        RefusalCategory::Unclassified);
    assert(NormalizeFixtureCategory(std::string_view{"cyber"}) == RefusalCategory::Cyber);
    assert(NormalizeFixtureCategory(std::string_view{"bio"}) == RefusalCategory::Bio);
    assert(NormalizeFixtureCategory(std::string_view{"frontier_llm"}) ==
        RefusalCategory::FrontierLlm);
    assert(NormalizeFixtureCategory(std::string_view{"reasoning_extraction"}) ==
        RefusalCategory::ReasoningExtraction);
}

void TestFailureCodeSeparation() {
    static_assert(std::is_same_v<decltype(ProviderError{}.Code), FailureCode>);
    constexpr std::array<FailureCode, 9> FailureCodes{
        FailureCode::InvalidProfile,
        FailureCode::InvalidRequest,
        FailureCode::UnsupportedCapability,
        FailureCode::AuthenticationFailure,
        FailureCode::RateLimited,
        FailureCode::TransportFailure,
        FailureCode::InvalidStream,
        FailureCode::ProviderRejected,
        FailureCode::ContentFiltered};

    static_assert(!std::is_convertible_v<LimitKind, FailureCode>);
    static_assert(!std::is_convertible_v<CancelReasonCode, FailureCode>);
    static_assert(!std::is_convertible_v<OutcomeUnknownReasonCode, FailureCode>);
    assert(FailureCodes.size() == 9);

    const ProviderError Failure{
        FailureCode::ContentFiltered,
        false,
        "Synthetic filtered result."};
    const CanonicalTerminalEvent Failed{ResponseFailedEvent{
        MakeMetadata(1),
        Failure}};
    const CanonicalTerminalEvent Limited{ResponseOutputLimitedEvent{
        MakeMetadata(1),
        LimitKind::OutputTokens,
        UsageSnapshot{1, 1, 2, std::nullopt, std::nullopt, std::nullopt, UsageCompleteness::Final},
        std::nullopt}};
    const CanonicalTerminalEvent Cancelled{ResponseCancelledEvent{
        MakeMetadata(1),
        CancelReasonCode{StableReasonCode{"CancelledBeforeSend"}}}};
    const CanonicalTerminalEvent Unknown{ResponseOutcomeUnknownEvent{
        MakeMetadata(1),
        OutcomeUnknownReasonCode{StableReasonCode{"StreamEndedBeforeTerminal"}}}};

    assert(std::holds_alternative<ResponseFailedEvent>(Failed));
    assert(std::holds_alternative<ResponseOutputLimitedEvent>(Limited));
    assert(std::holds_alternative<ResponseCancelledEvent>(Cancelled));
    assert(std::holds_alternative<ResponseOutcomeUnknownEvent>(Unknown));
}

void TestModelProviderPortCallShape() {
    static_assert(std::is_abstract_v<ModelProviderPort>);
    static_assert(!std::is_copy_constructible_v<CredentialLease>);
    static_assert(!std::is_copy_assignable_v<CredentialLease>);
    static_assert(!HasCredentialField<ProviderEventMetadata>);
    static_assert(!HasCredentialField<ToolCallReadyEvent>);
    static_assert(!std::is_same_v<RequestId, AgentTurnId>);
    static_assert(!std::is_convertible_v<RequestId, AgentTurnId>);
    static_assert(!HasCredentialField<CanonicalModelRequest>);
    static_assert(!HasCredentialField<CanonicalModelEvent>);
    static_assert(!HasCoreAuthorizationField<ToolCallReadyEvent>);
    static_assert(!std::is_invocable_v<EventSink, const CanonicalTerminalEvent&>);
    static_assert(std::variant_size_v<CanonicalNonTerminalEvent> == 7);
    static_assert(std::variant_size_v<CanonicalTerminalEvent> == 5);

    ModelProviderProfile Profile;
    Profile.Id = ProviderProfileId{std::string{"profile-synthetic"}};
    Profile.Protocol = ProviderProtocol::OpenAIResponses;
    Profile.Endpoint = "https://provider.example.invalid/v1/responses";
    Profile.ModelName = "synthetic-model";
    Profile.Credential = CredentialRef{std::string{"credential-ref-synthetic"}};
    Profile.Authentication = AuthenticationScheme::Bearer;
    Profile.ProtocolVersion = "synthetic-v1";
    Profile.Capabilities = ProviderCapabilities{true, true, false, true, true, true};

    CanonicalModelRequest Request;
    Request.Id = RequestId{std::string{"request-synthetic"}};
    Request.TurnId = AgentTurnId{std::string{"turn-synthetic"}};
    Request.SystemInstructions = "Synthetic instructions.";
    Request.Messages.push_back(CanonicalMessage{
        MessageRole::User,
        {TextContentPart{"Synthetic request input."}},
        {},
        std::nullopt});
    Request.ToolDefinitions.push_back(ToolDefinition{
        "lookup_order",
        "Return a synthetic order status.",
        JsonSchema{R"({"type":"object","properties":{"order_id":{"type":"string"}}})"},
        true});
    Request.ModelName = "synthetic-model";
    Request.Limit = OutputLimit{96};
    Request.CapabilitySnapshot = Profile.Capabilities;

    SyntheticCredentialLease Lease;
    SyntheticProvider Provider;
    std::vector<CanonicalNonTerminalEvent> DeliveredEvents;
    const EventSink Sink = [&DeliveredEvents](const CanonicalNonTerminalEvent& Event) {
        DeliveredEvents.push_back(Event);
    };

    const ProviderResult<ProviderCapabilities> Capabilities =
        Provider.GetCapabilities(Profile.Id);
    assert(std::holds_alternative<ProviderCapabilities>(Capabilities));

    const ProviderResult<std::monostate> Validation = Provider.ValidateProfile(Profile);
    assert(std::holds_alternative<std::monostate>(Validation));

    const CanonicalTerminalEvent Terminal = Provider.StreamTurn(
        Profile,
        Lease,
        Request,
        std::stop_token{},
        Sink);
    assert(Provider.CallCount == 1);
    assert(DeliveredEvents.size() == 2);
    assert(std::holds_alternative<ToolCallReadyEvent>(DeliveredEvents.back()));
    assert(std::holds_alternative<ResponseCompletedEvent>(Terminal));
    assert(std::get<ResponseCompletedEvent>(Terminal).Kind == CompletionKind::ToolHandoff);
    assert(std::get<ResponseCompletedEvent>(Terminal).Metadata.SequenceNumber == 3);

    const ProviderResult<ProviderHealthStatus> Health = Provider.HealthCheck(Profile);
    assert(std::holds_alternative<ProviderHealthStatus>(Health));
    assert(std::get<ProviderHealthStatus>(Health).State == ProviderHealthState::Available);
}

} // namespace

int main() {
    TestV1EventTypeCoverage();
    TestRefusalMetadataWithoutVisibleBody();
    TestFailureCodeSeparation();
    TestModelProviderPortCallShape();
}
