#include "Tests/Contract/Providers/Contracts/FakeProvider.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace EmulativeDialog::Providers;
using EmulativeDialog::Providers::Testing::FakeProvider;

class EmptyCredentialLease final : public CredentialLease {
public:
    [[nodiscard]] std::string_view GetAuthenticationSecret() const noexcept override {
        ++SecretReadCount;
        return {};
    }

    mutable std::size_t SecretReadCount{};
};

ProviderEventMetadata MakeWrongMetadata(std::uint64_t SequenceNumber) {
    return ProviderEventMetadata{
        RequestId{std::string{"script-request-placeholder"}},
        AgentTurnId{std::string{"script-turn-placeholder"}},
        SequenceNumber};
}

CanonicalModelRequest MakeSyntheticRequest(std::size_t Index) {
    const std::string Suffix = std::to_string(Index);
    CanonicalModelRequest Request;
    Request.Id = RequestId{std::string{"request-synthetic-"} + Suffix};
    Request.TurnId = AgentTurnId{std::string{"turn-synthetic-"} + Suffix};
    Request.SystemInstructions = "Synthetic instructions.";
    Request.Messages.push_back(CanonicalMessage{
        MessageRole::User,
        {TextContentPart{"Synthetic body " + Suffix}},
        {},
        std::nullopt});
    Request.ModelName = "synthetic-model";
    return Request;
}

std::vector<CanonicalNonTerminalEvent> MakeAllNonTerminalEvents() {
    return {
        ResponseStartedEvent{
            MakeWrongMetadata(90),
            ProviderResponseId{std::string{"response-synthetic"}}},
        TextDeltaEvent{
            MakeWrongMetadata(91),
            ItemId{std::string{"item-synthetic-text"}},
            "Synthetic text."},
        RefusalDeltaEvent{
            MakeWrongMetadata(92),
            ItemId{std::string{"item-synthetic-refusal"}},
            "Synthetic refusal delta.",
            RefusalCategory::GeneralHarms},
        ToolCallStartedEvent{
            MakeWrongMetadata(93),
            ItemId{std::string{"item-synthetic-tool"}},
            ToolCallId{std::string{"call-synthetic-tool"}},
            "synthetic_action"},
        ToolArgumentDeltaEvent{
            MakeWrongMetadata(94),
            ItemId{std::string{"item-synthetic-tool"}},
            ToolCallId{std::string{"call-synthetic-tool"}},
            R"({"value":)"},
        ToolCallReadyEvent{
            MakeWrongMetadata(95),
            ItemId{std::string{"item-synthetic-tool"}},
            ToolCallId{std::string{"call-synthetic-tool"}},
            "synthetic_action",
            JsonValue{R"({"value":"synthetic"})"}},
        UsageUpdatedEvent{
            MakeWrongMetadata(96),
            UsageSnapshot{
                12,
                6,
                18,
                1,
                0,
                2,
                UsageCompleteness::Final}}};
}

FakeProvider::Script MakeScript(
    std::vector<CanonicalNonTerminalEvent> NonTerminalEvents,
    CanonicalTerminalEvent Terminal) {
    return FakeProvider::Script{
        std::move(NonTerminalEvents),
        std::move(Terminal)};
}

void AssertMetadata(
    const ProviderEventMetadata& Metadata,
    const CanonicalModelRequest& Request,
    std::uint64_t ExpectedSequence) {
    assert(Metadata.Request == Request.Id);
    assert(Metadata.TurnId == Request.TurnId);
    assert(Metadata.SequenceNumber == ExpectedSequence);
}

ProviderEventMetadata GetMetadata(const CanonicalTerminalEvent& Terminal) {
    return std::visit(
        [](const auto& Event) { return Event.Metadata; },
        Terminal);
}

