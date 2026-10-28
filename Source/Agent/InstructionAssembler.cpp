#include "Source/Agent/InstructionAssembler.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EmulativeDialog::Agent {
namespace {

InstructionAssemblyFailure Failure(
    InstructionAssemblyFailureCode Code) noexcept {
  return InstructionAssemblyFailure{.Code = Code};
}

Providers::MessageRole ToProviderRole(
    Context::ProviderMessageRole Role) noexcept {
  switch (Role) {
  case Context::ProviderMessageRole::System:
    return Providers::MessageRole::System;
  case Context::ProviderMessageRole::Developer:
    return Providers::MessageRole::Developer;
  case Context::ProviderMessageRole::User:
    return Providers::MessageRole::User;
  case Context::ProviderMessageRole::Assistant:
    return Providers::MessageRole::Assistant;
  }
  return Providers::MessageRole::Assistant;
}

bool ContainsId(const std::vector<std::string>& Seen,
                std::string_view Candidate) {
  return std::find(Seen.begin(), Seen.end(), Candidate) != Seen.end();
}

} // namespace

InstructionAssembler::InstructionAssembler(Core::InstructionPolicy PolicyValue)
    : Policy(PolicyValue) {}

InstructionAssemblyResult InstructionAssembler::Assemble(
    const Context::ContextBundle& Bundle) const {
  if (Policy.GetVersion() == 0 || Policy.GetAiDisclosure().empty()) {
    return Failure(InstructionAssemblyFailureCode::InvalidPolicy);
  }
  if (Bundle.IdentityPolicyVersion != Policy.GetVersion()) {
    return Failure(InstructionAssemblyFailureCode::InvalidPolicy);
  }
  if (Bundle.Items.empty()) {
    return Failure(InstructionAssemblyFailureCode::InvalidBundle);
  }
  if (!Bundle.SystemInstructionsPreserved ||
      !Bundle.DeveloperInstructionsPreserved ||
      !Bundle.CurrentUserMessagePreserved) {
    return Failure(InstructionAssemblyFailureCode::MissingRequiredContext);
  }

  AssembledInstructions Assembled;
  Assembled.IdentityPolicyVersion = Bundle.IdentityPolicyVersion;
  Assembled.Provenance.reserve(Bundle.Items.size() + 1U);

  std::vector<std::string> SeenIds;
  SeenIds.reserve(Bundle.Items.size());

  bool HaveSystem = false;
  bool HaveDeveloper = false;
  bool HaveCurrentUser = false;
  bool HavePrevious = false;
  Core::InstructionRank PreviousRank{Core::InstructionRank::LowTrust};
  Context::ContextPriorityTier PreviousPriority{
      Context::ContextPriorityTier::Tier7};

  for (const Context::SelectedContextItem& Item : Bundle.Items) {
    if (Item.Id.Value.empty()) {
      return Failure(InstructionAssemblyFailureCode::InvalidBundle);
    }
    if (ContainsId(SeenIds, Item.Id.Value)) {
      return Failure(InstructionAssemblyFailureCode::DuplicateItem);
    }
    SeenIds.push_back(Item.Id.Value);

    if (!Core::InstructionPolicy::IsCanonicalUtf8(Item.Id.Value) ||
        !Core::InstructionPolicy::IsCanonicalUtf8(Item.Utf8Text)) {
      return Failure(InstructionAssemblyFailureCode::InvalidUtf8);
    }
    if (Item.Utf8Text.empty()) {
      return Failure(InstructionAssemblyFailureCode::InvalidBundle);
    }

    const std::optional<Core::InstructionRoleExpectation> Expectation =
        Policy.ExpectationFor(Item.Kind, Item.Trust);
    if (!Expectation.has_value()) {
      // A protected kind, or any non-assistant role on low-trust content, is a
      // spoofing attempt. Unknown kinds fail closed as unsupported.
      if (Policy.IsProtectedKind(Item.Kind) ||
          Item.ProviderRole != Context::ProviderMessageRole::Assistant) {
        return Failure(InstructionAssemblyFailureCode::RoleConflict);
      }
      return Failure(InstructionAssemblyFailureCode::UnsupportedItemKind);
    }
    if (Item.ProviderRole != Expectation->Role) {
      return Failure(InstructionAssemblyFailureCode::RoleConflict);
    }

    if (HavePrevious) {
      const bool RankDecreased = Expectation->Rank < PreviousRank;
      const bool PriorityDecreasedWithinRank =
          Expectation->Rank == PreviousRank &&
          Item.Priority < PreviousPriority;
      if (RankDecreased || PriorityDecreasedWithinRank) {
        return Failure(InstructionAssemblyFailureCode::NonDeterministicOrder);
      }
    }
    PreviousRank = Expectation->Rank;
    PreviousPriority = Item.Priority;
    HavePrevious = true;

    switch (Item.Kind) {
    case Context::ContextItemKind::SystemInstructions:
      if (HaveSystem) {
        return Failure(InstructionAssemblyFailureCode::DuplicateItem);
      }
      HaveSystem = true;
      Assembled.SystemInstructions = Item.Utf8Text;
      break;
    case Context::ContextItemKind::DeveloperInstructions:
      if (HaveDeveloper) {
        return Failure(InstructionAssemblyFailureCode::DuplicateItem);
      }
      HaveDeveloper = true;
      Assembled.DeveloperInstructions = Item.Utf8Text;
      break;
    case Context::ContextItemKind::CurrentUserMessage: {
      if (HaveCurrentUser) {
        return Failure(InstructionAssemblyFailureCode::DuplicateItem);
      }
      HaveCurrentUser = true;
      Providers::CanonicalMessage Message;
      Message.Role = Providers::MessageRole::User;
      Message.ContentParts.emplace_back(
          Providers::TextContentPart{.Text = Item.Utf8Text});
      Assembled.Messages.push_back(std::move(Message));
      break;
    }
    default: {
      Providers::CanonicalMessage Message;
      Message.Role = ToProviderRole(Expectation->Role);
      Message.ContentParts.emplace_back(
          Providers::TextContentPart{.Text = Item.Utf8Text});
      Assembled.Messages.push_back(std::move(Message));
      break;
    }
    }

    Assembled.Provenance.push_back(InstructionProvenance{
        .ItemId = Item.Id.Value,
        .Role = ToProviderRole(Expectation->Role),
        .Priority = Item.Priority,
        .Trust = Item.Trust,
        .SourceMessageId = Item.Source.SourceMessageId,
        .SourceOperationId = Item.Source.SourceOperationId,
    });
  }

  if (!HaveSystem || !HaveDeveloper || !HaveCurrentUser) {
    return Failure(InstructionAssemblyFailureCode::MissingRequiredContext);
  }

  if (Assembled.SystemInstructions.find(Policy.GetAiDisclosure()) ==
      std::string::npos) {
    Assembled.SystemInstructions.append("\n\n");
    Assembled.SystemInstructions.append(Policy.GetAiDisclosure());
    Assembled.AiDisclosureInjected = true;
    Assembled.Provenance.insert(
        Assembled.Provenance.begin(),
        InstructionProvenance{
            .ItemId = "instruction-policy:ai-disclosure",
            .Role = Providers::MessageRole::System,
            .Priority = Context::ContextPriorityTier::Tier1,
            .Trust = Context::ContextTrustClass::TrustedInstruction,
            .SourceMessageId = "",
            .SourceOperationId = "instruction-policy-v1",
        });
  }

  return Assembled;
}

} // namespace EmulativeDialog::Agent
