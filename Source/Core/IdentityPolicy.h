#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "Source/Channels/Contracts/ChannelAdapterPort.h"

namespace EmulativeDialog::Core {

struct PrincipalId final {
  std::string Value;
};

struct ConversationScope final {
  PrincipalId Principal;
  Channels::ChannelId Channel;
  Channels::AccountId Account;
  Channels::IntegrationMode Mode;
  Channels::ConversationId Conversation;
  Channels::ConversationKind ConversationType{Channels::ConversationKind::Private};
  Channels::SenderId Sender;
};

struct IdentityBinding final {
  Channels::AccountProfile Account;
  Channels::SenderId Sender;
  PrincipalId Principal;
  // Capability grants are trusted Core configuration, never envelope or
  // provider data. They are copied into each resolved turn context.
  std::vector<std::string> GrantedCapabilities{};
};

enum class IdentityPolicyFailureCode : std::uint8_t {
  InvalidEnvelope,
  UnknownAccount,
  UnknownSender,
  AccountRouteMismatch,
};

struct IdentityPolicyFailure final {
  IdentityPolicyFailureCode Code{IdentityPolicyFailureCode::InvalidEnvelope};
};

struct ResolvedIdentity final {
  PrincipalId Principal;
  ConversationScope Scope;
  Channels::AccountProfile Account;
  std::vector<std::string> GrantedCapabilities;
};

using IdentityResolution = std::variant<ResolvedIdentity, IdentityPolicyFailure>;

class IdentityPolicy final {
public:
  IdentityPolicy(std::uint64_t PolicyVersion, std::vector<IdentityBinding> Bindings);

  [[nodiscard]] IdentityResolution Resolve(
      const Channels::ChannelEnvelope& Envelope) const;
  [[nodiscard]] std::uint64_t GetPolicyVersion() const noexcept;

private:
  std::uint64_t PolicyVersion{};
  std::vector<IdentityBinding> Bindings;
};

} // namespace EmulativeDialog::Core
