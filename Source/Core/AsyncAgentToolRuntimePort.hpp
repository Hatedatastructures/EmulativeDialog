#pragma once

#include "Source/Core/AsyncAgentRuntimePort.hpp"
#include "Source/Core/AsyncBusinessActionDispatcherPort.hpp"

namespace EmulativeDialog::Core {

// Additive opt-in V2 interface. V1 synchronous and single-request async ports
// remain source-compatible and keep their existing behavior.
class AsyncAgentToolRuntimePort : public AsyncAgentRuntimePort {
public:
  ~AsyncAgentToolRuntimePort() override = default;

  [[nodiscard]] virtual boost::asio::awaitable<AgentRunResult>
  RunTurnWithToolsAsync(
      const AuthorizedTurnContext& Context,
      Providers::AsyncModelProviderPort& Provider,
      const Providers::ModelProviderProfile& Profile,
      const Providers::CredentialLease& Lease,
      AsyncBusinessActionDispatcherPort& Dispatcher,
      std::stop_token CancellationToken) = 0;
};

} // namespace EmulativeDialog::Core