void TestProfileQueriesStayOffline() {
    FakeProvider Provider;
    const ProviderProfileId ProfileId{std::string{"profile-synthetic"}};

    const ProviderResult<ProviderCapabilities> Capabilities =
        Provider.GetCapabilities(ProfileId);
    assert(std::holds_alternative<ProviderCapabilities>(Capabilities));
    assert(std::get<ProviderCapabilities>(Capabilities).Streaming);

    const ProviderResult<ProviderCapabilities> InvalidCapabilities =
        Provider.GetCapabilities(ProviderProfileId{});
    assert(std::holds_alternative<ProviderError>(InvalidCapabilities));
    assert(std::get<ProviderError>(InvalidCapabilities).Code == FailureCode::InvalidProfile);

    ModelProviderProfile Profile;
    Profile.Id = ProfileId;
    Profile.ModelName = "synthetic-model";
    const ProviderResult<std::monostate> ValidProfile = Provider.ValidateProfile(Profile);
    assert(std::holds_alternative<std::monostate>(ValidProfile));

    const ProviderResult<std::monostate> InvalidProfile =
        Provider.ValidateProfile(ModelProviderProfile{});
    assert(std::holds_alternative<ProviderError>(InvalidProfile));
    assert(std::get<ProviderError>(InvalidProfile).Code == FailureCode::InvalidProfile);

    const ProviderResult<ProviderHealthStatus> Health = Provider.HealthCheck(Profile);
    assert(std::holds_alternative<ProviderHealthStatus>(Health));
    assert(std::get<ProviderHealthStatus>(Health).State == ProviderHealthState::Available);
}

void TestFifoScriptsAndEventCorrelation() {
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        MakeAllNonTerminalEvents(),
        ResponseCompletedEvent{
            MakeWrongMetadata(99),
            CompletionKind::ToolHandoff,
            std::nullopt,
            std::nullopt,
            std::nullopt}));
    Provider.EnqueueScript(MakeScript(
        {TextDeltaEvent{
            MakeWrongMetadata(99),
            ItemId{std::string{"item-synthetic-limited"}},
            "Synthetic partial output."}},
        ResponseOutputLimitedEvent{
            MakeWrongMetadata(99),
            LimitKind::OutputTokens,
            UsageSnapshot{12, 96, 108, std::nullopt, std::nullopt, 0, UsageCompleteness::Partial},
            std::string{"Synthetic partial output."}}));
    Provider.EnqueueScript(MakeScript(
        {},
        ResponseFailedEvent{
            MakeWrongMetadata(99),
            ProviderError{FailureCode::ProviderRejected, false, "Synthetic provider rejection."}}));
    Provider.EnqueueScript(MakeScript(
        {},
        ResponseCancelledEvent{
            MakeWrongMetadata(99),
            CancelReasonCode{StableReasonCode{"CancelledBeforeSend"}}}));
    Provider.EnqueueScript(MakeScript(
        {},
        ResponseOutcomeUnknownEvent{
            MakeWrongMetadata(99),
            OutcomeUnknownReasonCode{StableReasonCode{"StreamEndedBeforeTerminal"}}}));

    EmptyCredentialLease Lease;
    std::vector<CanonicalNonTerminalEvent> DeliveredEvents;
    const EventSink Sink = [&DeliveredEvents](const CanonicalNonTerminalEvent& Event) {
        DeliveredEvents.push_back(Event);
    };

    const ModelProviderProfile Profile;
    const CanonicalModelRequest RequestOne = MakeSyntheticRequest(1);
    const CanonicalTerminalEvent FirstTerminal =
        Provider.StreamTurn(Profile, Lease, RequestOne, std::stop_token{}, Sink);
    assert(std::holds_alternative<ResponseCompletedEvent>(FirstTerminal));
    const ResponseCompletedEvent& Completion = std::get<ResponseCompletedEvent>(FirstTerminal);
    AssertMetadata(Completion.Metadata, RequestOne, 8);
    assert(Completion.Kind == CompletionKind::ToolHandoff);
    assert(DeliveredEvents.size() == 7);
    for (std::size_t Index = 0; Index < DeliveredEvents.size(); ++Index) {
        std::visit(
            [&RequestOne, Index](const auto& Event) {
                AssertMetadata(Event.Metadata, RequestOne, Index + 1);
            },
            DeliveredEvents[Index]);
    }
    assert(std::get<ToolCallReadyEvent>(DeliveredEvents[5]).Call.Value == "call-synthetic-tool");

    DeliveredEvents.clear();
    const CanonicalModelRequest RequestTwo = MakeSyntheticRequest(2);
    const CanonicalTerminalEvent SecondTerminal =
        Provider.StreamTurn(Profile, Lease, RequestTwo, std::stop_token{}, Sink);
    assert(std::holds_alternative<ResponseOutputLimitedEvent>(SecondTerminal));
    AssertMetadata(GetMetadata(SecondTerminal), RequestTwo, 2);
    assert(DeliveredEvents.size() == 1);
    std::visit(
        [&RequestTwo](const auto& Event) { AssertMetadata(Event.Metadata, RequestTwo, 1); },
        DeliveredEvents.front());

    DeliveredEvents.clear();
    const CanonicalModelRequest RequestThree = MakeSyntheticRequest(3);
    const CanonicalTerminalEvent ThirdTerminal =
        Provider.StreamTurn(Profile, Lease, RequestThree, std::stop_token{}, Sink);
    assert(std::holds_alternative<ResponseFailedEvent>(ThirdTerminal));
    AssertMetadata(GetMetadata(ThirdTerminal), RequestThree, 1);
    assert(DeliveredEvents.empty());

    const CanonicalModelRequest RequestFour = MakeSyntheticRequest(4);
    const CanonicalTerminalEvent FourthTerminal =
        Provider.StreamTurn(Profile, Lease, RequestFour, std::stop_token{}, Sink);
    assert(std::holds_alternative<ResponseCancelledEvent>(FourthTerminal));
    AssertMetadata(GetMetadata(FourthTerminal), RequestFour, 1);

    const CanonicalModelRequest RequestFive = MakeSyntheticRequest(5);
    const CanonicalTerminalEvent FifthTerminal =
        Provider.StreamTurn(Profile, Lease, RequestFive, std::stop_token{}, Sink);
    assert(std::holds_alternative<ResponseOutcomeUnknownEvent>(FifthTerminal));
    AssertMetadata(GetMetadata(FifthTerminal), RequestFive, 1);

    assert(Provider.GetRemainingScriptCount() == 0);
    assert(Provider.GetCapturedRequests().size() == 5);
    assert(Provider.GetCapturedRequests()[0].Id == RequestOne.Id);
    assert(Provider.GetCapturedRequests()[1].Id == RequestTwo.Id);
    assert(Provider.GetCapturedRequests()[2].Id == RequestThree.Id);
    assert(Provider.GetCapturedRequests()[3].Id == RequestFour.Id);
    assert(Provider.GetCapturedRequests()[4].Id == RequestFive.Id);
    assert(Lease.SecretReadCount == 0);
}

