#include "Source/Core/CoreChatService.h"

#include "Source/Agent/OutputBudgetController.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Source/Core/CoreAuthorizedAction.h"

namespace EmulativeDialog::Core {
namespace {

class TurnBudgetCleanup final {
public:
  TurnBudgetCleanup(Agent::OutputBudgetController& ControllerValue,
                    const Providers::AgentTurnId& TurnValue) noexcept
      : Controller(ControllerValue), Turn(TurnValue) {}
  ~TurnBudgetCleanup() { Controller.EndTurn(Turn); }

  TurnBudgetCleanup(const TurnBudgetCleanup&) = delete;
  TurnBudgetCleanup& operator=(const TurnBudgetCleanup&) = delete;

private:
  Agent::OutputBudgetController& Controller;
  const Providers::AgentTurnId& Turn;
};

bool IsNonEmpty(std::string_view Value) noexcept {
  return !Value.empty();
}

void AppendLengthPrefixed(std::string& Output, std::string_view Value) {
  Output.append(std::to_string(Value.size()));
  Output.push_back(':');
  Output.append(Value);
}

std::string ScopedDeduplicationKey(const Channels::ChannelEnvelope& Envelope) {
  std::string Key;
  AppendLengthPrefixed(Key, Envelope.Channel.Value);
  AppendLengthPrefixed(Key, Envelope.Account.Value);
  AppendLengthPrefixed(Key, Envelope.StableDeduplication.Value);
  return Key;
}

Providers::AgentTurnId MakeAgentTurnId(const Channels::ChannelEnvelope& Envelope) {
  std::string Value{"CoreTurn:"};
  AppendLengthPrefixed(Value, Envelope.Channel.Value);
  AppendLengthPrefixed(Value, Envelope.Account.Value);
  AppendLengthPrefixed(Value, Envelope.StableDeduplication.Value);
  return Providers::AgentTurnId{std::move(Value)};
}

bool HasNotExpired(const std::optional<Channels::Timestamp>& ValidUntil,
                   Channels::Timestamp Now) noexcept {
  return !ValidUntil.has_value() || Now <= *ValidUntil;
}

bool SnapshotMatchesAccount(const Channels::CapabilitySnapshot& Snapshot,
                            const Channels::AccountProfile& Account,
                            Channels::Timestamp Now) noexcept {
  return Snapshot.Channel.Value == Account.Channel.Value &&
         Snapshot.Account.Value == Account.Account.Value &&
         Snapshot.Mode.Value == Account.Mode.Value && Snapshot.SnapshotVersion != 0 &&
         Snapshot.AuthenticationStatus == Channels::VerificationStatus::Verified &&
         Snapshot.AuditStatus == Channels::VerificationStatus::Verified &&
         HasNotExpired(Snapshot.ValidUntil, Now);
}

bool HasVerifiedCapability(const Channels::CapabilitySnapshot& Snapshot,
                           Channels::ChannelCapability Capability,
                           Channels::Timestamp Now) noexcept {
  const Channels::CapabilityObservation* Found = nullptr;
  for (const Channels::CapabilityObservation& Observation : Snapshot.Capabilities) {
    if (Observation.Capability != Capability) {
      continue;
    }
    if (Found != nullptr) {
      return false;
    }
    Found = &Observation;
  }
  return Found != nullptr && Found->Status == Channels::CapabilityStatus::Supported &&
         Found->EvidenceStatus == Channels::VerificationStatus::Verified &&
         HasNotExpired(Found->ValidUntil, Now);
}

enum class TextExtractionStatus : std::uint8_t {
  Success,
  Empty,
  UnsupportedPart,
};

struct TextExtraction final {
  TextExtractionStatus Status{TextExtractionStatus::Empty};
  std::string Text;
};

TextExtraction ExtractInboundText(const Channels::CanonicalMessage& Message) {
  TextExtraction Result;
  for (const Channels::MessagePart& Part : Message.MessageParts) {
    const auto* Text = std::get_if<Channels::TextPart>(&Part);
    if (Text == nullptr) {
      return TextExtraction{TextExtractionStatus::UnsupportedPart, {}};
    }
    Result.Text += Text->Utf8Text;
  }
  if (Result.Text.empty()) {
    return TextExtraction{TextExtractionStatus::Empty, {}};
  }
  Result.Status = TextExtractionStatus::Success;
  return Result;
}

bool IsValidCompletionKind(Providers::CompletionKind Completion) noexcept {
  return Completion == Providers::CompletionKind::FinalAnswer ||
         Completion == Providers::CompletionKind::Refusal;
}

bool SameScope(const ConversationScope& Left,
               const ConversationScope& Right) noexcept {
  return Left.Principal.Value == Right.Principal.Value &&
         Left.Channel.Value == Right.Channel.Value &&
         Left.Account.Value == Right.Account.Value &&
         Left.Mode.Value == Right.Mode.Value &&
         Left.Conversation.Value == Right.Conversation.Value &&
         Left.ConversationType == Right.ConversationType &&
         Left.Sender.Value == Right.Sender.Value;
}

bool SameAuthorizedContext(const AuthorizedTurnContext& Left,
                           const AuthorizedTurnContext& Right) noexcept {
  return Left.Principal.Value == Right.Principal.Value &&
         SameScope(Left.Scope, Right.Scope) &&
         Left.AgentTurn.Value == Right.AgentTurn.Value &&
         Left.GrantedCapabilities == Right.GrantedCapabilities;
}

bool HasCapability(const std::vector<std::string>& Granted,
                   std::string_view Required) noexcept {
  return std::find(Granted.begin(), Granted.end(), Required) != Granted.end();
}

bool ScopeMatches(const BusinessActionScopeRequirement& Requirement,
                  const AuthorizedTurnContext& Context) noexcept {
  return (!Requirement.ChannelId.has_value() ||
          Context.Scope.Channel.Value == *Requirement.ChannelId) &&
         (!Requirement.ConversationId.has_value() ||
          Context.Scope.Conversation.Value == *Requirement.ConversationId);
}

bool IsSafeResultCode(std::string_view Value) noexcept {
  return !Value.empty() && Value.size() <= 64U &&
         std::all_of(Value.begin(), Value.end(), [](char Character) {
           const auto Byte = static_cast<unsigned char>(Character);
           return (Byte >= 'A' && Byte <= 'Z') ||
                  (Byte >= 'a' && Byte <= 'z') ||
                  (Byte >= '0' && Byte <= '9') || Character == '_' ||
                  Character == '-';
         });
}

std::string DefiniteActionCode(const BusinessActionExecutionResult& Result) {
  if (IsSafeResultCode(Result.SanitizedCode)) {
    return Result.SanitizedCode;
  }
  return Result.Status == BusinessActionExecutionStatus::Succeeded
             ? "Succeeded"
             : "ActionFailed";
}

class CoreOwnedAsyncBusinessActionDispatcher final
    : public AsyncBusinessActionDispatcherPort {
public:
  CoreOwnedAsyncBusinessActionDispatcher(
      const BusinessActionRegistry& RegistryValue,
      AuthorizedTurnContext ContextValue)
      : Registry(RegistryValue), Context(std::move(ContextValue)) {}

  std::vector<Providers::ToolDefinition> GetToolDefinitions(
      const AuthorizedTurnContext& Candidate) const override {
    std::vector<Providers::ToolDefinition> Tools;
    if (!SameAuthorizedContext(Context, Candidate)) {
      return Tools;
    }
    for (const BusinessActionDefinition* Definition :
         Registry.GetAsyncDefinitions()) {
      if (Definition == nullptr || !Definition->AsyncHandler ||
          Definition->EffectClass !=
              BusinessActionEffectClass::InProcessReadOnly ||
          !ScopeMatches(Definition->Scope, Context)) {
        continue;
      }
      const bool Authorized = std::all_of(
          Definition->RequiredCapabilities.begin(),
          Definition->RequiredCapabilities.end(),
          [this](const std::string& Capability) {
            return HasCapability(Context.GrantedCapabilities, Capability);
          });
      if (Authorized) {
        Tools.push_back(Definition->ModelTool);
      }
    }
    return Tools;
  }

  boost::asio::awaitable<std::vector<CoreActionReceipt>> DispatchAsync(
      const AuthorizedTurnContext& Candidate,
      const Providers::RequestId& Request,
      std::vector<Providers::FunctionCall> Calls,
      std::stop_token CancellationToken) override {
    if (!SameAuthorizedContext(Context, Candidate) ||
        Request.Value.empty() || Context.AgentTurn.Value.empty()) {
      throw std::logic_error("Async action context is not Core-authorized");
    }
    std::vector<CoreActionReceipt> Receipts;
    Receipts.reserve(Calls.size());
    for (const Providers::FunctionCall& Call : Calls) {
      if (CancellationToken.stop_requested()) {
        co_return Receipts;
      }
      const BusinessActionDefinition* Definition = Registry.Find(Call.Name);
      const std::string ReceiptName =
          Definition == nullptr ? "UnknownAction" : Definition->Name;
      auto Emit = [&Receipts, &Call, &Request, this, &ReceiptName](
                     CoreActionReceiptStatus Status,
                     std::string Code) {
        Receipts.push_back(CoreActionReceipt{
            .ToolCall = Call.Id,
            .Request = Request,
            .Turn = Context.AgentTurn,
            .ActionName = ReceiptName,
            .Status = Status,
            .SanitizedCode = std::move(Code),
        });
      };

      if (Call.Id.Value.empty() ||
          !SeenToolCallIds.emplace(Call.Id.Value).second) {
        Emit(CoreActionReceiptStatus::Rejected, "ReplayRejected");
        continue;
      }
      if (Definition == nullptr || !Definition->AsyncHandler) {
        Emit(CoreActionReceiptStatus::Rejected, "UnknownAction");
        continue;
      }
      if (Definition->EffectClass !=
          BusinessActionEffectClass::InProcessReadOnly) {
        Emit(CoreActionReceiptStatus::Rejected, "ActionIneligible");
        continue;
      }
      if (!BusinessActionRegistry::ValidateArguments(
              Call.Arguments, Definition->ModelTool)) {
        Emit(CoreActionReceiptStatus::Rejected, "InvalidArguments");
        continue;
      }
      if (!ScopeMatches(Definition->Scope, Context)) {
        Emit(CoreActionReceiptStatus::Rejected, "ScopeMismatch");
        continue;
      }
      const bool Authorized = std::all_of(
          Definition->RequiredCapabilities.begin(),
          Definition->RequiredCapabilities.end(),
          [this](const std::string& Capability) {
            return HasCapability(Context.GrantedCapabilities, Capability);
          });
      if (!Authorized) {
        Emit(CoreActionReceiptStatus::Rejected, "MissingCapability");
        continue;
      }

      const BusinessActionInvocation Invocation{
          .Name = Definition->Name,
          .ToolCallId = Call.Id.Value,
          .Arguments = Call.Arguments,
          .Principal = Context.Principal,
          .Scope = Context.Scope,
          .Turn = Context.AgentTurn,
      };
      BusinessActionExecutionResult Result;
      try {
        Result = co_await Definition->AsyncHandler(Invocation,
                                                   CancellationToken);
      } catch (...) {
        if (CancellationToken.stop_requested()) {
          co_return Receipts;
        }
        Emit(CoreActionReceiptStatus::Failed, "HandlerFailed");
        continue;
      }
      if (CancellationToken.stop_requested()) {
        co_return Receipts;
      }
      if (Result.Status == BusinessActionExecutionStatus::UnknownOutcome) {
        // This status is forbidden for a read-only in-process handler. Stop
        // the turn rather than inventing a receipt or continuing the model.
        throw std::logic_error("Read-only action returned an unknown outcome");
      }
      if (Result.Status == BusinessActionExecutionStatus::Succeeded) {
        Emit(CoreActionReceiptStatus::Succeeded, DefiniteActionCode(Result));
      } else {
        Emit(CoreActionReceiptStatus::Failed, DefiniteActionCode(Result));
      }
    }
    co_return Receipts;
  }

private:
  const BusinessActionRegistry& Registry;
  const AuthorizedTurnContext Context;
  std::unordered_set<std::string> SeenToolCallIds;
};

bool IsValidIdentifiers(const CoreActionIdentifiers& Identifiers) noexcept {
  if (!IsNonEmpty(Identifiers.ActionId.Value) ||
      !IsNonEmpty(Identifiers.OperationId.Value) ||
      !IsNonEmpty(Identifiers.IdempotencyKey.Value)) {
    return false;
  }
  return Identifiers.ActionId.Value != Identifiers.OperationId.Value &&
         Identifiers.ActionId.Value != Identifiers.IdempotencyKey.Value &&
         Identifiers.OperationId.Value != Identifiers.IdempotencyKey.Value;
}

} // namespace

AgentRunResult::AgentRunResult(
    AgentRunStatus Status,
    std::optional<Providers::CompletionKind> Completion,
    std::string FinalText,
    std::optional<AgentRunFailureCode> Failure)
    : Status(Status), Completion(Completion), FinalText(std::move(FinalText)),
      Failure(Failure) {}

AgentRunResult AgentRunResult::ResponseReady(
    Providers::CompletionKind Completion, std::string FinalText) {
  return AgentRunResult{AgentRunStatus::ResponseReady, Completion,
                        std::move(FinalText), std::nullopt};
}

AgentRunResult AgentRunResult::Failed(AgentRunFailureCode Failure) {
  return AgentRunResult{AgentRunStatus::Failed, std::nullopt, {}, Failure};
}

AgentRunResult AgentRunResult::Cancelled() {
  return AgentRunResult{AgentRunStatus::Cancelled, std::nullopt, {}, std::nullopt};
}

AgentRunResult AgentRunResult::Unsupported() {
  return AgentRunResult{AgentRunStatus::Unsupported, std::nullopt, {}, std::nullopt};
}

AgentRunResult AgentRunResult::UnknownOutcome() {
  return AgentRunResult{AgentRunStatus::UnknownOutcome, std::nullopt, {}, std::nullopt};
}

AgentRunStatus AgentRunResult::GetStatus() const noexcept {
  return Status;
}

std::optional<Providers::CompletionKind>
AgentRunResult::GetCompletionKind() const noexcept {
  return Completion;
}

std::string_view AgentRunResult::GetFinalText() const noexcept {
  return FinalText;
}

std::optional<AgentRunFailureCode> AgentRunResult::GetFailureCode() const noexcept {
  return Failure;
}

CoreAuthorizedAction::CoreAuthorizedAction(
    Channels::ActionId ActionId, Channels::OperationId OperationId,
    Channels::IdempotencyKey IdempotencyKey, ConversationScope TargetScope,
    std::string TextPayload, AuthorizationRef Authorization,
    std::uint64_t PolicyVersion, std::uint64_t CapabilitySnapshotVersion)
    : ActionId(std::move(ActionId)), OperationId(std::move(OperationId)),
      IdempotencyKey(std::move(IdempotencyKey)),
      TargetScope(std::move(TargetScope)), TextPayload(std::move(TextPayload)),
      Authorization(std::move(Authorization)), PolicyVersion(PolicyVersion),
      CapabilitySnapshotVersion(CapabilitySnapshotVersion) {}

const Channels::ActionId& CoreAuthorizedAction::GetActionId() const noexcept {
  return ActionId;
}

const Channels::OperationId& CoreAuthorizedAction::GetOperationId() const noexcept {
  return OperationId;
}

const Channels::IdempotencyKey& CoreAuthorizedAction::GetIdempotencyKey() const noexcept {
  return IdempotencyKey;
}

const ConversationScope& CoreAuthorizedAction::GetTargetScope() const noexcept {
  return TargetScope;
}

CoreAuthorizedActionKind CoreAuthorizedAction::GetActionKind() const noexcept {
  return ActionKind;
}

std::string_view CoreAuthorizedAction::GetTextPayload() const noexcept {
  return TextPayload;
}

const AuthorizationRef& CoreAuthorizedAction::GetAuthorizationRef() const noexcept {
  return Authorization;
}

std::uint64_t CoreAuthorizedAction::GetPolicyVersion() const noexcept {
  return PolicyVersion;
}

std::uint64_t CoreAuthorizedAction::GetCapabilitySnapshotVersion() const noexcept {
  return CapabilitySnapshotVersion;
}

CoreChatService::CoreChatService(
    const IdentityPolicy& Policy, AgentRuntimePort& AgentRuntime,
    Providers::ModelProviderPort& Provider,
    const Providers::ModelProviderProfile& ProviderProfile,
    const Providers::CredentialLease& ProviderLease,
    Channels::ChannelAdapterPort& Channel, const CredentialLease& ChannelLease,
    CoreActionIdSource& ActionIdSource, OperationRecordObserver* RecordObserver)
    : Policy(Policy), AgentRuntime(AgentRuntime), Provider(Provider),
      ProviderProfile(ProviderProfile), ProviderLease(ProviderLease), Channel(Channel),
      ChannelLease(ChannelLease), ActionIdSource(ActionIdSource),
      RecordObserver(RecordObserver) {}

CoreChatService::CoreChatService(
    const IdentityPolicy& Policy, AgentRuntimePort& AgentRuntime,
    Providers::ModelProviderPort& Provider,
    const Providers::ModelProviderProfile& ProviderProfile,
    const Providers::CredentialLease& ProviderLease,
    Channels::ChannelAdapterPort& Channel, const CredentialLease& ChannelLease,
    CoreActionIdSource& ActionIdSource,
    const BusinessActionRegistry& BusinessActions,
    OperationRecordObserver* RecordObserver)
    : Policy(Policy), AgentRuntime(AgentRuntime), Provider(Provider),
      ProviderProfile(ProviderProfile), ProviderLease(ProviderLease),
      Channel(Channel), ChannelLease(ChannelLease),
      ActionIdSource(ActionIdSource), RecordObserver(RecordObserver),
      BusinessActions(&BusinessActions) {}

std::optional<CoreChatResult> CoreChatService::PrepareTurn(
    const Channels::InboundChannelMessage& Message,
    std::stop_token CancellationToken,
    std::optional<ResolvedIdentity>& Identity,
    std::optional<AuthorizedTurnContext>& Context) {
  if (CancellationToken.stop_requested()) {
    return CoreChatResult{CoreChatStatus::Cancelled, CoreChatFailureCode::None,
                          std::nullopt, std::nullopt, std::nullopt};
  }

  auto Resolution = Policy.Resolve(Message.Envelope);
  if (const auto* Failure = std::get_if<IdentityPolicyFailure>(&Resolution)) {
    CoreChatFailureCode FailureCode = CoreChatFailureCode::InvalidEnvelope;
    switch (Failure->Code) {
    case IdentityPolicyFailureCode::InvalidEnvelope:
      FailureCode = CoreChatFailureCode::InvalidEnvelope;
      break;
    case IdentityPolicyFailureCode::UnknownAccount:
      FailureCode = CoreChatFailureCode::UnknownAccount;
      break;
    case IdentityPolicyFailureCode::UnknownSender:
      FailureCode = CoreChatFailureCode::UnknownSender;
      break;
    case IdentityPolicyFailureCode::AccountRouteMismatch:
      FailureCode = CoreChatFailureCode::AccountRouteMismatch;
      break;
    }
    return CoreChatResult{CoreChatStatus::Rejected, FailureCode, std::nullopt,
                          std::nullopt, std::nullopt};
  }

  Identity.emplace(std::get<ResolvedIdentity>(std::move(Resolution)));
  std::string DeduplicationKey;
  try {
    DeduplicationKey = ScopedDeduplicationKey(Message.Envelope);
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidAgentResult,
                          AgentRunFailureCode::InternalError,
                          std::nullopt, std::nullopt};
  }

