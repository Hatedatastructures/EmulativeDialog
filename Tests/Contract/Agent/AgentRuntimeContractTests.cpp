#include "Source/Agent/AgentRuntime.h"
#include "Source/Agent/OutputBudgetController.hpp"
#include "Source/Core/AgentRuntimePort.h"
#include "Tests/Contract/Providers/Contracts/FakeProvider.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace EmulativeDialog;
using Providers::Testing::FakeProvider;

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

class TestProviderCredential final : public Providers::CredentialLease {
public:
  std::string_view GetAuthenticationSecret() const noexcept override {
    return {};
  }
};

Providers::ProviderEventMetadata Metadata(const Providers::RequestId& Request,
                                          const Providers::AgentTurnId& Turn,
                                          std::uint64_t Sequence) {
  return Providers::ProviderEventMetadata{
      .Request = Request,
      .TurnId = Turn,
      .SequenceNumber = Sequence,
  };
}

Providers::RequestId ExpectedRequestId(const Providers::AgentTurnId& Turn,
                                       std::uint64_t Sequence = 1) {
  return Providers::RequestId{
      "agent-runtime:" + std::to_string(Turn.Value.size()) + ":" +
      Turn.Value + ":" + std::to_string(Sequence)};
}

Providers::ModelProviderProfile MakeProfile(
    Providers::ProviderProtocol Protocol =
        Providers::ProviderProtocol::OpenAIResponses,
    bool HasOutputLimit = true) {
  Providers::ModelProviderProfile Profile;
  Profile.Id = Providers::ProviderProfileId{"profile-synthetic"};
  Profile.Protocol = Protocol;
  Profile.Endpoint = "https://provider.invalid";
  Profile.ModelName = "synthetic-model";
  Profile.Credential = Providers::CredentialRef{"credential-synthetic"};
  Profile.Authentication = Protocol ==
                                  Providers::ProviderProtocol::AnthropicMessages
                              ? Providers::AuthenticationScheme::XApiKeyHeader
                              : Providers::AuthenticationScheme::Bearer;
  if (Protocol == Providers::ProviderProtocol::AnthropicMessages) {
    Profile.ProtocolVersion = "2026-10-21";
  }
  if (HasOutputLimit) {
    Profile.Limit.MaximumOutputTokens = 64;
  }
  Profile.Capabilities = Providers::ProviderCapabilities{
      .Streaming = true,
      .ToolCalls = false,
      .Vision = true,
      .Usage = true,
      .Cancellation = true,
      .OutputLimit = HasOutputLimit,
  };
  return Profile;
}

Core::AuthorizedTurnContext MakeContext(
    std::string TurnValue,
    Channels::CanonicalMessage Message = Channels::CanonicalMessage{
        .MessageParts = {Channels::TextPart{"synthetic user question"}},
        .ReplyToId = std::nullopt,
        .MentionedIds = {},
        .PlatformMetadata = {},
    }) {
  Core::PrincipalId Principal{"principal-synthetic"};
  return Core::AuthorizedTurnContext{
      .Principal = Principal,
      .Scope = Core::ConversationScope{
          .Principal = Principal,
          .Channel = Channels::ChannelId{"Mock"},
          .Account = Channels::AccountId{"account-synthetic"},
          .Mode = Channels::IntegrationMode{"Mock"},
          .Conversation = Channels::ConversationId{"conversation-synthetic"},
          .ConversationType = Channels::ConversationKind::Private,
          .Sender = Channels::SenderId{"sender-synthetic"},
      },
      .AgentTurn = Providers::AgentTurnId{std::move(TurnValue)},
      .Message = std::move(Message),
  };
}

FakeProvider::Script MakeScript(
    std::vector<Providers::CanonicalNonTerminalEvent> Events,
    Providers::CanonicalTerminalEvent Terminal) {
  return FakeProvider::Script{
      .NonTerminalEvents = std::move(Events),
      .Terminal = std::move(Terminal),
  };
}

Providers::CanonicalNonTerminalEvent StartedEvent(
    const Providers::RequestId& Request,
    const Providers::AgentTurnId& Turn,
    std::uint64_t Sequence) {
  return Providers::ResponseStartedEvent{
      .Metadata = Metadata(Request, Turn, Sequence),
      .ProviderResponse = std::nullopt,
  };
}

Providers::CanonicalNonTerminalEvent TextEvent(
    const Providers::RequestId& Request,
    const Providers::AgentTurnId& Turn,
    std::uint64_t Sequence,
    std::string Text) {
  return Providers::TextDeltaEvent{
      .Metadata = Metadata(Request, Turn, Sequence),
      .Item = Providers::ItemId{"item-text"},
      .Utf8Delta = std::move(Text),
  };
}

Providers::CanonicalNonTerminalEvent RefusalEvent(
    const Providers::RequestId& Request,
    const Providers::AgentTurnId& Turn,
    std::uint64_t Sequence,
    std::string Text) {
  return Providers::RefusalDeltaEvent{
      .Metadata = Metadata(Request, Turn, Sequence),
      .Item = Providers::ItemId{"item-refusal"},
      .Utf8Delta = std::move(Text),
      .Category = Providers::RefusalCategory::GeneralHarms,
  };
}

Providers::CanonicalNonTerminalEvent UsageEvent(
    const Providers::RequestId& Request,
    const Providers::AgentTurnId& Turn,
    std::uint64_t Sequence,
    Providers::UsageSnapshot Usage) {
  return Providers::UsageUpdatedEvent{
      .Metadata = Metadata(Request, Turn, Sequence),
      .Usage = std::move(Usage),
  };
}

