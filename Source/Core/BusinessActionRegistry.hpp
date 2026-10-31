#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

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

enum class BusinessActionEffectClass : std::uint8_t {
  Unspecified,
  InProcessReadOnly,
  ExternalWrite,
  ExternalIo,
};

struct BusinessActionDefinition final {
  std::string Name;
  std::vector<std::string> RequiredArgumentFields;
  std::vector<std::string> RequiredCapabilities;
  BusinessActionScopeRequirement Scope;
  std::function<BusinessActionExecutionResult(
      const BusinessActionInvocation&)>
      Handler;
  Providers::ToolDefinition ModelTool;
  BusinessActionEffectClass EffectClass{
      BusinessActionEffectClass::Unspecified};
  std::function<boost::asio::awaitable<BusinessActionExecutionResult>(
      const BusinessActionInvocation&, std::stop_token)>
      AsyncHandler;
};

enum class BusinessActionRegistrationErrorCode : std::uint8_t {
  DuplicateAction,
  InvalidActionName,
  ForbiddenActionName,
  EmptyHandler,
  InvalidSchemaField,
  InvalidToolDefinition,
  IneligibleEffectClass,
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
  [[nodiscard]] std::vector<const BusinessActionDefinition*>
  GetAsyncDefinitions() const;
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

  // Validates the contract-supported JSON Schema subset and the complete
  // argument object, including primitive types and additionalProperties.
  [[nodiscard]] static bool IsValidToolDefinition(
      const Providers::ToolDefinition& Tool) noexcept;
  [[nodiscard]] static bool ValidateArguments(
      const Providers::JsonValue& Arguments,
      const Providers::ToolDefinition& Tool) noexcept;

  // Measures the deterministic compact JSON representation used by Core
  // validation without materializing a second serialized string. Object key
  // order does not affect the measured byte length; arrays retain input order.
  // NestedStringBytes reports decoded UTF-8 key/value bytes for the separately
  // retained parsed representation. ExceedsLimit is set before any result is
  // retained when either bounded measure would cross its allowance.
  [[nodiscard]] static bool MeasureCanonicalArgumentJson(
      const Providers::JsonValue& Arguments,
      std::size_t MaximumInputBytes,
      std::size_t MaximumCanonicalBytes,
      std::size_t MaximumNestedStringBytes,
      std::size_t& CanonicalBytes,
      std::size_t& NestedStringBytes,
      bool& ExceedsLimit) noexcept;

private:
  std::vector<BusinessActionDefinition> Definitions;
};

} // namespace EmulativeDialog::Core