  {
    std::lock_guard<std::mutex> Lock(StateMutex);
    try {
      if (!ProcessedDeduplicationIds.emplace(std::move(DeduplicationKey)).second) {
        return CoreChatResult{CoreChatStatus::Duplicate,
                              CoreChatFailureCode::None,
                              std::nullopt, std::nullopt, std::nullopt};
      }
    } catch (...) {
      return CoreChatResult{CoreChatStatus::Failed,
                            CoreChatFailureCode::InvalidAgentResult,
                            AgentRunFailureCode::InternalError,
                            std::nullopt, std::nullopt};
    }
  }

  TextExtraction InputText;
  try {
    InputText = ExtractInboundText(Message.Message);
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidAgentResult,
                          AgentRunFailureCode::InternalError,
                          std::nullopt, std::nullopt};
  }
  if (InputText.Status == TextExtractionStatus::UnsupportedPart) {
    return CoreChatResult{CoreChatStatus::Unsupported,
                          CoreChatFailureCode::UnsupportedInput,
                          std::nullopt, std::nullopt, std::nullopt};
  }
  if (InputText.Status == TextExtractionStatus::Empty) {
    return CoreChatResult{CoreChatStatus::Rejected,
                          CoreChatFailureCode::InvalidMessage,
                          std::nullopt, std::nullopt, std::nullopt};
  }

  const Channels::Timestamp InboundCheckTime = std::chrono::system_clock::now();
  Channels::CapabilitySnapshot InboundCapabilities;
  try {
    InboundCapabilities = Channel.GetCapabilities(Identity->Account);
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::CapabilityUnavailable,
                          std::nullopt, std::nullopt, std::nullopt};
  }
  if (!SnapshotMatchesAccount(InboundCapabilities, Identity->Account,
                              InboundCheckTime) ||
      !HasVerifiedCapability(InboundCapabilities,
                             Channels::ChannelCapability::ReceiveText,
                             InboundCheckTime)) {
    return CoreChatResult{CoreChatStatus::Unsupported,
                          CoreChatFailureCode::CapabilityUnavailable,
                          std::nullopt, std::nullopt, std::nullopt};
  }

  try {
    Context.emplace(AuthorizedTurnContext{
        Identity->Principal, Identity->Scope,
        MakeAgentTurnId(Message.Envelope), Message.Message,
        Identity->GrantedCapabilities});
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidAgentResult,
                          AgentRunFailureCode::InternalError,
                          std::nullopt, std::nullopt};
  }

  Agent::OutputBudgetController& Budget = Agent::OutputBudgetController::Shared();
  try {
    if (!Budget.BeginTurn(Context->AgentTurn,
                          ProviderProfile.Limit.MaximumOutputTokens)) {
      return CoreChatResult{CoreChatStatus::Failed,
                            CoreChatFailureCode::InvalidAgentResult,
                            AgentRunFailureCode::InternalError,
                            std::nullopt, std::nullopt};
    }
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidAgentResult,
                          AgentRunFailureCode::InternalError,
                          std::nullopt, std::nullopt};
  }
  return std::nullopt;
}