Providers::ResponseCompletedEvent Completed(
    const Providers::RequestId& Request,
    const Providers::AgentTurnId& Turn,
    std::uint64_t Sequence,
    Providers::CompletionKind Kind,
    std::optional<Providers::RefusalStopDetails> StopDetails = std::nullopt) {
  return Providers::ResponseCompletedEvent{
      .Metadata = Metadata(Request, Turn, Sequence),
      .Kind = Kind,
      .StopSequenceCode = std::nullopt,
      .FinalUsage = std::nullopt,
      .StopDetails = std::move(StopDetails),
  };
}

class RawScriptProvider final : public Providers::ModelProviderPort {
public:
  RawScriptProvider(
      std::vector<Providers::CanonicalNonTerminalEvent> Events,
      Providers::CanonicalTerminalEvent Terminal,
      Providers::ProviderCapabilities Capabilities =
          Providers::ProviderCapabilities{true, true, true, true, true, true})
      : Events(std::move(Events)), Terminal(std::move(Terminal)),
        Capabilities(Capabilities) {}

  Providers::ProviderResult<Providers::ProviderCapabilities> GetCapabilities(
      const Providers::ProviderProfileId&) const override {
    return Capabilities;
  }

  Providers::ProviderResult<std::monostate> ValidateProfile(
      const Providers::ModelProviderProfile&) const override {
    return std::monostate{};
  }

  Providers::CanonicalTerminalEvent StreamTurn(
      const Providers::ModelProviderProfile&,
      const Providers::CredentialLease&,
      const Providers::CanonicalModelRequest& Request,
      std::stop_token,
      const Providers::EventSink& Sink) override {
    CapturedRequests.push_back(Request);
    for (const auto& Event : Events) {
      Sink(Event);
    }
    return Terminal;
  }

  Providers::ProviderResult<Providers::ProviderHealthStatus> HealthCheck(
      const Providers::ModelProviderProfile&) override {
    return Providers::ProviderHealthStatus{
        Providers::ProviderHealthState::Available};
  }

private:
  std::vector<Providers::CanonicalNonTerminalEvent> Events;
  Providers::CanonicalTerminalEvent Terminal;
  Providers::ProviderCapabilities Capabilities;
  std::vector<Providers::CanonicalModelRequest> CapturedRequests;
};

Core::AgentRunResult RunScript(
    const Core::AuthorizedTurnContext& Context,
    const Providers::ModelProviderProfile& Profile,
    Providers::ModelProviderPort& Provider) {
  Agent::SharedTurnBudgetLedger().Reset();
  Agent::AgentRuntime Runtime;
  TestProviderCredential Credential;
  return Runtime.RunTurn(Context, Provider, Profile, Credential, {});
}

void TestFinalAnswerCreatesOneCorrelatedRequest() {
  const Core::AuthorizedTurnContext Context = MakeContext("contract-final");
  const Providers::RequestId Request = ExpectedRequestId(Context.AgentTurn);
  FakeProvider Provider;
  Provider.EnqueueScript(MakeScript(
      {StartedEvent(Request, Context.AgentTurn, 1),
       TextEvent(Request, Context.AgentTurn, 2, "final answer")},
      Completed(Request, Context.AgentTurn, 3,
                Providers::CompletionKind::FinalAnswer)));

  const auto Result = RunScript(Context, MakeProfile(), Provider);
  Check(Result.GetStatus() == Core::AgentRunStatus::ResponseReady,
        "a complete non-empty FinalAnswer should produce ResponseReady");
  Check(Result.GetCompletionKind() == Providers::CompletionKind::FinalAnswer,
        "FinalAnswer completion kind must remain typed");
  Check(Result.GetFinalText() == "final answer",
        "only complete final text should be returned");
  const auto& Requests = Provider.GetCapturedRequests();
  Check(Requests.size() == 1,
        "one AgentRuntime turn must issue exactly one provider request");
  Check(Requests[0].Id.Value == Request.Value &&
            Requests[0].TurnId.Value == Context.AgentTurn.Value,
        "captured request must carry the Core turn and generated request IDs");
  Check(Requests[0].Messages.size() == 1 &&
            Requests[0].Messages[0].Role == Providers::MessageRole::User &&
            Requests[0].Messages[0].ContentParts.size() == 1 &&
            std::get<Providers::TextContentPart>(
                Requests[0].Messages[0].ContentParts[0]).Text ==
                "synthetic user question",
        "only canonical user content should map to the provider request");
  Check(!Requests[0].ConversationScope.has_value(),
        "channel scope identity must not be serialized into provider context");
  Check(Provider.GetRemainingScriptCount() == 0,
        "exactly one scripted provider terminal should be consumed");
}

void TestRefusalNeedsNonEmptyRefusalText() {
  const Core::AuthorizedTurnContext Context = MakeContext("contract-refusal");
  const Providers::RequestId Request = ExpectedRequestId(Context.AgentTurn);
  FakeProvider Provider;
  Provider.EnqueueScript(MakeScript(
      {RefusalEvent(Request, Context.AgentTurn, 1, "I cannot help with that.")},
      Completed(Request, Context.AgentTurn, 2,
                Providers::CompletionKind::Refusal)));

  const auto Result = RunScript(Context, MakeProfile(), Provider);
  Check(Result.GetStatus() == Core::AgentRunStatus::ResponseReady &&
            Result.GetCompletionKind() == Providers::CompletionKind::Refusal &&
            Result.GetFinalText() == "I cannot help with that.",
        "completed non-empty refusal content should remain a typed refusal");
}

