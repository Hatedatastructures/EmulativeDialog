#include "Source/Core/IdentityPolicy.h"

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace EmulativeDialog::Core {
namespace {

bool IsNonEmpty(std::string_view Value) noexcept {
  return !Value.empty();
}

bool IsValidConversationType(Channels::ConversationKind Kind) noexcept {
  return Kind == Channels::ConversationKind::Private ||
         Kind == Channels::ConversationKind::Group;
}

bool IsValidEnvelope(const Channels::ChannelEnvelope& Envelope) noexcept {
  return IsNonEmpty(Envelope.Channel.Value) && IsNonEmpty(Envelope.Mode.Value) &&
         IsNonEmpty(Envelope.Account.Value) && IsNonEmpty(Envelope.Conversation.Value) &&
         IsNonEmpty(Envelope.Sender.Value) && IsNonEmpty(Envelope.Message.Value) &&
         IsNonEmpty(Envelope.PlatformEvent.Value) &&
         IsNonEmpty(Envelope.StableDeduplication.Value) &&
         IsValidConversationType(Envelope.ConversationType);
}

bool SameBindingKey(const IdentityBinding& Left,
                    const IdentityBinding& Right) noexcept {
  return Left.Account.Channel.Value == Right.Account.Channel.Value &&
         Left.Account.Account.Value == Right.Account.Account.Value &&
         Left.Account.Mode.Value == Right.Account.Mode.Value &&
         Left.Sender.Value == Right.Sender.Value;
}

} // namespace

IdentityPolicy::IdentityPolicy(std::uint64_t PolicyVersion,
                               std::vector<IdentityBinding> Bindings)
    : PolicyVersion(PolicyVersion), Bindings(std::move(Bindings)) {
  if (this->PolicyVersion == 0) {
    throw std::invalid_argument("Core policy version must be nonzero");
  }

  for (std::size_t Index = 0; Index < this->Bindings.size(); ++Index) {
    const IdentityBinding& Binding = this->Bindings[Index];
    if (!IsNonEmpty(Binding.Account.Channel.Value) ||
        !IsNonEmpty(Binding.Account.Account.Value) ||
        !IsNonEmpty(Binding.Account.Mode.Value) ||
        !IsNonEmpty(Binding.Sender.Value) ||
        !IsNonEmpty(Binding.Principal.Value)) {
      throw std::invalid_argument("Identity bindings require nonempty opaque identifiers");
    }
    std::unordered_set<std::string> Capabilities;
    for (const std::string& Capability : Binding.GrantedCapabilities) {
      if (Capability.empty() || !Capabilities.emplace(Capability).second) {
        throw std::invalid_argument(
            "Identity capability grants must be nonempty and unique");
      }
    }
    for (std::size_t EarlierIndex = 0; EarlierIndex < Index; ++EarlierIndex) {
      if (SameBindingKey(Binding, this->Bindings[EarlierIndex])) {
        throw std::invalid_argument("Identity policy contains a duplicate sender binding");
      }
    }
  }
}

IdentityResolution IdentityPolicy::Resolve(
    const Channels::ChannelEnvelope& Envelope) const {
  if (!IsValidEnvelope(Envelope)) {
    return IdentityPolicyFailure{IdentityPolicyFailureCode::InvalidEnvelope};
  }

  bool AccountFound = false;
  bool RegisteredAccountRouteFound = false;
  for (const IdentityBinding& Binding : Bindings) {
    const bool SameChannel = Binding.Account.Channel.Value == Envelope.Channel.Value;
    const bool SameAccount = Binding.Account.Account.Value == Envelope.Account.Value;
    if (!SameChannel || !SameAccount) {
      continue;
    }
    AccountFound = true;

    if (Binding.Account.Mode.Value != Envelope.Mode.Value) {
      continue;
    }
    RegisteredAccountRouteFound = true;
    if (Binding.Sender.Value != Envelope.Sender.Value) {
      continue;
    }

    ConversationScope Scope;
    Scope.Principal = Binding.Principal;
    Scope.Channel = Envelope.Channel;
    Scope.Account = Envelope.Account;
    Scope.Mode = Envelope.Mode;
    Scope.Conversation = Envelope.Conversation;
    Scope.ConversationType = Envelope.ConversationType;
    Scope.Sender = Envelope.Sender;
    return ResolvedIdentity{
        .Principal = Binding.Principal,
        .Scope = std::move(Scope),
        .Account = Binding.Account,
        .GrantedCapabilities = Binding.GrantedCapabilities,
    };
  }

  if (!AccountFound) {
    return IdentityPolicyFailure{IdentityPolicyFailureCode::UnknownAccount};
  }
  if (!RegisteredAccountRouteFound) {
    return IdentityPolicyFailure{IdentityPolicyFailureCode::AccountRouteMismatch};
  }
  return IdentityPolicyFailure{IdentityPolicyFailureCode::UnknownSender};
}

std::uint64_t IdentityPolicy::GetPolicyVersion() const noexcept {
  return PolicyVersion;
}

} // namespace EmulativeDialog::Core