CoreChatResult CoreChatService::CompleteTurn(
    const ResolvedIdentity& Identity,
    const AuthorizedTurnContext& Context,
    AgentRunResult AgentResult,
    std::stop_token CancellationToken) {
  if (AgentResult.GetStatus() == AgentRunStatus::UnknownOutcome) {
    return CoreChatResult{CoreChatStatus::UnknownOutcome,
                          CoreChatFailureCode::None,
                          std::nullopt, std::nullopt, std::nullopt};
  }
  if (CancellationToken.stop_requested()) {
    return CoreChatResult{CoreChatStatus::Cancelled, CoreChatFailureCode::None,
                          std::nullopt, std::nullopt, std::nullopt};
  }

  switch (AgentResult.GetStatus()) {
  case AgentRunStatus::Failed:
    if (!AgentResult.GetFailureCode().has_value()) {
      return CoreChatResult{CoreChatStatus::Failed,
                            CoreChatFailureCode::InvalidAgentResult,
                            std::nullopt, std::nullopt, std::nullopt};
    }
    return CoreChatResult{CoreChatStatus::Failed, CoreChatFailureCode::None,
                          AgentResult.GetFailureCode(), std::nullopt,
                          std::nullopt};
  case AgentRunStatus::Cancelled:
    return CoreChatResult{CoreChatStatus::Cancelled, CoreChatFailureCode::None,
                          std::nullopt, std::nullopt, std::nullopt};
  case AgentRunStatus::Unsupported:
    return CoreChatResult{CoreChatStatus::Unsupported,
                          CoreChatFailureCode::None,
                          std::nullopt, std::nullopt, std::nullopt};
  case AgentRunStatus::UnknownOutcome:
    return CoreChatResult{CoreChatStatus::UnknownOutcome,
                          CoreChatFailureCode::None,
                          std::nullopt, std::nullopt, std::nullopt};
  case AgentRunStatus::ResponseReady:
    break;
  }

  const std::optional<Providers::CompletionKind> Completion =
      AgentResult.GetCompletionKind();
  if (!Completion.has_value() || !IsValidCompletionKind(*Completion) ||
      AgentResult.GetFinalText().empty()) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidAgentResult,
                          std::nullopt, std::nullopt, std::nullopt};
  }

  const Channels::Timestamp SendCheckTime = std::chrono::system_clock::now();
  Channels::CapabilitySnapshot SendCapabilities;
  try {
    SendCapabilities = Channel.GetCapabilities(Identity.Account);
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::CapabilityUnavailable,
                          std::nullopt, Completion, std::nullopt};
  }
  if (!SnapshotMatchesAccount(SendCapabilities, Identity.Account,
                              SendCheckTime) ||
      !HasVerifiedCapability(SendCapabilities,
                             Channels::ChannelCapability::SendText,
                             SendCheckTime)) {
    return CoreChatResult{CoreChatStatus::Unsupported,
                          CoreChatFailureCode::CapabilityUnavailable,
                          std::nullopt, Completion, std::nullopt};
  }

  const std::uint64_t CurrentPolicyVersion = Policy.GetPolicyVersion();
  const std::uint64_t CapabilityVersion = SendCapabilities.SnapshotVersion;
  CoreActionIdentifiers Identifiers;
  try {
    // This call is intentionally outside StateMutex. The injected source's
    // contract requires concurrent calls for distinct messages to be safe.
    Identifiers = ActionIdSource.Next();
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidActionIdentifiers,
                          std::nullopt, Completion, std::nullopt};
  }
  if (!IsValidIdentifiers(Identifiers)) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidActionIdentifiers,
                          std::nullopt, Completion, std::nullopt};
  }
  {
    std::lock_guard<std::mutex> Lock(StateMutex);
    if (IssuedActionIds.contains(Identifiers.ActionId.Value) ||
        IssuedOperationIds.contains(Identifiers.OperationId.Value) ||
        IssuedIdempotencyKeys.contains(Identifiers.IdempotencyKey.Value)) {
      return CoreChatResult{CoreChatStatus::Failed,
                            CoreChatFailureCode::InvalidActionIdentifiers,
                            std::nullopt, Completion, std::nullopt};
    }
    try {
      IssuedActionIds.emplace(Identifiers.ActionId.Value);
      IssuedOperationIds.emplace(Identifiers.OperationId.Value);
      IssuedIdempotencyKeys.emplace(Identifiers.IdempotencyKey.Value);
    } catch (...) {
      // Any identifiers inserted before allocation failed stay consumed. This
      // is fail-closed and prevents reuse after an incomplete reservation.
      return CoreChatResult{CoreChatStatus::Failed,
                            CoreChatFailureCode::InvalidActionIdentifiers,
                            std::nullopt, Completion, std::nullopt};
    }
  }

  OperationRecord Record{Identifiers.OperationId, Identifiers.IdempotencyKey,
                         Context.AgentTurn, Identifiers.ActionId, Identity.Scope,
                         CurrentPolicyVersion, CapabilityVersion};
  if (RecordObserver != nullptr) {
    const OperationRecord Snapshot = Record;
    RecordObserver->OnOperationRecordChanged(Snapshot);
  }
  if (!Record.Authorize()) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::OperationTransitionFailure,
                          std::nullopt, Completion, Record.GetState()};
  }
  if (RecordObserver != nullptr) {
    const OperationRecord Snapshot = Record;
    RecordObserver->OnOperationRecordChanged(Snapshot);
  }

  std::string AuthorizationValue{"CoreAuthorization:"};
  AppendLengthPrefixed(AuthorizationValue, Identifiers.ActionId.Value);
  AppendLengthPrefixed(AuthorizationValue, Identifiers.OperationId.Value);
  AppendLengthPrefixed(AuthorizationValue, std::to_string(CurrentPolicyVersion));
  AppendLengthPrefixed(AuthorizationValue, std::to_string(CapabilityVersion));
  AuthorizationRef Authorization{std::move(AuthorizationValue)};
  CoreAuthorizedAction Action{Identifiers.ActionId,
                              Identifiers.OperationId,
                              Identifiers.IdempotencyKey,
                              Identity.Scope,
                              std::string(AgentResult.GetFinalText()),
                              std::move(Authorization),
                              CurrentPolicyVersion,
                              CapabilityVersion};

  const Channels::Timestamp DispatchCheckTime = std::chrono::system_clock::now();
  Channels::CapabilitySnapshot DispatchCapabilities;
  try {
    DispatchCapabilities = Channel.GetCapabilities(Identity.Account);
  } catch (...) {
    static_cast<void>(Record.FailBeforeDispatch(
        Channels::ChannelErrorCode::InvalidProfile));
    if (RecordObserver != nullptr) {
      const OperationRecord Snapshot = Record;
      RecordObserver->OnOperationRecordChanged(Snapshot);
    }
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::CapabilityUnavailable,
                          std::nullopt, Completion, Record.GetState()};
  }
  if (Policy.GetPolicyVersion() != Action.GetPolicyVersion()) {
    static_cast<void>(Record.FailBeforeDispatch(Channels::ChannelErrorCode::Forbidden));
    if (RecordObserver != nullptr) {
      const OperationRecord Snapshot = Record;
      RecordObserver->OnOperationRecordChanged(Snapshot);
    }
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::PolicyVersionChanged,
                          std::nullopt, Completion, Record.GetState()};
  }
  if (!SnapshotMatchesAccount(DispatchCapabilities, Identity.Account,
                              DispatchCheckTime) ||
      DispatchCapabilities.SnapshotVersion !=
          Action.GetCapabilitySnapshotVersion() ||
      !HasVerifiedCapability(DispatchCapabilities,
                             Channels::ChannelCapability::SendText,
                             DispatchCheckTime)) {
    static_cast<void>(Record.FailBeforeDispatch(
        Channels::ChannelErrorCode::UnsupportedCapability));
    if (RecordObserver != nullptr) {
      const OperationRecord Snapshot = Record;
      RecordObserver->OnOperationRecordChanged(Snapshot);
    }
    return CoreChatResult{CoreChatStatus::Unsupported,
                          CoreChatFailureCode::CapabilityVersionChanged,
                          std::nullopt, Completion, Record.GetState()};
  }

  if (!Record.MarkDispatched()) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::OperationTransitionFailure,
                          std::nullopt, Completion, Record.GetState()};
  }
  if (RecordObserver != nullptr) {
    const OperationRecord Snapshot = Record;
    RecordObserver->OnOperationRecordChanged(Snapshot);
  }

  Channels::ChannelReceipt Receipt;
  try {
    Receipt = Channel.SendAction(Action, ChannelLease);
  } catch (...) {
    static_cast<void>(Record.MarkUnknownOutcome(
        Channels::ChannelErrorCode::OutcomeUnknown));
    if (RecordObserver != nullptr) {
      const OperationRecord Snapshot = Record;
      RecordObserver->OnOperationRecordChanged(Snapshot);
    }
    return CoreChatResult{CoreChatStatus::UnknownOutcome,
                          CoreChatFailureCode::ChannelException,
                          std::nullopt, Completion, Record.GetState()};
  }

  const bool ReceiptMatches = Record.ApplyReceipt(Receipt);
  if (RecordObserver != nullptr) {
    const OperationRecord Snapshot = Record;
    RecordObserver->OnOperationRecordChanged(Snapshot);
  }
  if (!ReceiptMatches || Record.GetState() == OperationState::UnknownOutcome) {
    return CoreChatResult{CoreChatStatus::UnknownOutcome,
                          CoreChatFailureCode::None,
                          std::nullopt, Completion, Record.GetState()};
  }

  switch (Record.GetState()) {
  case OperationState::Succeeded:
    return CoreChatResult{CoreChatStatus::Completed,
                          CoreChatFailureCode::None, std::nullopt,
                          Completion, Record.GetState()};
  case OperationState::Rejected:
    return CoreChatResult{CoreChatStatus::Rejected,
                          CoreChatFailureCode::None, std::nullopt,
                          Completion, Record.GetState()};
  case OperationState::Unsupported:
    return CoreChatResult{CoreChatStatus::Unsupported,
                          CoreChatFailureCode::None, std::nullopt,
                          Completion, Record.GetState()};
  case OperationState::Failed:
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::None, std::nullopt,
                          Completion, Record.GetState()};
  case OperationState::Prepared:
  case OperationState::Authorized:
  case OperationState::Dispatched:
  case OperationState::UnknownOutcome:
    break;
  }

  return CoreChatResult{CoreChatStatus::Failed,
                        CoreChatFailureCode::OperationTransitionFailure,
                        std::nullopt, Completion, Record.GetState()};
}