void TestToolHandoffAndEmptyFinalFailClosed() {
  {
    const Core::AuthorizedTurnContext Context = MakeContext("contract-tool");
    const Providers::RequestId Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Providers::CanonicalNonTerminalEvent ToolReady =
        Providers::ToolCallReadyEvent{
            .Metadata = Metadata(Request, Context.AgentTurn, 1),
            .Item = Providers::ItemId{"item-tool"},
            .Call = Providers::ToolCallId{"call-synthetic"},
            .ToolName = "unsupported-tool",
            .Arguments = Providers::JsonValue{"{}"},
        };
    Provider.EnqueueScript(MakeScript(
        {std::move(ToolReady)},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::ToolHandoff)));

    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Unsupported,
          "ToolHandoff must fail closed as Unsupported without tool execution");
  }
  {
    const Core::AuthorizedTurnContext Context = MakeContext("contract-empty");
    const Providers::RequestId Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {}, Completed(Request, Context.AgentTurn, 1,
                      Providers::CompletionKind::FinalAnswer)));

    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() == Core::AgentRunFailureCode::InvalidStream,
          "empty completed text must fail closed");
  }
}

void TestOutputLimitNeverReturnsPartialTextOrRetries() {
  const Core::AuthorizedTurnContext Context = MakeContext("contract-limit");
  const Providers::RequestId Request = ExpectedRequestId(Context.AgentTurn);
  FakeProvider Provider;
  Provider.EnqueueScript(MakeScript(
      {TextEvent(Request, Context.AgentTurn, 1, "partial answer")},
      Providers::ResponseOutputLimitedEvent{
          .Metadata = Metadata(Request, Context.AgentTurn, 2),
          .Kind = Providers::LimitKind::OutputTokens,
          .Usage = Providers::UsageSnapshot{},
          .SafeGeneratedText = "partial answer",
      }));
  Provider.EnqueueScript(MakeScript(
      {TextEvent(Providers::RequestId{"unused-retry"}, Context.AgentTurn, 1,
                 "autonomous retry must not happen")},
      Completed(Providers::RequestId{"unused-retry"}, Context.AgentTurn, 2,
                Providers::CompletionKind::FinalAnswer)));

  const auto Result = RunScript(Context, MakeProfile(), Provider);
  Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
            Result.GetFailureCode() ==
                Core::AgentRunFailureCode::OutputLimitReached &&
            Result.GetFinalText().empty(),
        "output-limited partial text must become typed OutputLimitReached only");
  Check(Provider.GetCapturedRequests().size() == 1 &&
            Provider.GetRemainingScriptCount() == 1,
        "AgentRuntime must not retry or compact autonomously");
}

void TestCoreApprovalAllowsOnlyFreshRequestContinuation() {
  Agent::SharedTurnBudgetLedger().Reset();
  const Core::AuthorizedTurnContext Context =
      MakeContext("contract-budget-continuation");
  const Providers::RequestId FirstRequest =
      ExpectedRequestId(Context.AgentTurn, 1);
  const Providers::RequestId ContinuationRequest =
      ExpectedRequestId(Context.AgentTurn, 3);
  Providers::ModelProviderProfile Profile = MakeProfile();
  Profile.Limit.MaximumOutputTokens = 64;

  FakeProvider Provider;
  Provider.EnqueueScript(MakeScript(
      {UsageEvent(FirstRequest, Context.AgentTurn, 1,
                  Providers::UsageSnapshot{
                      .InputTokens = std::nullopt,
                      .OutputTokens = std::nullopt,
                      .TotalTokens = 80,
                      .CachedInputReadTokens = std::nullopt,
                      .CachedInputWriteTokens = std::nullopt,
                      .ReasoningOutputTokens = std::nullopt,
                      .Completeness =
                          Providers::UsageCompleteness::Partial,
                  })},
      Providers::ResponseOutputLimitedEvent{
          .Metadata = Metadata(FirstRequest, Context.AgentTurn, 2),
          .Kind = Providers::LimitKind::OutputTokens,
          .Usage = Providers::UsageSnapshot{
              .InputTokens = std::nullopt,
              .OutputTokens = std::nullopt,
              .TotalTokens = 80,
              .CachedInputReadTokens = std::nullopt,
              .CachedInputWriteTokens = std::nullopt,
              .ReasoningOutputTokens = std::nullopt,
              .Completeness =
                  Providers::UsageCompleteness::Partial,
          },
          .SafeGeneratedText = std::nullopt,
      }));

  Agent::AgentRuntime Runtime;
  TestProviderCredential Credential;
  const auto Limited =
      Runtime.RunTurn(Context, Provider, Profile, Credential, {});
  Check(Limited.GetStatus() == Core::AgentRunStatus::Failed &&
            Limited.GetFailureCode() ==
                Core::AgentRunFailureCode::OutputLimitReached,
        "first request must stop at the output limit");
  Check(Provider.GetCapturedRequests().size() == 1U &&
            Agent::SharedTurnBudgetLedger()
                    .GetState(Context.AgentTurn)
                    ->RemainingOutputTokens == 0U,
        "output-limited request must be recorded as exhausted");
  const auto Denied =
      Runtime.RunTurn(Context, Provider, Profile, Credential, {});
  Check(Denied.GetStatus() == Core::AgentRunStatus::Failed &&
            Denied.GetFailureCode() ==
                Core::AgentRunFailureCode::OutputLimitReached &&
            Provider.GetCapturedRequests().size() == 1U,
        "continuation without Core approval must not reach Provider");

  Check(!Agent::SharedTurnBudgetLedger()
             .ApproveContinuation(Context.AgentTurn, FirstRequest)
             .has_value(),
        "Core approval must bind to the exhausted request");
  Provider.EnqueueScript(MakeScript(
      {UsageEvent(ContinuationRequest, Context.AgentTurn, 1,
                  Providers::UsageSnapshot{
                      .InputTokens = 20,
                      .OutputTokens = 10,
                      .TotalTokens = 30,
                      .CachedInputReadTokens = std::nullopt,
                      .CachedInputWriteTokens = std::nullopt,
                      .ReasoningOutputTokens = std::nullopt,
                      .Completeness =
                          Providers::UsageCompleteness::Final,
                  }),
       TextEvent(ContinuationRequest, Context.AgentTurn, 2, "continued")},
      Completed(ContinuationRequest, Context.AgentTurn, 3,
                Providers::CompletionKind::FinalAnswer)));
  const auto Continued =
      Runtime.RunTurn(Context, Provider, Profile, Credential, {});
  Check(Continued.GetStatus() == Core::AgentRunStatus::ResponseReady &&
            Continued.GetFinalText() == "continued",
        "approved continuation must complete with the final answer");
  Check(Provider.GetCapturedRequests().size() == 2U &&
            Provider.GetCapturedRequests()[0].Id.Value !=
                Provider.GetCapturedRequests()[1].Id.Value &&
            Provider.GetCapturedRequests()[1].Id.Value ==
                ContinuationRequest.Value,
        "approved continuation must use a fresh RequestId");
  Agent::SharedTurnBudgetLedger().Reset();
}

