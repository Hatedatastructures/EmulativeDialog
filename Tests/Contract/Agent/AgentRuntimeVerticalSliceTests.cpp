#include "Source/Agent/AgentRuntime.h"
#include "Source/Core/CoreChatService.h"
#include "Source/Core/IdentityPolicy.h"
#include "Tests/Contract/Channels/Contracts/MockChannelAdapter.h"
#include "Tests/Contract/Providers/Contracts/FakeProvider.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace EmulativeDialog::Channels;
using EmulativeDialog::Channels::Testing::MockActionReceipt;
using EmulativeDialog::Channels::Testing::MockChannelAdapter;
using EmulativeDialog::Channels::Testing::MockChannelConfiguration;
using EmulativeDialog::Core::CoreActionIdentifiers;
using EmulativeDialog::Core::CoreActionIdSource;
using EmulativeDialog::Core::CoreChatResult;
using EmulativeDialog::Core::CoreChatService;
using EmulativeDialog::Core::CoreChatStatus;
using EmulativeDialog::Core::IdentityBinding;
using EmulativeDialog::Core::IdentityPolicy;
using EmulativeDialog::Core::PrincipalId;
using EmulativeDialog::Providers::Testing::FakeProvider;

void Check(bool Condition, const char* Message) {
  if (!Condition) {
    std::cerr << Message << '\n';
    std::exit(1);
  }
}

class TestChannelCredential final : public EmulativeDialog::Core::CredentialLease {
public:
  std::string_view GetAuthenticationSecret() const noexcept override {
    return {};
  }
};

class TestProviderCredential final
    : public EmulativeDialog::Providers::CredentialLease {
public:
  std::string_view GetAuthenticationSecret() const noexcept override {
    return {};
  }
};

enum class ProviderScenario {
  FinalAnswer,
  Refusal,
  ToolHandoff,
  EmptyFinal,
  OutputLimited,
  Failure,
  Cancelled,
  UnknownOutcome,
  InvalidSequence,
  AnthropicPartialRefusal,
};

const char* ScenarioName(ProviderScenario Scenario) {
  switch (Scenario) {
  case ProviderScenario::FinalAnswer: return "FinalAnswer";
  case ProviderScenario::Refusal: return "Refusal";
  case ProviderScenario::ToolHandoff: return "ToolHandoff";
  case ProviderScenario::EmptyFinal: return "EmptyFinal";
  case ProviderScenario::OutputLimited: return "OutputLimited";
  case ProviderScenario::Failure: return "Failure";
  case ProviderScenario::Cancelled: return "Cancelled";
  case ProviderScenario::UnknownOutcome: return "UnknownOutcome";
  case ProviderScenario::InvalidSequence: return "InvalidSequence";
  case ProviderScenario::AnthropicPartialRefusal: return "AnthropicPartialRefusal";
  }
  return "InvalidScenario";
}

EmulativeDialog::Providers::ProviderEventMetadata Metadata(
    const EmulativeDialog::Providers::CanonicalModelRequest& Request,
    std::uint64_t Sequence) {
  return EmulativeDialog::Providers::ProviderEventMetadata{
      .Request = Request.Id,
      .TurnId = Request.TurnId,
      .SequenceNumber = Sequence,
  };
}

