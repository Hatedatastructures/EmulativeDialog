#pragma once

#include "Source/Core/AsyncAgentRuntimePort.hpp"

#include <boost/asio/awaitable.hpp>

namespace EmulativeDialog::Agent {

// The concrete coroutine loop is implemented by the dependent AgentRuntime
// capability task. This declaration fixes its public Core port boundary.
class AsyncAgentRuntime final : public Core::AsyncAgentRuntimePort {
public:
  ~AsyncAgentRuntime() override = default;

  [[nodiscard]] boost::asio::awaitable<Core::AgentRunResult>
  RunTurnAsync(const Core::AuthorizedTurnContext& Context,
               Providers::AsyncModelProviderPort& Provider,
               const Providers::ModelProviderProfile& Profile,
               const Providers::CredentialLease& Lease,
               std::stop_token CancellationToken) override;
};

} // namespace EmulativeDialog::Agent