void TestProviderFailureCancelAndUnknownStayDistinct() {
  {
    const auto Context = MakeContext("contract-provider-failed");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {}, Providers::ResponseFailedEvent{
                .Metadata = Metadata(Request, Context.AgentTurn, 1),
                .Failure = Providers::ProviderError{
                    .Code = Providers::FailureCode::RateLimited,
                    .Retryable = false,
                    .SanitizedDescription = "synthetic rate limit",
                },
            }));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() == Core::AgentRunFailureCode::RateLimited,
          "provider failures must preserve their typed failure code");
  }
  {
    const auto Context = MakeContext("contract-cancelled");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {}, Providers::ResponseCancelledEvent{
                .Metadata = Metadata(Request, Context.AgentTurn, 1),
                .Reason = Providers::CancelReasonCode{
                    .Code = Providers::StableReasonCode{"synthetic-cancel"}},
            }));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Cancelled,
          "provider cancellation must remain distinct");
  }
  {
    const auto Context = MakeContext("contract-unknown");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {}, Providers::ResponseOutcomeUnknownEvent{
                .Metadata = Metadata(Request, Context.AgentTurn, 1),
                .Reason = Providers::OutcomeUnknownReasonCode{
                    .Code = Providers::StableReasonCode{"synthetic-unknown"}},
            }));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::UnknownOutcome,
          "unknown provider outcome must remain distinct and never retry");
  }
}

void TestRequestTurnAndSequenceMismatchFailClosed() {
  const auto ExpectInvalidStream = [](FakeProvider::Script Script,
                                      const char* Message) {
    const auto Context = MakeContext("contract-invalid-stream");
    RawScriptProvider Provider(std::move(Script.NonTerminalEvents),
                               std::move(Script.Terminal));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() == Core::AgentRunFailureCode::InvalidStream,
          Message);
  };

  const auto Context = MakeContext("contract-invalid-stream");
  const auto Request = ExpectedRequestId(Context.AgentTurn);
  ExpectInvalidStream(
      MakeScript(
          {TextEvent(Providers::RequestId{"wrong-request"}, Context.AgentTurn,
                     1, "forged correlation")},
          Completed(Request, Context.AgentTurn, 2,
                    Providers::CompletionKind::FinalAnswer)),
      "mismatched RequestId must fail closed");
  ExpectInvalidStream(
      MakeScript(
          {TextEvent(Request, Providers::AgentTurnId{"wrong-turn"}, 1,
                     "forged turn")},
          Completed(Request, Context.AgentTurn, 2,
                    Providers::CompletionKind::FinalAnswer)),
      "mismatched AgentTurnId must fail closed");
  ExpectInvalidStream(
      MakeScript(
          {TextEvent(Request, Context.AgentTurn, 2, "sequence gap")},
          Completed(Request, Context.AgentTurn, 3,
                    Providers::CompletionKind::FinalAnswer)),
      "non-contiguous provider sequence numbers must fail closed");
  ExpectInvalidStream(
      MakeScript(
          {TextEvent(Request, Context.AgentTurn, 1, "valid delta")},
          Completed(Request, Context.AgentTurn, 3,
                    Providers::CompletionKind::FinalAnswer)),
      "terminal sequence must immediately follow the last event");
}