FakeProvider::Script MakeProviderScript(
    ProviderScenario Scenario,
    const EmulativeDialog::Providers::CanonicalModelRequest& Request) {
  using namespace EmulativeDialog::Providers;
  FakeProvider::Script Script;
  switch (Scenario) {
  case ProviderScenario::FinalAnswer:
    Script.NonTerminalEvents = {
        CanonicalNonTerminalEvent{ResponseStartedEvent{
            .Metadata = Metadata(Request, 1),
            .ProviderResponse = ProviderResponseId{"response-synthetic"},
        }},
        CanonicalNonTerminalEvent{TextDeltaEvent{
            .Metadata = Metadata(Request, 2),
            .Item = ItemId{"item-synthetic"},
            .Utf8Delta = "Synthetic vertical answer.",
        }},
    };
    Script.Terminal = ResponseCompletedEvent{
        .Metadata = Metadata(Request, 3),
        .Kind = CompletionKind::FinalAnswer,
        .StopSequenceCode = std::nullopt,
        .FinalUsage = std::nullopt,
        .StopDetails = std::nullopt,
    };
    break;
  case ProviderScenario::Refusal:
    Script.NonTerminalEvents = {
        CanonicalNonTerminalEvent{RefusalDeltaEvent{
            .Metadata = Metadata(Request, 1),
            .Item = ItemId{"item-refusal-synthetic"},
            .Utf8Delta = "Synthetic refusal.",
            .Category = RefusalCategory::GeneralHarms,
        }},
    };
    Script.Terminal = ResponseCompletedEvent{
        .Metadata = Metadata(Request, 2),
        .Kind = CompletionKind::Refusal,
        .StopSequenceCode = std::nullopt,
        .FinalUsage = std::nullopt,
        .StopDetails = std::nullopt,
    };
    break;
  case ProviderScenario::ToolHandoff:
    Script.NonTerminalEvents = {
        CanonicalNonTerminalEvent{ToolCallReadyEvent{
            .Metadata = Metadata(Request, 1),
            .Item = ItemId{"item-tool-synthetic"},
            .Call = ToolCallId{"call-synthetic"},
            .ToolName = "not-authorized",
            .Arguments = JsonValue{"{}"},
        }},
    };
    Script.Terminal = ResponseCompletedEvent{
        .Metadata = Metadata(Request, 2),
        .Kind = CompletionKind::ToolHandoff,
        .StopSequenceCode = std::nullopt,
        .FinalUsage = std::nullopt,
        .StopDetails = std::nullopt,
    };
    break;
  case ProviderScenario::EmptyFinal:
    Script.Terminal = ResponseCompletedEvent{
        .Metadata = Metadata(Request, 1),
        .Kind = CompletionKind::FinalAnswer,
        .StopSequenceCode = std::nullopt,
        .FinalUsage = std::nullopt,
        .StopDetails = std::nullopt,
    };
    break;
  case ProviderScenario::OutputLimited:
    Script.NonTerminalEvents = {
        CanonicalNonTerminalEvent{TextDeltaEvent{
            .Metadata = Metadata(Request, 1),
            .Item = ItemId{"item-partial"},
            .Utf8Delta = "must not be sent",
        }},
    };
    Script.Terminal = ResponseOutputLimitedEvent{
        .Metadata = Metadata(Request, 2),
        .Kind = LimitKind::OutputTokens,
        .Usage = UsageSnapshot{},
        .SafeGeneratedText = "must not be sent",
    };
    break;
  case ProviderScenario::Failure:
    Script.Terminal = ResponseFailedEvent{
        .Metadata = Metadata(Request, 1),
        .Failure = ProviderError{
            .Code = FailureCode::RateLimited,
            .Retryable = false,
            .SanitizedDescription = "synthetic rate limit",
        },
    };
    break;
  case ProviderScenario::Cancelled:
    Script.Terminal = ResponseCancelledEvent{
        .Metadata = Metadata(Request, 1),
        .Reason = CancelReasonCode{
            .Code = StableReasonCode{"synthetic-cancel"},
        },
    };
    break;
  case ProviderScenario::UnknownOutcome:
    Script.Terminal = ResponseOutcomeUnknownEvent{
        .Metadata = Metadata(Request, 1),
        .Reason = OutcomeUnknownReasonCode{
            .Code = StableReasonCode{"synthetic-unknown"},
        },
    };
    break;
  case ProviderScenario::InvalidSequence:
    Script.NonTerminalEvents = {
        CanonicalNonTerminalEvent{TextDeltaEvent{
            .Metadata = Metadata(Request, 2),
            .Item = ItemId{"item-gap"},
            .Utf8Delta = "invalid sequence",
        }},
    };
    Script.Terminal = ResponseCompletedEvent{
        .Metadata = Metadata(Request, 3),
        .Kind = CompletionKind::FinalAnswer,
        .StopSequenceCode = std::nullopt,
        .FinalUsage = std::nullopt,
        .StopDetails = std::nullopt,
    };
    break;
  case ProviderScenario::AnthropicPartialRefusal:
    Script.NonTerminalEvents = {
        CanonicalNonTerminalEvent{TextDeltaEvent{
            .Metadata = Metadata(Request, 1),
            .Item = ItemId{"item-partial-refusal"},
            .Utf8Delta = "discard this partial text",
        }},
    };
    Script.Terminal = ResponseCompletedEvent{
        .Metadata = Metadata(Request, 2),
        .Kind = CompletionKind::Refusal,
        .StopSequenceCode = std::nullopt,
        .FinalUsage = std::nullopt,
        .StopDetails = RefusalStopDetails{
            .Type = RefusalStopDetailsType::Refusal,
            .Category = RefusalCategory::GeneralHarms,
        },
    };
    break;
  }
  return Script;
}