CoreChatResult CoreChatService::Process(
    const Channels::InboundChannelMessage& Message,
    std::stop_token CancellationToken) {
  std::optional<ResolvedIdentity> Identity;
  std::optional<AuthorizedTurnContext> Context;
  if (const std::optional<CoreChatResult> Failure =
          PrepareTurn(Message, CancellationToken, Identity, Context)) {
    return *Failure;
  }

  const TurnBudgetCleanup BudgetCleanup{
      Agent::OutputBudgetController::Shared(), Context->AgentTurn};
  AgentRunResult AgentResult =
      AgentRunResult::Failed(AgentRunFailureCode::InternalError);
  try {
    AgentResult = AgentRuntime.RunTurn(*Context, Provider, ProviderProfile,
                                       ProviderLease, CancellationToken);
  } catch (...) {
    return CoreChatResult{CoreChatStatus::Failed,
                          CoreChatFailureCode::InvalidAgentResult,
                          AgentRunFailureCode::InternalError,
                          std::nullopt, std::nullopt};
  }
  return CompleteTurn(*Identity, *Context, std::move(AgentResult),
                      CancellationToken);
}

boost::asio::awaitable<CoreChatResult> CoreChatService::ProcessAsync(
    Channels::InboundChannelMessage Message,
    AsyncAgentRuntimePort& AsyncAgentRuntime,
    Providers::AsyncModelProviderPort& AsyncProvider,
    std::stop_token CancellationToken) {
  std::optional<ResolvedIdentity> Identity;
  std::optional<AuthorizedTurnContext> Context;
  if (const std::optional<CoreChatResult> Failure =
          PrepareTurn(Message, CancellationToken, Identity, Context)) {
    co_return *Failure;
  }

  const TurnBudgetCleanup BudgetCleanup{
      Agent::OutputBudgetController::Shared(), Context->AgentTurn};
  AgentRunResult AgentResult =
      AgentRunResult::Failed(AgentRunFailureCode::InternalError);
  try {
    AgentResult = co_await AsyncAgentRuntime.RunTurnAsync(
        *Context, AsyncProvider, ProviderProfile, ProviderLease,
        CancellationToken);
  } catch (...) {
    // A thrown coroutine may have dispatched a remote request. Do not turn an
    // unobserved terminal into a definite failure or a replayable result.
    co_return CoreChatResult{CoreChatStatus::UnknownOutcome,
                             CoreChatFailureCode::None,
                             std::nullopt, std::nullopt, std::nullopt};
  }
  co_return CompleteTurn(*Identity, *Context, std::move(AgentResult),
                         CancellationToken);
}

