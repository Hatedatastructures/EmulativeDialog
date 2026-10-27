#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "Source/Channels/Contracts/ChannelAdapterPort.h"
#include "Source/Core/IdentityPolicy.h"
#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Core {

struct AuthorizedTurnContext final {
  PrincipalId Principal;
  ConversationScope Scope;
  Providers::AgentTurnId AgentTurn;
  Channels::CanonicalMessage Message;
};

enum class AgentRunStatus : std::uint8_t {
  ResponseReady,
  Failed,
  Cancelled,
  Unsupported,
  UnknownOutcome,
};

enum class AgentRunFailureCode : std::uint8_t {
  InvalidProfile,
  InvalidRequest,
  UnsupportedCapability,
  AuthenticationFailure,
  RateLimited,
  TransportFailure,
  InvalidStream,
  ProviderRejected,
  ContentFiltered,
  OutputLimitReached,
  InternalError,
};

class AgentRunResult final {
public:
  [[nodiscard]] static AgentRunResult ResponseReady(
      Providers::CompletionKind Completion, std::string FinalText);
  [[nodiscard]] static AgentRunResult Failed(AgentRunFailureCode Failure);
  [[nodiscard]] static AgentRunResult Cancelled();
  [[nodiscard]] static AgentRunResult Unsupported();
  [[nodiscard]] static AgentRunResult UnknownOutcome();

  [[nodiscard]] AgentRunStatus GetStatus() const noexcept;
  [[nodiscard]] std::optional<Providers::CompletionKind> GetCompletionKind() const noexcept;
  [[nodiscard]] std::string_view GetFinalText() const noexcept;
  [[nodiscard]] std::optional<AgentRunFailureCode> GetFailureCode() const noexcept;

private:
  AgentRunResult(AgentRunStatus Status,
                 std::optional<Providers::CompletionKind> Completion,
                 std::string FinalText,
                 std::optional<AgentRunFailureCode> Failure);

  AgentRunStatus Status;
  std::optional<Providers::CompletionKind> Completion;
  std::string FinalText;
  std::optional<AgentRunFailureCode> Failure;
};

[[nodiscard]] inline std::optional<AgentRunFailureCode> MapProviderFailureCode(
    Providers::FailureCode Failure) noexcept {
  switch (Failure) {
  case Providers::FailureCode::InvalidProfile:
    return AgentRunFailureCode::InvalidProfile;
  case Providers::FailureCode::InvalidRequest:
    return AgentRunFailureCode::InvalidRequest;
  case Providers::FailureCode::UnsupportedCapability:
    return AgentRunFailureCode::UnsupportedCapability;
  case Providers::FailureCode::AuthenticationFailure:
    return AgentRunFailureCode::AuthenticationFailure;
  case Providers::FailureCode::RateLimited:
    return AgentRunFailureCode::RateLimited;
  case Providers::FailureCode::TransportFailure:
    return AgentRunFailureCode::TransportFailure;
  case Providers::FailureCode::InvalidStream:
    return AgentRunFailureCode::InvalidStream;
  case Providers::FailureCode::ProviderRejected:
    return AgentRunFailureCode::ProviderRejected;
  case Providers::FailureCode::ContentFiltered:
    return AgentRunFailureCode::ContentFiltered;
  }
  return std::nullopt;
}
class AgentRuntimePort {
public:
  virtual ~AgentRuntimePort() = default;

  [[nodiscard]] virtual AgentRunResult RunTurn(
      const AuthorizedTurnContext& Context,
      Providers::ModelProviderPort& Provider,
      const Providers::ModelProviderProfile& Profile,
      const Providers::CredentialLease& Lease,
      std::stop_token CancellationToken) = 0;
};

} // namespace EmulativeDialog::Core