class RequestBoundFakeProvider final : public EmulativeDialog::Providers::ModelProviderPort {
public:
  explicit RequestBoundFakeProvider(ProviderScenario Scenario)
      : Scenario(Scenario), Fake(EmulativeDialog::Providers::ProviderCapabilities{
            .Streaming = true,
            .ToolCalls = true,
            .Vision = true,
            .Usage = true,
            .Cancellation = true,
            .OutputLimit = true,
        }) {}

  EmulativeDialog::Providers::ProviderResult<
      EmulativeDialog::Providers::ProviderCapabilities>
  GetCapabilities(
      const EmulativeDialog::Providers::ProviderProfileId& ProfileId) const override {
    return Fake.GetCapabilities(ProfileId);
  }

  EmulativeDialog::Providers::ProviderResult<std::monostate> ValidateProfile(
      const EmulativeDialog::Providers::ModelProviderProfile& Profile) const override {
    return Fake.ValidateProfile(Profile);
  }

  EmulativeDialog::Providers::CanonicalTerminalEvent StreamTurn(
      const EmulativeDialog::Providers::ModelProviderProfile& Profile,
      const EmulativeDialog::Providers::CredentialLease& Lease,
      const EmulativeDialog::Providers::CanonicalModelRequest& Request,
      std::stop_token CancellationToken,
      const EmulativeDialog::Providers::EventSink& Sink) override {
    CapturedRequests.push_back(Request);
    if (Scenario == ProviderScenario::InvalidSequence) {
      using namespace EmulativeDialog::Providers;
      Sink(CanonicalNonTerminalEvent{TextDeltaEvent{
          .Metadata = Metadata(Request, 2),
          .Item = ItemId{"item-invalid-sequence"},
          .Utf8Delta = "must be rejected",
      }});
      return ResponseCompletedEvent{
          .Metadata = Metadata(Request, 3),
          .Kind = CompletionKind::FinalAnswer,
          .StopSequenceCode = std::nullopt,
          .FinalUsage = std::nullopt,
          .StopDetails = std::nullopt,
      };
    }
    Fake.EnqueueScript(MakeProviderScript(Scenario, Request));
    return Fake.StreamTurn(Profile, Lease, Request, CancellationToken, Sink);
  }

  EmulativeDialog::Providers::ProviderResult<
      EmulativeDialog::Providers::ProviderHealthStatus>
  HealthCheck(
      const EmulativeDialog::Providers::ModelProviderProfile& Profile) override {
    return Fake.HealthCheck(Profile);
  }

  const std::vector<EmulativeDialog::Providers::CanonicalModelRequest>&
  GetCapturedRequests() const noexcept {
    return CapturedRequests;
  }

private:
  ProviderScenario Scenario;
  FakeProvider Fake;
  std::vector<EmulativeDialog::Providers::CanonicalModelRequest>
      CapturedRequests;
};

class ScriptedActionIdSource final : public CoreActionIdSource {
public:
  explicit ScriptedActionIdSource(CoreActionIdentifiers Identifiers)
      : Identifiers(std::move(Identifiers)) {}

  CoreActionIdentifiers Next() override {
    ++Calls;
    return Identifiers;
  }

  CoreActionIdentifiers Identifiers;
  std::size_t Calls = 0;
};

class CollectingSink final : public ChannelEventSink {
public:
  void OnChannelMessage(const InboundChannelMessage& Message) override {
    Events.push_back(Message);
  }