void TestExhaustedQueueFailsClosed() {
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {},
        ResponseCompletedEvent{
            MakeWrongMetadata(99),
            CompletionKind::FinalAnswer,
            std::nullopt,
            std::nullopt,
            std::nullopt}));

    const ModelProviderProfile Profile;
    EmptyCredentialLease Lease;
    std::vector<CanonicalNonTerminalEvent> DeliveredEvents;
    const EventSink Sink = [&DeliveredEvents](const CanonicalNonTerminalEvent& Event) {
        DeliveredEvents.push_back(Event);
    };

    const CanonicalModelRequest RequestOne = MakeSyntheticRequest(10);
    const CanonicalTerminalEvent Scripted = Provider.StreamTurn(
        Profile,
        Lease,
        RequestOne,
        std::stop_token{},
        Sink);
    assert(std::holds_alternative<ResponseCompletedEvent>(Scripted));
    AssertMetadata(GetMetadata(Scripted), RequestOne, 1);
    assert(Provider.GetRemainingScriptCount() == 0);

    const CanonicalModelRequest RequestTwo = MakeSyntheticRequest(11);
    const CanonicalTerminalEvent ExhaustedOne = Provider.StreamTurn(
        Profile,
        Lease,
        RequestTwo,
        std::stop_token{},
        Sink);
    assert(std::holds_alternative<ResponseFailedEvent>(ExhaustedOne));
    assert(std::get<ResponseFailedEvent>(ExhaustedOne).Failure.Code == FailureCode::InvalidRequest);
    AssertMetadata(GetMetadata(ExhaustedOne), RequestTwo, 1);
    assert(DeliveredEvents.empty());
    assert(Provider.GetRemainingScriptCount() == 0);

    const CanonicalModelRequest RequestThree = MakeSyntheticRequest(12);
    const CanonicalTerminalEvent ExhaustedTwo = Provider.StreamTurn(
        Profile,
        Lease,
        RequestThree,
        std::stop_token{},
        Sink);
    assert(std::holds_alternative<ResponseFailedEvent>(ExhaustedTwo));
    AssertMetadata(GetMetadata(ExhaustedTwo), RequestThree, 1);
    assert(Provider.GetRemainingScriptCount() == 0);
    assert(Provider.GetCapturedRequests().size() == 3);
    assert(Lease.SecretReadCount == 0);
}
} // namespace

int main() {
    static_assert(!std::is_invocable_v<EventSink, const CanonicalTerminalEvent&>);
    TestProfileQueriesStayOffline();
    TestFifoScriptsAndEventCorrelation();
    TestExhaustedQueueFailsClosed();
}