void TestProviderCapabilitiesGateUnsupportedInputs() {
  {
    const auto Context = MakeContext(
        "contract-image-unsupported",
        Channels::CanonicalMessage{
            .MessageParts = {Channels::MediaReferencePart{
                .Reference = Channels::MediaReferenceId{"image-synthetic"},
                .Kind = Channels::MediaReferenceKind::Image,
            }},
            .ReplyToId = std::nullopt,
            .MentionedIds = {},
            .PlatformMetadata = {},
        });
    FakeProvider Provider(Providers::ProviderCapabilities{
        .Streaming = true,
        .ToolCalls = false,
        .Vision = false,
        .Usage = true,
        .Cancellation = true,
        .OutputLimit = true,
    });
    Provider.EnqueueScript(MakeScript(
        {}, Completed(ExpectedRequestId(Context.AgentTurn), Context.AgentTurn,
                      1, Providers::CompletionKind::FinalAnswer)));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Unsupported &&
              Provider.GetCapturedRequests().empty() &&
              Provider.GetRemainingScriptCount() == 1,
          "image input without Provider vision support must be rejected pre-request");
  }
  {
    const auto Context = MakeContext("contract-limit-unsupported");
    FakeProvider Provider(Providers::ProviderCapabilities{
        .Streaming = true,
        .ToolCalls = false,
        .Vision = true,
        .Usage = true,
        .Cancellation = true,
        .OutputLimit = false,
    });
    Provider.EnqueueScript(MakeScript(
        {}, Completed(ExpectedRequestId(Context.AgentTurn), Context.AgentTurn,
                      1, Providers::CompletionKind::FinalAnswer)));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Unsupported &&
              Provider.GetCapturedRequests().empty() &&
              Provider.GetRemainingScriptCount() == 1,
          "an unsupported configured output limit must fail before StreamTurn");
  }
}

void TestInvalidInputsAndPreCancelledTurnsDoNotCallProvider() {
  const auto ExpectNoProviderCall = [](
      Core::AuthorizedTurnContext Context,
      std::stop_token CancellationToken,
      Core::AgentRunStatus ExpectedStatus,
      std::optional<Core::AgentRunFailureCode> ExpectedFailure,
      const char* Message) {
    FakeProvider Provider;
    Provider.EnqueueScript(FakeProvider::Script{});
    Agent::AgentRuntime Runtime;
    TestProviderCredential Credential;
    const auto Result = Runtime.RunTurn(
        Context, Provider, MakeProfile(), Credential, CancellationToken);
    Check(Result.GetStatus() == ExpectedStatus &&
              Result.GetFailureCode() == ExpectedFailure,
          Message);
    Check(Provider.GetCapturedRequests().empty() &&
              Provider.GetRemainingScriptCount() == 1,
          "invalid or cancelled input must not start a Provider request");
  };

  auto EmptyTurn = MakeContext("contract-empty-turn");
  EmptyTurn.AgentTurn.Value.clear();
  ExpectNoProviderCall(
      std::move(EmptyTurn), {}, Core::AgentRunStatus::Failed,
      Core::AgentRunFailureCode::InvalidRequest,
      "empty Core AgentTurnId must fail before Provider dispatch");

  auto EmptyMessage = MakeContext(
      "contract-empty-message",
      Channels::CanonicalMessage{
          .MessageParts = {},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      });
  ExpectNoProviderCall(
      std::move(EmptyMessage), {}, Core::AgentRunStatus::Failed,
      Core::AgentRunFailureCode::InvalidRequest,
      "empty canonical input must fail before Provider dispatch");

  auto InvalidImage = MakeContext(
      "contract-invalid-image-reference",
      Channels::CanonicalMessage{
          .MessageParts = {Channels::MediaReferencePart{
              .Reference = Channels::MediaReferenceId{""},
              .Kind = Channels::MediaReferenceKind::Image,
          }},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      });
  ExpectNoProviderCall(
      std::move(InvalidImage), {}, Core::AgentRunStatus::Failed,
      Core::AgentRunFailureCode::InvalidRequest,
      "empty media reference must fail before Provider dispatch");

  auto UnsupportedMedia = MakeContext(
      "contract-unsupported-media",
      Channels::CanonicalMessage{
          .MessageParts = {Channels::MediaReferencePart{
              .Reference = Channels::MediaReferenceId{"audio-synthetic"},
              .Kind = Channels::MediaReferenceKind::Audio,
          }},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      });
  ExpectNoProviderCall(
      std::move(UnsupportedMedia), {}, Core::AgentRunStatus::Unsupported,
      std::nullopt,
      "unsupported media kind must fail closed before Provider dispatch");

  std::stop_source Cancellation;
  Cancellation.request_stop();
  ExpectNoProviderCall(
      MakeContext("contract-pre-cancelled"), Cancellation.get_token(),
      Core::AgentRunStatus::Cancelled, std::nullopt,
      "pre-cancelled turn must not start a Provider request");
}

void TestAnthropicRefusalDiscardsPriorText() {
  const auto Context = MakeContext("contract-anthropic-refusal");
  const auto Request = ExpectedRequestId(Context.AgentTurn);
  FakeProvider Provider;
  Provider.EnqueueScript(MakeScript(
      {TextEvent(Request, Context.AgentTurn, 1, "partial must be discarded")},
      Completed(Request, Context.AgentTurn, 2,
                Providers::CompletionKind::Refusal,
                Providers::RefusalStopDetails{
                    .Type = Providers::RefusalStopDetailsType::Refusal,
                    .Category = Providers::RefusalCategory::GeneralHarms,
                })));

  const auto Result = RunScript(
      Context, MakeProfile(Providers::ProviderProtocol::AnthropicMessages),
      Provider);
  Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
            Result.GetFailureCode() == Core::AgentRunFailureCode::InvalidStream &&
            Result.GetFinalText().empty(),
        "Anthropic refusal must never expose prior partial TextDelta as final text");
}