  std::vector<InboundChannelMessage> Events;
};

AccountProfile MakeChannelProfile() {
  return AccountProfile{
      .Channel = ChannelId{"Mock"},
      .Account = AccountId{"account-vertical-synthetic"},
      .Mode = IntegrationMode{"Mock"},
  };
}

CapabilitySnapshot MakeChannelCapabilities(const AccountProfile& Profile) {
  return CapabilitySnapshot{
      .Channel = Profile.Channel,
      .Account = Profile.Account,
      .Mode = Profile.Mode,
      .SnapshotVersion = 17,
      .CapturedAt = Timestamp{},
      .ValidUntil = std::nullopt,
      .AuthenticationStatus = VerificationStatus::Verified,
      .AuditStatus = VerificationStatus::Verified,
      .Capabilities = {
          CapabilityObservation{
              .Capability = ChannelCapability::ReceiveText,
              .Status = CapabilityStatus::Supported,
              .EvidenceStatus = VerificationStatus::Verified,
              .ObservedAt = Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = EvidenceReference{"vertical-receive"},
          },
          CapabilityObservation{
              .Capability = ChannelCapability::SendText,
              .Status = CapabilityStatus::Supported,
              .EvidenceStatus = VerificationStatus::Verified,
              .ObservedAt = Timestamp{},
              .ValidUntil = std::nullopt,
              .Evidence = EvidenceReference{"vertical-send"},
          },
      },
  };
}

InboundChannelMessage MakeInboundMessage() {
  return InboundChannelMessage{
      .Envelope = ChannelEnvelope{
          .Channel = ChannelId{"Mock"},
          .Mode = IntegrationMode{"Mock"},
          .Account = AccountId{"account-vertical-synthetic"},
          .Conversation = ConversationId{"conversation-vertical-synthetic"},
          .ConversationType = ConversationKind::Private,
          .Sender = SenderId{"sender-vertical-synthetic"},
          .Message = MessageId{"message-vertical-synthetic"},
          .PlatformEvent = PlatformEventId{"event-vertical-synthetic"},
          .OccurredAt = Timestamp{},
          .StableDeduplication =
              StableDeduplicationId{"dedup-vertical-synthetic"},
      },
      .Message = CanonicalMessage{
          .MessageParts = {TextPart{"synthetic inbound question"}},
          .ReplyToId = std::nullopt,
          .MentionedIds = {},
          .PlatformMetadata = {},
      },
  };
}

EmulativeDialog::Providers::ModelProviderProfile MakeProviderProfile(
    EmulativeDialog::Providers::ProviderProtocol Protocol) {
  EmulativeDialog::Providers::ModelProviderProfile Profile;
  Profile.Id = EmulativeDialog::Providers::ProviderProfileId{
      "profile-vertical-synthetic"};
  Profile.Protocol = Protocol;
  Profile.Endpoint = "https://fake-provider.invalid";
  Profile.ModelName = "fake-vertical-model";
  Profile.Credential = EmulativeDialog::Providers::CredentialRef{
      "credential-vertical-synthetic"};
  Profile.Authentication =
      Protocol == EmulativeDialog::Providers::ProviderProtocol::AnthropicMessages
          ? EmulativeDialog::Providers::AuthenticationScheme::XApiKeyHeader
          : EmulativeDialog::Providers::AuthenticationScheme::Bearer;
  if (Protocol ==
      EmulativeDialog::Providers::ProviderProtocol::AnthropicMessages) {
    Profile.ProtocolVersion = "2026-10-21";
  }
  Profile.Limit.MaximumOutputTokens = 64;
  Profile.Capabilities = EmulativeDialog::Providers::ProviderCapabilities{
      .Streaming = true,
      .ToolCalls = true,
      .Vision = true,
      .Usage = true,
      .Cancellation = true,
      .OutputLimit = true,
  };
  return Profile;
}

CoreActionIdentifiers MakeActionIdentifiers() {
  return CoreActionIdentifiers{
      .ActionId = ActionId{"action-vertical-synthetic"},
      .OperationId = OperationId{"operation-vertical-synthetic"},
      .IdempotencyKey = IdempotencyKey{"idempotency-vertical-synthetic"},
  };
}

