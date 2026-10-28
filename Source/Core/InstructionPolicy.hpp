#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "Source/Context/ContextSelector.hpp"

namespace EmulativeDialog::Core {

// Deterministic precedence rank for instruction assembly. Lower values are
// more authoritative and can never be overridden by later bundle items.
enum class InstructionRank : std::uint8_t {
  System = 1,
  Developer = 2,
  CurrentUser = 3,
  LowTrust = 4,
};

struct InstructionRoleExpectation final {
  Context::ProviderMessageRole Role{Context::ProviderMessageRole::Assistant};
  InstructionRank Rank{InstructionRank::LowTrust};
};

// Core-owned policy for how trusted instructions and low-trust context may be
// assembled into a provider request. The policy itself is never derived from
// conversation text, memory, provider output, or sender claims.
class InstructionPolicy final {
public:
  static constexpr std::uint64_t V1Version = 1;

  // Mandatory identity disclosure. It is appended by the assembler when a
  // system instruction block omits it, so a persona prompt cannot hide that
  // the assistant is software.
  static constexpr std::string_view V1AiDisclosure =
      "You are an automated AI assistant. Never claim to be a real person, "
      "and never hide that replies are produced by software.";

  [[nodiscard]] static InstructionPolicy V1() noexcept;

  [[nodiscard]] std::optional<InstructionRoleExpectation> ExpectationFor(
      Context::ContextItemKind Kind,
      Context::ContextTrustClass Trust) const noexcept;

  [[nodiscard]] bool IsProtectedKind(
      Context::ContextItemKind Kind) const noexcept;

  // Strict UTF-8 validation used before any bundle text reaches a provider
  // request. Invalid byte sequences fail closed instead of being replaced.
  [[nodiscard]] static bool IsCanonicalUtf8(std::string_view Text) noexcept;

  [[nodiscard]] std::uint64_t GetVersion() const noexcept { return Version; }
  [[nodiscard]] std::string_view GetAiDisclosure() const noexcept {
    return AiDisclosure;
  }

private:
  InstructionPolicy(std::uint64_t VersionValue, std::string_view Disclosure)
      : Version(VersionValue), AiDisclosure(Disclosure) {}

  std::uint64_t Version{};
  std::string_view AiDisclosure{};
};

} // namespace EmulativeDialog::Core
