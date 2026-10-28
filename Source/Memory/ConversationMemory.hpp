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

inline constexpr int SensitiveContentPolicyVersion = 1;

enum class ConversationScopeKind : std::uint8_t {
  Direct,
  Group,
  Session,
};

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
  ConversationScopeKind ScopeKind{ConversationScopeKind::Direct};
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
  ConversationScopeKind ScopeKind{ConversationScopeKind::Direct};
  MemoryCategory Category{MemoryCategory::IdentityPreference};
  std::string SourceMessageId;
  std::chrono::sys_seconds GrantedAt{};
  std::optional<std::string> ProposalId;
  std::uint64_t IdentityPolicyVersion{0};
  // Exact verified members allowed to read/write a GroupMemory consent.
  std::vector<std::string> AllowedGroupMembers;
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
  ConversationScopeKind ScopeKind{ConversationScopeKind::Direct};
  std::string PrincipalId;
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

// Atomic storage batch. Implementations apply every upsert and tombstone in a
// single transaction; a failure leaves the previous state untouched.
struct MemoryMutationBatch final {
  StoredMemory Scope;
  std::vector<StoredMemory> Upserts;
  std::vector<std::string> Tombstones;
};

class MemoryStoragePort {
public:
  virtual ~MemoryStoragePort() = default;
  MemoryStoragePort(const MemoryStoragePort&) = delete;
  MemoryStoragePort& operator=(const MemoryStoragePort&) = delete;

  [[nodiscard]] virtual MemoryStorageResult<std::monostate> ApplyAtomically(
      const MemoryMutationBatch& Batch) = 0;
  [[nodiscard]] virtual MemoryStorageResult<std::vector<StoredMemory>>
  ListActive(const StoredMemory& ScopeTemplate,
             std::chrono::sys_seconds Now) const = 0;
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
  SessionScopeVolatile,
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
  [[nodiscard]] static bool ContainsSensitiveContent(std::string_view Text);
  [[nodiscard]] static std::optional<MemoryError> Authorize(
      const AuthorizedMemoryContext& Context,
      const MemoryWriteRequest& Request);
  [[nodiscard]] static bool SameScope(
      const ConsentGrant& Grant, const AuthorizedMemoryContext& Context);
  [[nodiscard]] StoredMemory ScopeTemplate(
      const AuthorizedMemoryContext& Context) const;
  [[nodiscard]] const ActiveConsent* FindConsent(
      std::string_view ConsentId) const;
  [[nodiscard]] const ActiveConsent* FindExactConsent(
      const AuthorizedMemoryContext& Context,
      std::string_view ConsentId) const;
  [[nodiscard]] bool ConsentMatches(
      const ActiveConsent& Consent,
      const AuthorizedMemoryContext& Context, MemoryCategory Category,
      const std::optional<std::string>& ProposalId) const;
  [[nodiscard]] MemoryResult<std::vector<StoredMemory>> ListActive(
      const AuthorizedMemoryContext& Context,
      std::chrono::sys_seconds Now) const;

  MemoryStoragePort& Storage_;
  std::size_t PerScopeCapacity_;
  std::vector<ActiveConsent> Consents_;
};

} // namespace EmulativeDialog::Memory