ChannelReceipt MakeSuccessReceipt(const CoreActionIdentifiers& Identifiers) {
  return ChannelReceipt{
      .Action = Identifiers.ActionId,
      .Operation = Identifiers.OperationId,
      .PlatformReceipt =
          PlatformReceiptId{"receipt-vertical-synthetic"},
      .OccurredAt = Timestamp{},
      .Status = ReceiptStatus::Succeeded,
      .ErrorCode = ChannelErrorCode::None,
      .RetryAfter = std::nullopt,
  };
}

MockChannelConfiguration MakeMockConfiguration(
    const AccountProfile& Profile,
    const CoreActionIdentifiers& Identifiers,
    ProviderScenario Scenario) {
  static_cast<void>(Scenario);
  return MockChannelConfiguration{
      .Profile = Profile,
      .Capabilities = MakeChannelCapabilities(Profile),
      .InboundEvents = {MakeInboundMessage()},
      .SendActionResults = {MockActionReceipt{
          .Action = Identifiers.ActionId,
          .Operation = Identifiers.OperationId,
          .Receipt = MakeSuccessReceipt(Identifiers),
      }},
  };
}

class Fixture final {
public:
  explicit Fixture(ProviderScenario Scenario)
      : ChannelProfile(MakeChannelProfile()),
        Identifiers(MakeActionIdentifiers()),
        Channel(MakeMockConfiguration(ChannelProfile, Identifiers, Scenario)),
        Provider(Scenario),
        ProviderProfile(MakeProviderProfile(
            Scenario == ProviderScenario::AnthropicPartialRefusal
                ? EmulativeDialog::Providers::ProviderProtocol::AnthropicMessages
                : EmulativeDialog::Providers::ProviderProtocol::OpenAIResponses)),
        ActionIdSource(Identifiers),
        Policy(21, {IdentityBinding{
                       .Account = ChannelProfile,
                       .Sender = SenderId{"sender-vertical-synthetic"},
                       .Principal = PrincipalId{"principal-vertical-synthetic"},
                   }}),
        Service(Policy, Runtime, Provider, ProviderProfile,
                ProviderCredential, Channel, ChannelCredential,
                ActionIdSource) {
    Check(Channel.Start(ChannelProfile, ChannelCredential).Status ==
              StartStatus::Started,
          "synthetic Mock should start before receiving an event");
    Check(Channel.SubscribeEvents(Sink).Status ==
              SubscriptionStatus::Subscribed,
          "synthetic Mock should publish its trusted inbound envelope");
  }

  CoreChatResult Process() {
    return Service.Process(Sink.Events.at(0));
  }

  AccountProfile ChannelProfile;
  CoreActionIdentifiers Identifiers;
  MockChannelAdapter Channel;
  RequestBoundFakeProvider Provider;
  EmulativeDialog::Providers::ModelProviderProfile ProviderProfile;
  TestProviderCredential ProviderCredential;
  TestChannelCredential ChannelCredential;
  EmulativeDialog::Agent::AgentRuntime Runtime;
  ScriptedActionIdSource ActionIdSource;
  IdentityPolicy Policy;
  CoreChatService Service;
  CollectingSink Sink;
};

void TestSuccessfulMockToMockVerticalSlice() {
  Fixture TestFixture(ProviderScenario::FinalAnswer);
  const CoreChatResult Result = TestFixture.Process();
  Check(Result.Status == CoreChatStatus::Completed &&
            Result.Completion ==
                EmulativeDialog::Providers::CompletionKind::FinalAnswer,
        "final FakeProvider answer should complete only after Core receives a receipt");
  Check(TestFixture.Provider.GetCapturedRequests().size() == 1,
        "vertical slice should create exactly one FakeProvider request");
  Check(TestFixture.Channel.GetSendActionCallCount() == 1 &&
            TestFixture.ActionIdSource.Calls == 1,
        "Core should obtain one authorized action ID tuple and send once");
  const auto& MaybeReceipt = TestFixture.Channel.GetLastSendActionReceipt();
  Check(MaybeReceipt.has_value(),
        "vertical send must return exactly one MockChannel receipt");
  const auto& Receipt = *MaybeReceipt;
  Check(Receipt.Status == ReceiptStatus::Succeeded &&
            Receipt.Action.Value == TestFixture.Identifiers.ActionId.Value &&
            Receipt.Operation.Value == TestFixture.Identifiers.OperationId.Value,
        "Mock receipt must correlate to the exact Core action and operation");
}

