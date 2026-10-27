#pragma once

#include <string_view>

namespace EmulativeDialog::Core {

class CredentialLease {
public:
  CredentialLease() = default;
  CredentialLease(const CredentialLease&) = delete;
  CredentialLease& operator=(const CredentialLease&) = delete;
  CredentialLease(CredentialLease&&) = delete;
  CredentialLease& operator=(CredentialLease&&) = delete;
  virtual ~CredentialLease() = default;

  [[nodiscard]] virtual std::string_view GetAuthenticationSecret() const noexcept = 0;
};

} // namespace EmulativeDialog::Core