class ConfigurableProvider final : public Providers::ModelProviderPort {
public:
  std::optional<Providers::ProviderError> ValidateError;
  std::optional<Providers::ProviderError> CapabilitiesError;
  bool ThrowOnValidate{false};
  bool ThrowOnStream{false};
  std::vector<Providers::CanonicalNonTerminalEvent> Events;
  std::function<Providers::CanonicalTerminalEvent(
      const Providers::CanonicalModelRequest&)>
      TerminalFactory;
  Providers::ProviderCapabilities Capabilities{true, true, true, true, true,
                                               true};
  std::size_t StreamCalls{0};

  Providers::ProviderResult<Providers::ProviderCapabilities> GetCapabilities(
      const Providers::ProviderProfileId&) const override {
    if (CapabilitiesError.has_value()) {
      return *CapabilitiesError;
    }
    return Capabilities;
  }

  Providers::ProviderResult<std::monostate> ValidateProfile(
      const Providers::ModelProviderProfile&) const override {
    if (ThrowOnValidate) {
      throw std::runtime_error("synthetic validation failure");
    }
    if (ValidateError.has_value()) {
      return *ValidateError;
    }
    return std::monostate{};
  }

  Providers::CanonicalTerminalEvent StreamTurn(
      const Providers::ModelProviderProfile&,
      const Providers::CredentialLease&,
      const Providers::CanonicalModelRequest& Request, std::stop_token,
      const Providers::EventSink& Sink) override {
    ++StreamCalls;
    if (ThrowOnStream) {
      throw std::runtime_error("synthetic stream failure");
    }
    for (const auto& Event : Events) {
      Sink(Event);
    }
    return TerminalFactory(Request);
  }

  Providers::ProviderResult<Providers::ProviderHealthStatus> HealthCheck(
      const Providers::ModelProviderProfile&) override {
    return Providers::ProviderHealthStatus{
        Providers::ProviderHealthState::Available};
  }
};

Providers::CanonicalTerminalEvent FinalTerminalAfter(
    const Providers::CanonicalModelRequest& Request,
    std::uint64_t Sequence) {
  return Providers::ResponseCompletedEvent{
      .Metadata = Providers::ProviderEventMetadata{
          .Request = Request.Id,
          .TurnId = Request.TurnId,
          .SequenceNumber = Sequence,
      },
      .Kind = Providers::CompletionKind::FinalAnswer,
      .StopSequenceCode = std::nullopt,
      .FinalUsage = std::nullopt,
      .StopDetails = std::nullopt,
  };
}

void TestProviderProfileAndCapabilityErrorsMap() {
  const auto Context = MakeContext("contract-provider-errors");
  {
    ConfigurableProvider Provider;
    Provider.ValidateError = Providers::ProviderError{
        Providers::FailureCode::AuthenticationFailure, false,
        "synthetic authentication failure"};
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::AuthenticationFailure,
          "ValidateProfile errors must map to the matching Agent failure");
    Check(Provider.StreamCalls == 0,
          "profile validation failures must not reach StreamTurn");
  }
  {
    ConfigurableProvider Provider;
    Provider.CapabilitiesError = Providers::ProviderError{
        Providers::FailureCode::RateLimited, true, "synthetic rate limit"};
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::RateLimited,
          "GetCapabilities errors must map to the matching Agent failure");
    Check(Provider.StreamCalls == 0,
          "capability failures must not reach StreamTurn");
  }
}

void TestProviderExceptionsMapToSafeOutcomes() {
  const auto Context = MakeContext("contract-provider-exceptions");
  {
    ConfigurableProvider Provider;
    Provider.ThrowOnValidate = true;
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::InternalError,
          "unexpected validation exceptions must map to InternalError");
  }
  {
    ConfigurableProvider Provider;
    Provider.ThrowOnStream = true;
    Provider.Events.clear();
    Provider.TerminalFactory = [](const Providers::CanonicalModelRequest& R) {
      return FinalTerminalAfter(R, 1);
    };
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::UnknownOutcome,
          "stream exceptions must map to UnknownOutcome");
    Check(Provider.StreamCalls == 1,
          "stream exceptions must be observed once without retry");
  }
}

