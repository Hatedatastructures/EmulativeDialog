#include "Source/Agent/InstructionAssembler.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
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
  assert(false && "InstructionPolicy returned an unexpected provider role");
  std::abort();
}

bool ContainsId(const std::vector<std::string>& Seen,
                std::string_view Candidate) {
  return std::find(Seen.begin(), Seen.end(), Candidate) != Seen.end();
}

bool IsBlank(std::string_view Text) {
  return std::all_of(Text.begin(), Text.end(), [](char Value) {
    switch (Value) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
      return true;
    default:
      return false;
    }
  });
}

bool EndsWithDisclosureParagraph(std::string_view Text,
                                 std::string_view Disclosure) {
  if (Text == Disclosure) {
    return true;
  }
  if (Text.size() < Disclosure.size() + 2U) {
    return false;
  }
  const std::size_t Start = Text.size() - Disclosure.size();
  if (Text.compare(Start, Disclosure.size(), Disclosure) != 0) {
    return false;
  }
  // The disclosure must be its own final paragraph, so an inline quotation or
  // negation in the preceding sentence cannot satisfy the policy.
  return Text.substr(0, Start).ends_with("\n\n");
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
    if (Item.Source.SourceMessageId.empty() &&
        Item.Source.SourceOperationId.empty()) {
      return Failure(InstructionAssemblyFailureCode::MissingSourceReference);
    }
    if (IsBlank(Item.Utf8Text)) {
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

  // The disclosure must be the exact final paragraph. A mere mention,
  // quotation, or negation of the disclosure text does not satisfy policy.
  if (!EndsWithDisclosureParagraph(Assembled.SystemInstructions,
                                   Policy.GetAiDisclosure())) {
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
