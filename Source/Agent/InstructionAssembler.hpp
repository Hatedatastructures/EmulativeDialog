#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "Source/Context/ContextSelector.hpp"
#include "Source/Core/InstructionPolicy.hpp"
#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Agent {

enum class InstructionAssemblyFailureCode : std::uint8_t {
  InvalidPolicy,
  InvalidBundle,
  MissingRequiredContext,
  RoleConflict,
  NonDeterministicOrder,
  DuplicateItem,
  UnsupportedItemKind,
  InvalidUtf8,
  MissingSourceReference,
};

struct InstructionAssemblyFailure final {
  InstructionAssemblyFailureCode Code{
      InstructionAssemblyFailureCode::InvalidBundle};

  friend bool operator==(const InstructionAssemblyFailure&,
                         const InstructionAssemblyFailure&) = default;
};

// Observability record proving where each assembled message came from. It
// contains metadata only, never secrets or raw provider payloads.
struct InstructionProvenance final {
  std::string ItemId;
  Providers::MessageRole Role{Providers::MessageRole::Assistant};
  Context::ContextPriorityTier Priority{Context::ContextPriorityTier::Tier3};
  Context::ContextTrustClass Trust{
      Context::ContextTrustClass::LowTrustHistory};
  std::string SourceMessageId;
  std::string SourceOperationId;

  friend bool operator==(const InstructionProvenance&,
                         const InstructionProvenance&) = default;
};

struct AssembledInstructions final {
  std::string SystemInstructions;
  std::string DeveloperInstructions;
  std::vector<Providers::CanonicalMessage> Messages;
  std::vector<InstructionProvenance> Provenance;
  std::uint64_t IdentityPolicyVersion{};
  bool AiDisclosureInjected{false};
};

using InstructionAssemblyResult =
    std::variant<AssembledInstructions, InstructionAssemblyFailure>;

// Deterministically turns a ContextSelector bundle into provider-ready
// instruction blocks and messages. Protected policy can only come from
// trusted instruction items; low-trust text is always assembled as assistant
// context and can never replace system or developer policy.
class InstructionAssembler final {
public:
  explicit InstructionAssembler(
      Core::InstructionPolicy Policy = Core::InstructionPolicy::V1());

  [[nodiscard]] InstructionAssemblyResult Assemble(
      const Context::ContextBundle& Bundle) const;

private:
  Core::InstructionPolicy Policy;
};

} // namespace EmulativeDialog::Agent