void TestInvalidStreamMetadataFailsClosed() {
  const auto ExpectInvalidStream = [](
      std::string Turn,
      const std::vector<Providers::CanonicalNonTerminalEvent>& Events,
      Providers::CanonicalTerminalEvent Terminal, const char* Message) {
    const auto Context = MakeContext(std::move(Turn));
    RawScriptProvider Provider(Events, std::move(Terminal));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::InvalidStream,
          Message);
  };

  {
    const auto Context = MakeContext("contract-wrong-request");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    ExpectInvalidStream(
        "contract-wrong-request",
        {TextEvent(Providers::RequestId{"other-request"},
                   Context.AgentTurn, 1, "delta")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::FinalAnswer),
        "a nonterminal event from another request must fail closed");
  }
  {
    const auto Context = MakeContext("contract-wrong-turn");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    ExpectInvalidStream(
        "contract-wrong-turn",
        {TextEvent(Request, Providers::AgentTurnId{"other-turn"}, 1, "delta")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::FinalAnswer),
        "a nonterminal event from another turn must fail closed");
  }
  {
    const auto Context = MakeContext("contract-duplicate-sequence");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    ExpectInvalidStream(
        "contract-duplicate-sequence",
        {TextEvent(Request, Context.AgentTurn, 1, "delta"),
         TextEvent(Request, Context.AgentTurn, 1, "delta again")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::FinalAnswer),
        "duplicate nonterminal sequence numbers must fail closed");
  }
  {
    const auto Context = MakeContext("contract-sequence-gap");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    ExpectInvalidStream(
        "contract-sequence-gap",
        {TextEvent(Request, Context.AgentTurn, 2, "delta")},
        Completed(Request, Context.AgentTurn, 3,
                  Providers::CompletionKind::FinalAnswer),
        "gapped nonterminal sequence numbers must fail closed");
  }
  {
    const auto Context = MakeContext("contract-terminal-request");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    ExpectInvalidStream(
        "contract-terminal-request",
        {TextEvent(Request, Context.AgentTurn, 1, "delta")},
        Completed(Providers::RequestId{"other-request"}, Context.AgentTurn, 2,
                  Providers::CompletionKind::FinalAnswer),
        "a terminal event from another request must fail closed");
  }
  {
    const auto Context = MakeContext("contract-terminal-sequence");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    ExpectInvalidStream(
        "contract-terminal-sequence",
        {TextEvent(Request, Context.AgentTurn, 1, "delta")},
        Completed(Request, Context.AgentTurn, 1,
                  Providers::CompletionKind::FinalAnswer),
        "a duplicated terminal sequence must fail closed");
  }
}

void TestNonStreamingProviderIsUnsupported() {
  const auto Context = MakeContext("contract-non-streaming");
  ConfigurableProvider Provider;
  Provider.Capabilities = Providers::ProviderCapabilities{
      .Streaming = false,
      .ToolCalls = false,
      .Vision = true,
      .Usage = true,
      .Cancellation = true,
      .OutputLimit = true,
  };
  const auto Result = RunScript(Context, MakeProfile(), Provider);
  Check(Result.GetStatus() == Core::AgentRunStatus::Unsupported,
        "providers without streaming must be rejected before dispatch");
  Check(Provider.StreamCalls == 0,
        "non-streaming rejection must not reach StreamTurn");
}

void TestToolEventsNeverAuthorizeExecution() {
  const auto RunToolEvent = [](std::string Turn,
                               Providers::CanonicalNonTerminalEvent ToolEvent) {
    const auto Context = MakeContext(std::move(Turn));
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {std::move(ToolEvent)},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::FinalAnswer)));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Unsupported,
          "tool call events must never authorize business actions");
  };

  RunToolEvent(
      "contract-tool-started",
      Providers::ToolCallStartedEvent{
          .Metadata = {},
          .Item = Providers::ItemId{"item-tool"},
          .Call = Providers::ToolCallId{"call-tool"},
          .ToolName = "unsupported-tool",
      });
  RunToolEvent(
      "contract-tool-arguments",
      Providers::ToolArgumentDeltaEvent{
          .Metadata = {},
          .Item = Providers::ItemId{"item-tool"},
          .Call = Providers::ToolCallId{"call-tool"},
          .JsonTextDelta = "{}",
      });
  RunToolEvent(
      "contract-tool-ready",
      Providers::ToolCallReadyEvent{
          .Metadata = {},
          .Item = Providers::ItemId{"item-tool"},
          .Call = Providers::ToolCallId{"call-tool"},
          .ToolName = "unsupported-tool",
          .Arguments = Providers::JsonValue{"{}"},
      });
}

void TestCompletedTerminalMatrixFailsClosed() {
  {
    const auto Context = MakeContext("contract-stop-details-final");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {TextEvent(Request, Context.AgentTurn, 1, "text")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::FinalAnswer,
                  Providers::RefusalStopDetails{
                      .Type = Providers::RefusalStopDetailsType::Refusal,
                      .Category = Providers::RefusalCategory::GeneralHarms,
                  })));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::InvalidStream,
          "FinalAnswer with refusal stop details must fail closed");
  }
  {
    const auto Context = MakeContext("contract-tool-handoff-no-events");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {}, Completed(Request, Context.AgentTurn, 1,
                      Providers::CompletionKind::ToolHandoff)));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Unsupported,
          "ToolHandoff completion must remain unsupported");
  }
  {
    const auto Context = MakeContext("contract-anthropic-refusal-missing-details");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {RefusalEvent(Request, Context.AgentTurn, 1, "refusal")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::Refusal)));
    const auto Result = RunScript(
        Context, MakeProfile(Providers::ProviderProtocol::AnthropicMessages),
        Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::InvalidStream,
          "Anthropic refusals without typed stop details must fail closed");
  }
  {
    const auto Context = MakeContext("contract-chat-refusal-mixed-text");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {TextEvent(Request, Context.AgentTurn, 1, "visible text"),
         RefusalEvent(Request, Context.AgentTurn, 2, "refusal")},
        Completed(Request, Context.AgentTurn, 3,
                  Providers::CompletionKind::Refusal)));
    const auto Result = RunScript(
        Context,
        MakeProfile(Providers::ProviderProtocol::
                        OpenAICompatibleChatCompletions),
        Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::InvalidStream,
          "Chat Completions refusals mixed with visible text must fail closed");
  }
  {
    const auto Context = MakeContext("contract-final-with-refusal-delta");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {RefusalEvent(Request, Context.AgentTurn, 1, "refusal")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::FinalAnswer)));
    const auto Result = RunScript(Context, MakeProfile(), Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
              Result.GetFailureCode() ==
                  Core::AgentRunFailureCode::InvalidStream,
          "FinalAnswer after refusal deltas must fail closed");
  }
}

