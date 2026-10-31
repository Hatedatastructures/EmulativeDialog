#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <vector>

#include "Source/Channels/Contracts/ChannelAdapterPort.h"
#include "Source/Core/AgentRuntimePort.h"
#include "Source/Core/AsyncAgentToolRuntimePort.hpp"
#include "Source/Core/BusinessActionRegistry.hpp"
#include "Source/Core/CredentialLease.h"
#include "Source/Core/IdentityPolicy.h"
#include "Source/Core/OperationRecord.h"
#include "Source/Providers/Contracts/ModelProviderPort.h"
#include "Source/Providers/Contracts/AsyncModelProviderPort.hpp"

#include <boost/asio/awaitable.hpp>

namespace EmulativeDialog::Core {

struct CoreActionIdentifiers final {
  Channels::ActionId ActionId;
  Channels::OperationId OperationId;
  Channels::IdempotencyKey IdempotencyKey;
};

class CoreActionIdSource {
public:
  virtual ~CoreActionIdSource() = default;
  // Different messages may call Next concurrently. Implementations must be
  // thread-safe and return globally unique identifiers for all three fields.
  [[nodiscard]] virtual CoreActionIdentifiers Next() = 0;
};

enum class CoreChatStatus : std::uint8_t {
  Completed,
  Rejected,
  Duplicate,
  Unsupported,
  Failed,
  Cancelled,
  UnknownOutcome,
};

enum class CoreChatFailureCode : std::uint8_t {
  None,
  InvalidEnvelope,
  UnknownAccount,
  UnknownSender,
  AccountRouteMismatch,
  InvalidMessage,
  UnsupportedInput,
  CapabilityUnavailable,
  PolicyVersionChanged,
  CapabilityVersionChanged,
  InvalidAgentResult,
  InvalidActionIdentifiers,
  OperationTransitionFailure,
  ChannelException,
};

struct CoreChatResult final {
  CoreChatStatus Status{CoreChatStatus::Failed};
  CoreChatFailureCode Failure{CoreChatFailureCode::None};
  std::optional<AgentRunFailureCode> AgentFailure;
  std::optional<Providers::CompletionKind> Completion;
  std::optional<OperationState> Operation;
};

class CoreChatService final {
public:
  CoreChatService(const IdentityPolicy& Policy,
                  AgentRuntimePort& AgentRuntime,
                  Providers::ModelProviderPort& Provider,
                  const Providers::ModelProviderProfile& ProviderProfile,
                  const Providers::CredentialLease& ProviderLease,
                  Channels::ChannelAdapterPort& Channel,
                  const CredentialLease& ChannelLease,
                  CoreActionIdSource& ActionIdSource,
                  OperationRecordObserver* RecordObserver = nullptr);

  CoreChatService(const IdentityPolicy& Policy,
                  AgentRuntimePort& AgentRuntime,
                  Providers::ModelProviderPort& Provider,
                  const Providers::ModelProviderProfile& ProviderProfile,
                  const Providers::CredentialLease& ProviderLease,
                  Channels::ChannelAdapterPort& Channel,
                  const CredentialLease& ChannelLease,
                  CoreActionIdSource& ActionIdSource,
                  const BusinessActionRegistry& BusinessActions,
                  OperationRecordObserver* RecordObserver = nullptr);

  [[nodiscard]] CoreChatResult Process(
      const Channels::InboundChannelMessage& Message,
      std::stop_token CancellationToken = {});

  [[nodiscard]] boost::asio::awaitable<CoreChatResult> ProcessAsync(
      Channels::InboundChannelMessage Message,
      AsyncAgentRuntimePort& AsyncAgentRuntime,
      Providers::AsyncModelProviderPort& AsyncProvider,
      std::stop_token CancellationToken = {});

  [[nodiscard]] boost::asio::awaitable<CoreChatResult> ProcessAsync(
      Channels::InboundChannelMessage Message,
      AsyncAgentToolRuntimePort& AsyncAgentRuntime,
      Providers::AsyncModelProviderPort& AsyncProvider,
      std::stop_token CancellationToken = {});

private:
  [[nodiscard]] std::optional<CoreChatResult> PrepareTurn(
      const Channels::InboundChannelMessage& Message,
      std::stop_token CancellationToken,
      std::optional<ResolvedIdentity>& Identity,
      std::optional<AuthorizedTurnContext>& Context);

  [[nodiscard]] CoreChatResult CompleteTurn(
      const ResolvedIdentity& Identity,
      const AuthorizedTurnContext& Context,
      AgentRunResult AgentResult,
      std::stop_token CancellationToken);

  const IdentityPolicy& Policy;
  AgentRuntimePort& AgentRuntime;
  Providers::ModelProviderPort& Provider;
  const Providers::ModelProviderProfile& ProviderProfile;
  const Providers::CredentialLease& ProviderLease;
  Channels::ChannelAdapterPort& Channel;
  const CredentialLease& ChannelLease;
  CoreActionIdSource& ActionIdSource;
  OperationRecordObserver* RecordObserver{};
  const BusinessActionRegistry* BusinessActions{};
  std::mutex StateMutex;
  std::unordered_set<std::string> ProcessedDeduplicationIds;
  std::unordered_set<std::string> IssuedActionIds;
  std::unordered_set<std::string> IssuedOperationIds;
  std::unordered_set<std::string> IssuedIdempotencyKeys;
  std::vector<OperationRecord> OperationRecords;
};

} // namespace EmulativeDialog::Core
