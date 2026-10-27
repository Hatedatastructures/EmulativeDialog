#pragma once

#include "Source/Core/AgentRuntimePort.h"

#include <atomic>
#include <cstdint>

namespace EmulativeDialog::Agent {

class AgentRuntime final : public Core::AgentRuntimePort {
public:
  AgentRuntime() = default;
  ~AgentRuntime() override = default;

  AgentRuntime(const AgentRuntime&) = delete;
  AgentRuntime& operator=(const AgentRuntime&) = delete;
  AgentRuntime(AgentRuntime&&) = delete;
  AgentRuntime& operator=(AgentRuntime&&) = delete;

  [[nodiscard]] Core::AgentRunResult RunTurn(
      const Core::AuthorizedTurnContext& Context,
      Providers::ModelProviderPort& Provider,
      const Providers::ModelProviderProfile& Profile,
      const Providers::CredentialLease& Lease,
      std::stop_token CancellationToken) override;

private:
  std::atomic<std::uint64_t> NextRequestSequence{1};
};

} // namespace EmulativeDialog::Agent
