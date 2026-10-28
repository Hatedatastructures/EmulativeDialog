#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Source/Core/IdentityPolicy.h"
#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Core {

enum class BusinessActionExecutionStatus : std::uint8_t {
  Succeeded,
  Failed,
  UnknownOutcome,
};

struct BusinessActionExecutionResult final {
  BusinessActionExecutionStatus Status{
      BusinessActionExecutionStatus::Failed};
  // Stable, sanitized result code only; never raw platform payloads.
  std::string SanitizedCode;
};

struct BusinessActionInvocation final {
  std::string Name;
  std::string ToolCallId;
  Providers::JsonValue Arguments;
  PrincipalId Principal;
  ConversationScope Scope;
  Providers::AgentTurnId Turn;
};

struct BusinessActionScopeRequirement final {
  std::optional<std::string> ChannelId;
  std::optional<std::string> ConversationId;
};

struct BusinessActionDefinition final {
  std::string Name;
  std::vector<std::string> RequiredArgumentFields;
  std::vector<std::string> RequiredCapabilities;
  BusinessActionScopeRequirement Scope;
  std::function<BusinessActionExecutionResult(
      const BusinessActionInvocation&)>
      Handler;
};

enum class BusinessActionRegistrationErrorCode : std::uint8_t {
  DuplicateAction,
  InvalidActionName,
  ForbiddenActionName,
  EmptyHandler,
  InvalidSchemaField,
};

struct BusinessActionRegistrationError final {
  BusinessActionRegistrationErrorCode Code{
      BusinessActionRegistrationErrorCode::InvalidActionName};

  friend bool operator==(const BusinessActionRegistrationError&,
                         const BusinessActionRegistrationError&) = default;
};

// Core-owned explicit allowlist for chat business actions. It starts empty and
// only accepts explicitly registered actions; there is no generic Shell,
// filesystem, network, browser, or MCP action. Single-owner, no locking.
class BusinessActionRegistry final {
public:
  [[nodiscard]] std::optional<BusinessActionRegistrationError> Register(
      BusinessActionDefinition Definition);

  [[nodiscard]] const BusinessActionDefinition* Find(
      std::string_view Name) const noexcept;
  [[nodiscard]] std::size_t Count() const noexcept {
    return Definitions.size();
  }

  // PascalCase business action names only, at most 64 characters.
  [[nodiscard]] static bool IsValidActionName(
      std::string_view Name) noexcept;
  [[nodiscard]] static bool IsForbiddenToolName(std::string_view Name);

  // Validates that JsonText is a well-formed JSON object without duplicate
  // top-level keys and that every required top-level field is present.
  [[nodiscard]] static bool ValidateArguments(
      std::string_view JsonText,
      const std::vector<std::string>& RequiredFields) noexcept;

private:
  std::vector<BusinessActionDefinition> Definitions;
};

} // namespace EmulativeDialog::Core
