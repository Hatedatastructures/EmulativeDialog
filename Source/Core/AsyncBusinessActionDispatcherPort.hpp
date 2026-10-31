#pragma once

#include <stop_token>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "Source/Core/AgentRuntimePort.h"

namespace EmulativeDialog::Core {

enum class CoreActionReceiptStatus : std::uint8_t {
  Succeeded,
  Failed,
  Rejected,
  Unsupported,
};

// Definite Core evidence for an in-process read-only action. It is not a
// ChannelReceipt and cannot represent or authorize an external effect.
struct CoreActionReceipt final {
  Providers::ToolCallId ToolCall;
  Providers::RequestId Request;
  Providers::AgentTurnId Turn;
  std::string ActionName;
  CoreActionReceiptStatus Status{CoreActionReceiptStatus::Rejected};
  std::string SanitizedCode;
};

class AsyncBusinessActionDispatcherPort {
public:
  virtual ~AsyncBusinessActionDispatcherPort() = default;

  [[nodiscard]] virtual std::vector<Providers::ToolDefinition>
  GetToolDefinitions(const AuthorizedTurnContext& Context) const = 0;

  [[nodiscard]] virtual boost::asio::awaitable<
      std::vector<CoreActionReceipt>>
  DispatchAsync(const AuthorizedTurnContext& Context,
                const Providers::RequestId& Request,
                std::vector<Providers::FunctionCall> Calls,
                std::stop_token CancellationToken) = 0;
};

} // namespace EmulativeDialog::Core
