#pragma once

#include "Source/Core/IdentityPolicy.h"
#include "Source/Providers/Contracts/ModelProviderPort.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace EmulativeDialog::Memory {
struct MemoryGrantConsumer;
}

namespace EmulativeDialog::Core {

struct RecallBudget final {
  std::uint64_t TokenBudget{0};
};

enum class MemoryRecallCandidateKind : std::uint8_t {
  IdentityPreference,
  ConversationSummary,
  GroupMemory,
  IdentityLinkMemory,
  ActiveConversationContext,
};

enum class MemoryRecallConfidence : std::uint8_t { Low, Medium, High };

struct MemoryRecallCandidate final {
  std::string MemoryId;
  MemoryRecallCandidateKind Kind{MemoryRecallCandidateKind::ConversationSummary};
  std::uint8_t Priority{0};
  std::string Content;
  std::string SourceMessageId;
  std::chrono::sys_seconds CreatedAt{};
  std::optional<std::chrono::sys_seconds> ExpiresAt;
  bool UserConfirmed{false};
  MemoryRecallConfidence Confidence{MemoryRecallConfidence::Low};
  std::uint64_t TokenCost{0};
};

enum class MemoryRecallErrorCode : std::uint8_t {
  InvalidGrant,
  InvalidBudget,
  UnauthorizedIdentity,
  SessionScopeVolatile,
  ConsentMismatch,
  StorageFailure,
};

struct MemoryRecallError final {
  MemoryRecallErrorCode Code{MemoryRecallErrorCode::InvalidGrant};
};

using MemoryRecallResult =
    std::variant<std::vector<MemoryRecallCandidate>, MemoryRecallError>;

class MemoryReadGrant final {
public:
  MemoryReadGrant(const MemoryReadGrant&) = delete;
  MemoryReadGrant& operator=(const MemoryReadGrant&) = delete;
  MemoryReadGrant(MemoryReadGrant&& Other) noexcept;
  MemoryReadGrant& operator=(MemoryReadGrant&& Other) noexcept = delete;
  ~MemoryReadGrant() = default;

  [[nodiscard]] const PrincipalId& GetPrincipal() const noexcept {
    return Principal;
  }
  [[nodiscard]] const ConversationScope& GetScope() const noexcept {
    return Scope;
  }
  [[nodiscard]] std::uint64_t GetPolicyVersion() const noexcept {
    return PolicyVersion;
  }
  [[nodiscard]] const Providers::AgentTurnId& GetAgentTurn() const noexcept {
    return AgentTurn;
  }

private:
  friend class CoreChatService;
  friend struct EmulativeDialog::Memory::MemoryGrantConsumer;

  MemoryReadGrant(PrincipalId PrincipalValue, ConversationScope ScopeValue,
                  std::uint64_t PolicyVersionValue,
                  Providers::AgentTurnId AgentTurnValue);

  [[nodiscard]] bool Consume() noexcept;

  PrincipalId Principal;
  ConversationScope Scope;
  std::uint64_t PolicyVersion{0};
  Providers::AgentTurnId AgentTurn;
  bool Consumed{false};
};

class MemoryRecallPort {
public:
  virtual ~MemoryRecallPort() = default;
  [[nodiscard]] virtual MemoryRecallResult Recall(
      MemoryReadGrant Grant, const RecallBudget& Budget) = 0;
};

} // namespace EmulativeDialog::Core