boost::asio::awaitable<CoreChatResult> CoreChatService::ProcessAsync(
    Channels::InboundChannelMessage Message,
    AsyncAgentToolRuntimePort& AsyncAgentRuntime,
    Providers::AsyncModelProviderPort& AsyncProvider,
    std::stop_token CancellationToken) {
  if (BusinessActions == nullptr) {
    co_return co_await ProcessAsync(
        std::move(Message),
        static_cast<AsyncAgentRuntimePort&>(AsyncAgentRuntime),
        AsyncProvider, CancellationToken);
  }

  std::optional<ResolvedIdentity> Identity;
  std::optional<AuthorizedTurnContext> Context;
  if (const std::optional<CoreChatResult> Failure =
          PrepareTurn(Message, CancellationToken, Identity, Context)) {
    co_return *Failure;
  }

  const TurnBudgetCleanup BudgetCleanup{
      Agent::OutputBudgetController::Shared(), Context->AgentTurn};
  CoreOwnedAsyncBusinessActionDispatcher Dispatcher{*BusinessActions, *Context};
  AgentRunResult AgentResult =
      AgentRunResult::Failed(AgentRunFailureCode::InternalError);
  try {
    AgentResult = co_await AsyncAgentRuntime.RunTurnWithToolsAsync(
        *Context, AsyncProvider, ProviderProfile, ProviderLease, Dispatcher,
        CancellationToken);
  } catch (...) {
    co_return CoreChatResult{CoreChatStatus::Failed,
                             CoreChatFailureCode::InvalidAgentResult,
                             AgentRunFailureCode::InternalError,
                             std::nullopt, std::nullopt};
  }
  co_return CompleteTurn(*Identity, *Context, std::move(AgentResult),
                         CancellationToken);
}

} // namespace EmulativeDialog::Core