void TestRefusalCanCompleteOnlyWithFinalNonEmptyText() {
  Fixture TestFixture(ProviderScenario::Refusal);
  const auto Result = TestFixture.Process();
  Check(Result.Status == CoreChatStatus::Completed &&
            Result.Completion ==
                EmulativeDialog::Providers::CompletionKind::Refusal,
        "completed refusal text may be sent only through Core policy and receipt");
  Check(TestFixture.Channel.GetSendActionCallCount() == 1,
        "completed non-empty refusal should dispatch one authorized action");
}

void TestEveryNonFinalOutcomeHasNoActionOrChannelSend() {
  const std::array Scenarios{
      ProviderScenario::ToolHandoff,
      ProviderScenario::EmptyFinal,
      ProviderScenario::OutputLimited,
      ProviderScenario::Failure,
      ProviderScenario::Cancelled,
      ProviderScenario::UnknownOutcome,
      ProviderScenario::InvalidSequence,
      ProviderScenario::AnthropicPartialRefusal,
  };
  for (const ProviderScenario Scenario : Scenarios) {
    Fixture TestFixture(Scenario);
    const CoreChatResult Result = TestFixture.Process();
    const bool NoActionOrSend =
        TestFixture.Channel.GetSendActionCallCount() == 0 &&
        TestFixture.ActionIdSource.Calls == 0;
    if (!NoActionOrSend) {
      std::cerr << "unexpected action/send for scenario " << ScenarioName(Scenario)
                << ", CoreStatus=" << static_cast<int>(Result.Status)
                << ", sends=" << TestFixture.Channel.GetSendActionCallCount()
                << ", idCalls=" << TestFixture.ActionIdSource.Calls << '\n';
    }
    Check(NoActionOrSend,
          "non-final or invalid AgentRuntime result must not create/send an action");
    Check(TestFixture.Provider.GetCapturedRequests().size() == 1,
          "terminal validation scenarios should issue a single provider request");
    if (Scenario == ProviderScenario::OutputLimited) {
      Check(Result.Status == CoreChatStatus::Failed &&
                Result.AgentFailure ==
                    EmulativeDialog::Core::AgentRunFailureCode::OutputLimitReached,
            "output limit must stay typed and non-sendable");
    }
    if (Scenario == ProviderScenario::UnknownOutcome) {
      Check(Result.Status == CoreChatStatus::UnknownOutcome,
            "provider unknown outcome must remain distinct");
    }
    if (Scenario == ProviderScenario::Cancelled) {
      Check(Result.Status == CoreChatStatus::Cancelled,
            "provider cancellation must remain distinct");
    }
    if (Scenario == ProviderScenario::ToolHandoff) {
      Check(Result.Status == CoreChatStatus::Unsupported,
            "unsupported ToolHandoff must fail closed");
    }
  }
}

void TestSupportedCapabilitiesAllowVerticalSlice() {
  // Direct AgentRuntime contract tests cover the pre-StreamTurn Vision and
  // OutputLimit capability gates. The vertical channel still remains untouched.
  Fixture TestFixture(ProviderScenario::FinalAnswer);
  const auto Result = TestFixture.Process();
  Check(Result.Status == CoreChatStatus::Completed &&
            TestFixture.Channel.GetSendActionCallCount() == 1,
        "supported synthetic capabilities allow the complete vertical path");
}

} // namespace

int main() {
  TestSuccessfulMockToMockVerticalSlice();
  TestRefusalCanCompleteOnlyWithFinalNonEmptyText();
  TestEveryNonFinalOutcomeHasNoActionOrChannelSend();
  TestSupportedCapabilitiesAllowVerticalSlice();
  return 0;
}
