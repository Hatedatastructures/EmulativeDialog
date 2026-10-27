#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "Source/Channels/Contracts/ChannelAdapterPort.h"
#include "Source/Core/IdentityPolicy.h"

namespace EmulativeDialog::Core {

class CoreChatService;

struct AuthorizationRef final {
  std::string Value;

private:
  explicit AuthorizationRef(std::string Value) : Value(std::move(Value)) {}
  friend class CoreChatService;
};

enum class CoreAuthorizedActionKind : std::uint8_t {
  SendText,
};

class CoreAuthorizedAction final {
public:
  CoreAuthorizedAction(const CoreAuthorizedAction&) = delete;
  CoreAuthorizedAction& operator=(const CoreAuthorizedAction&) = delete;
  CoreAuthorizedAction(CoreAuthorizedAction&&) = delete;
  CoreAuthorizedAction& operator=(CoreAuthorizedAction&&) = delete;
  ~CoreAuthorizedAction() = default;

  [[nodiscard]] const Channels::ActionId& GetActionId() const noexcept;
  [[nodiscard]] const Channels::OperationId& GetOperationId() const noexcept;
  [[nodiscard]] const Channels::IdempotencyKey& GetIdempotencyKey() const noexcept;
  [[nodiscard]] const ConversationScope& GetTargetScope() const noexcept;
  [[nodiscard]] CoreAuthorizedActionKind GetActionKind() const noexcept;
  [[nodiscard]] std::string_view GetTextPayload() const noexcept;
  [[nodiscard]] const AuthorizationRef& GetAuthorizationRef() const noexcept;
  [[nodiscard]] std::uint64_t GetPolicyVersion() const noexcept;
  [[nodiscard]] std::uint64_t GetCapabilitySnapshotVersion() const noexcept;

private:
  friend class CoreChatService;

  CoreAuthorizedAction(Channels::ActionId ActionId,
                       Channels::OperationId OperationId,
                       Channels::IdempotencyKey IdempotencyKey,
                       ConversationScope TargetScope,
                       std::string TextPayload,
                       AuthorizationRef Authorization,
                       std::uint64_t PolicyVersion,
                       std::uint64_t CapabilitySnapshotVersion);

  Channels::ActionId ActionId;
  Channels::OperationId OperationId;
  Channels::IdempotencyKey IdempotencyKey;
  ConversationScope TargetScope;
  CoreAuthorizedActionKind ActionKind{CoreAuthorizedActionKind::SendText};
  std::string TextPayload;
  AuthorizationRef Authorization;
  std::uint64_t PolicyVersion{};
  std::uint64_t CapabilitySnapshotVersion{};
};

} // namespace EmulativeDialog::Core
