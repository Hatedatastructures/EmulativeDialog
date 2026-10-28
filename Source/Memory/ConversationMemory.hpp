#pragma once

#include "Source/Context/ContextSelector.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace EmulativeDialog::Memory {

enum class MemoryCategory : std::uint8_t {
  IdentityPreference,
  ConversationSummary,
  GroupMemory,
  IdentityLinkMemory,
  ActiveConversationContext,
};

enum class MemoryProvenance : std::uint8_t {
  UserExplicit,
  UserConfirmed,
  AuthorizedSummary,
};

struct AuthorizedMemoryContext final {
  std::string ChannelId;
  std::string AccountId;
  std::string ConversationId;
  std::string PrincipalId;
  std::uint64_t IdentityPolicyVersion{0};
  bool IdentityVerified{false};

  [[nodiscard]] bool IsComplete() const noexcept;
};

struct ConsentGrant final {
  std::string ConsentId;
  std::string GrantorPrincipalId;
  std::string ChannelId;
  std::string AccountId;
  std::string ConversationId;
  MemoryCategory Category{MemoryCategory::IdentityPreference};
  bool Granted{true};
};

struct MemoryWriteRequest final {
  std::string MemoryId;
  MemoryCategory Category{MemoryCategory::IdentityPreference};
  std::string SubjectId;
  std::string Content;
  std::string SourceReference;
  std::string ConsentId;
  std::optional<std::string> ProposalId;
  std::chrono::sys_seconds CreatedAt{};
  std::optional<std::chrono::sys_seconds> ExpiresAt;
  MemoryProvenance Provenance{MemoryProvenance::UserExplicit};
  bool UserConfirmed{false};
  std::uint64_t Version{1};
};

struct StoredMemory final {
  std::string MemoryId;
  MemoryCategory Category{MemoryCategory::IdentityPreference};
  std::string ChannelId;
  std::string AccountId;
  std::string ConversationId;
  std::string PrincipalId;
  std::string SubjectId;
  std::string Content;
  std::string SourceReference;
  std::string ConsentId;
  std::chrono::sys_seconds CreatedAt{};
  std::optional<std::chrono::sys_seconds> ExpiresAt;
  MemoryProvenance Provenance{MemoryProvenance::UserExplicit};
  bool UserConfirmed{false};
  std::uint64_t Version{1};
  bool Tombstoned{false};
};

enum class MemoryStorageErrorCode : std::uint8_t {
  NotFound,
  VersionConflict,
  InvalidArgument,
  Failure,
};

struct MemoryStorageError final {
  MemoryStorageErrorCode Code{MemoryStorageErrorCode::Failure};
  std::string Detail;
};

template <typename Value>
using MemoryStorageResult = std::variant<Value, MemoryStorageError>;

// Storage seam implemented by the pinned SQLite store (production) and an
// in-memory fake (unit contracts). Authorization never lives in storage.
class MemoryStoragePort {
public:
  virtual ~MemoryStoragePort() = default;
  MemoryStoragePort(const MemoryStoragePort&) = delete;
  MemoryStoragePort& operator=(const MemoryStoragePort&) = delete;

  [[nodiscard]] virtual MemoryStorageResult<std::monostate> Put(
      const StoredMemory& Record) = 0;
  [[nodiscard]] virtual MemoryStorageResult<StoredMemory> GetActive(
      const StoredMemory& ScopeTemplate, std::string_view MemoryId,
      std::chrono::sys_seconds Now) const = 0;
  [[nodiscard]] virtual MemoryStorageResult<std::vector<StoredMemory>>
  ListActive(const StoredMemory& ScopeTemplate,
             std::chrono::sys_seconds Now) const = 0;
  [[nodiscard]] virtual MemoryStorageResult<std::monostate> Tombstone(
      const StoredMemory& ScopeTemplate, std::string_view MemoryId,
      std::chrono::sys_seconds Now) = 0;
  [[nodiscard]] virtual MemoryStorageResult<std::vector<StoredMemory>>
  RecallBackup(const StoredMemory& ScopeTemplate,
               std::chrono::sys_seconds Now) const = 0;

protected:
  MemoryStoragePort() = default;
};

enum class MemoryErrorCode : std::uint8_t {
  UnauthorizedIdentity,
  ConsentRequired,
  ConsentMismatch,
  SensitiveContent,
  InvalidArgument,
  StorageFailure,
  ForgetScopeRequired,
};

struct MemoryError final {
  MemoryErrorCode Code{MemoryErrorCode::InvalidArgument};
  std::string Detail;
};

template <typename Value>
using MemoryResult = std::variant<Value, MemoryError>;

struct ForgetRequest final {
  std::optional<std::string> MemoryId;
  std::optional<MemoryCategory> Category;
  bool EntireAuthorizedScope{false};
};

// Chat memory built over the scoped storage seam. Development task ledgers are
// a different domain and must never be stored here.
class ConversationMemory final {
public:
  explicit ConversationMemory(MemoryStoragePort& Storage,
                              std::size_t PerScopeCapacity = 8) noexcept;

  [[nodiscard]] MemoryResult<std::monostate> RegisterConsent(
      const ConsentGrant& Grant);
  [[nodiscard]] MemoryResult<std::monostate> RevokeConsent(
      const AuthorizedMemoryContext& Context, std::string_view ConsentId,
      std::chrono::sys_seconds Now);
  [[nodiscard]] MemoryResult<std::monostate> Write(
      const AuthorizedMemoryContext& Context,
      const MemoryWriteRequest& Request,
      std::chrono::sys_seconds Now);
  [[nodiscard]] MemoryResult<std::vector<Context::ContextCandidate>> Recall(
      const AuthorizedMemoryContext& Context, std::chrono::sys_seconds Now,
      std::uint64_t TokenBudget) const;
  [[nodiscard]] MemoryResult<std::monostate> Forget(
      const AuthorizedMemoryContext& Context, const ForgetRequest& Request,
      std::chrono::sys_seconds Now);
  [[nodiscard]] MemoryResult<std::size_t> RestoreBackup(
      const AuthorizedMemoryContext& Context, std::chrono::sys_seconds Now);
  [[nodiscard]] bool IsConsentActive(std::string_view ConsentId) const;

private:
  struct ActiveConsent final {
    ConsentGrant Grant;
    bool Active{true};
  };

  [[nodiscard]] static MemoryError Error(MemoryErrorCode Code,
                                         std::string Detail);
  [[nodiscard]] static std::optional<MemoryError> Authorize(
      const AuthorizedMemoryContext& Context,
      const MemoryWriteRequest& Request);
  [[nodiscard]] StoredMemory ScopeTemplate(
      const AuthorizedMemoryContext& Context) const;
  [[nodiscard]] const ActiveConsent* FindConsent(
      std::string_view ConsentId) const;
  [[nodiscard]] bool ConsentMatches(
      const ActiveConsent& Consent,
      const AuthorizedMemoryContext& Context,
      MemoryCategory Category) const;
  [[nodiscard]] MemoryResult<std::vector<StoredMemory>> ListActive(
      const AuthorizedMemoryContext& Context,
      std::chrono::sys_seconds Now) const;

  MemoryStoragePort& Storage_;
  std::size_t PerScopeCapacity_;
  std::vector<ActiveConsent> Consents_;
};

} // namespace EmulativeDialog::Memory