void TestVisionRequestMapsApprovedImageReference() {
  const auto Context = MakeContext(
      "contract-vision-supported",
      Channels::CanonicalMessage{
          .MessageParts =
              {Channels::TextPart{"describe"},
               Channels::MediaReferencePart{
                   .Reference =
                       Channels::MediaReferenceId{"image-synthetic"},
                   .Kind = Channels::MediaReferenceKind::Image,
               }},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      });
  const auto Request = ExpectedRequestId(Context.AgentTurn);
  FakeProvider Provider;
  Provider.EnqueueScript(MakeScript(
      {TextEvent(Request, Context.AgentTurn, 1, "description")},
      Completed(Request, Context.AgentTurn, 2,
                Providers::CompletionKind::FinalAnswer)));
  const auto Result = RunScript(Context, MakeProfile(), Provider);
  Check(Result.GetStatus() == Core::AgentRunStatus::ResponseReady,
        "vision-capable providers should accept approved image references");
  const auto& Requests = Provider.GetCapturedRequests();
  Check(Requests.size() == 1 &&
            Requests[0].Messages.size() == 1 &&
            Requests[0].Messages[0].ContentParts.size() == 2,
        "text plus image content must map into one canonical message");
  Check(std::holds_alternative<Providers::ImageContentReference>(
            Requests[0].Messages[0].ContentParts[1]),
        "the approved image reference must stay typed");
}

void TestUnknownFailureCodesFailClosed() {
  const auto Context = MakeContext("contract-unknown-failure");
  const auto Request = ExpectedRequestId(Context.AgentTurn);
  RawScriptProvider Provider(
      {},
      Providers::ResponseFailedEvent{
          .Metadata = Metadata(Request, Context.AgentTurn, 1),
          .Failure = Providers::ProviderError{
              static_cast<Providers::FailureCode>(250), false,
              "synthetic unrecognized failure"},
      });
  const auto Result = RunScript(Context, MakeProfile(), Provider);
  Check(Result.GetStatus() == Core::AgentRunStatus::Failed &&
            Result.GetFailureCode() ==
                Core::AgentRunFailureCode::InternalError,
        "unrecognized provider failure codes must map to InternalError");
}

void TestTypedRefusalsCompleteForTheirProtocols() {
  {
    const auto Context =
        MakeContext("contract-anthropic-typed-refusal");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {RefusalEvent(Request, Context.AgentTurn, 1, "cannot comply")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::Refusal,
                  Providers::RefusalStopDetails{
                      .Type = Providers::RefusalStopDetailsType::Refusal,
                      .Category = Providers::RefusalCategory::GeneralHarms,
                  })));
    const auto Result = RunScript(
        Context, MakeProfile(Providers::ProviderProtocol::AnthropicMessages),
        Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::ResponseReady &&
              Result.GetCompletionKind() ==
                  Providers::CompletionKind::Refusal &&
              Result.GetFinalText() == "cannot comply",
          "typed Anthropic refusals must complete as refusals");
  }
  {
    const auto Context =
        MakeContext("contract-chat-typed-refusal");
    const auto Request = ExpectedRequestId(Context.AgentTurn);
    FakeProvider Provider;
    Provider.EnqueueScript(MakeScript(
        {RefusalEvent(Request, Context.AgentTurn, 1, "cannot comply")},
        Completed(Request, Context.AgentTurn, 2,
                  Providers::CompletionKind::Refusal)));
    const auto Result = RunScript(
        Context,
        MakeProfile(Providers::ProviderProtocol::
                        OpenAICompatibleChatCompletions),
        Provider);
    Check(Result.GetStatus() == Core::AgentRunStatus::ResponseReady &&
              Result.GetCompletionKind() ==
                  Providers::CompletionKind::Refusal &&
              Result.GetFinalText() == "cannot comply",
          "refusal-only Chat Completions responses must complete as refusals");
  }
}

} // namespace

int main() {
  TestFinalAnswerCreatesOneCorrelatedRequest();
  TestRefusalNeedsNonEmptyRefusalText();
  TestToolHandoffAndEmptyFinalFailClosed();
  TestOutputLimitNeverReturnsPartialTextOrRetries();
  TestCoreApprovalAllowsOnlyFreshRequestContinuation();
  TestProviderFailureCancelAndUnknownStayDistinct();
  TestRequestTurnAndSequenceMismatchFailClosed();
  TestProviderCapabilitiesGateUnsupportedInputs();
  TestInvalidInputsAndPreCancelledTurnsDoNotCallProvider();
  TestAnthropicRefusalDiscardsPriorText();
  TestProviderProfileAndCapabilityErrorsMap();
  TestProviderExceptionsMapToSafeOutcomes();
  TestInvalidStreamMetadataFailsClosed();
  TestNonStreamingProviderIsUnsupported();
  TestToolEventsNeverAuthorizeExecution();
  TestCompletedTerminalMatrixFailsClosed();
  TestVisionRequestMapsApprovedImageReference();
  TestUnknownFailureCodesFailClosed();
  TestTypedRefusalsCompleteForTheirProtocols();
  return 0;
}
