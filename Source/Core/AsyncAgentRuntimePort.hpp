#pragma once

#include "Source/Core/AgentRuntimePort.h"
#include "Source/Providers/Contracts/AsyncModelProviderPort.hpp"

#include <boost/asio/awaitable.hpp>

namespace EmulativeDialog::Core {

class AsyncAgentRuntimePort {
public:
  virtual ~AsyncAgentRuntimePort() = default;

  [[nodiscard]] virtual boost::asio::awaitable<AgentRunResult>
  RunTurnAsync(const AuthorizedTurnContext& Context,
               Providers::AsyncModelProviderPort& Provider,
               const Providers::ModelProviderProfile& Profile,
               const Providers::CredentialLease& Lease,
               std::stop_token CancellationToken) = 0;
};

} // namespace EmulativeDialog::Core