#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace EmulativeDialog::Core {

// Core owns these definitions. The Channel port accepts them only by const
// reference and does not create credentials or authorization tokens.
class CredentialLease;
class CoreAuthorizedAction;

} // namespace EmulativeDialog::Core

namespace EmulativeDialog::Channels {

template <typename Tag>
struct OpaqueIdentifier {
  std::string Value;
};

struct ChannelIdTag {};
struct AccountIdTag {};
struct IntegrationModeTag {};
struct ConversationIdTag {};
struct SenderIdTag {};
struct MessageIdTag {};
struct PlatformEventIdTag {};
struct StableDeduplicationIdTag {};
struct MediaReferenceIdTag {};
struct ActionIdTag {};
struct OperationIdTag {};
struct IdempotencyKeyTag {};
struct PlatformReceiptIdTag {};
struct EvidenceReferenceTag {};

using ChannelId = OpaqueIdentifier<ChannelIdTag>;
using AccountId = OpaqueIdentifier<AccountIdTag>;
using IntegrationMode = OpaqueIdentifier<IntegrationModeTag>;
using ConversationId = OpaqueIdentifier<ConversationIdTag>;
using SenderId = OpaqueIdentifier<SenderIdTag>;
using MessageId = OpaqueIdentifier<MessageIdTag>;
using PlatformEventId = OpaqueIdentifier<PlatformEventIdTag>;
using StableDeduplicationId = OpaqueIdentifier<StableDeduplicationIdTag>;
using MediaReferenceId = OpaqueIdentifier<MediaReferenceIdTag>;
using ActionId = OpaqueIdentifier<ActionIdTag>;
using OperationId = OpaqueIdentifier<OperationIdTag>;
using IdempotencyKey = OpaqueIdentifier<IdempotencyKeyTag>;
using PlatformReceiptId = OpaqueIdentifier<PlatformReceiptIdTag>;
using EvidenceReference = OpaqueIdentifier<EvidenceReferenceTag>;
using Timestamp = std::chrono::system_clock::time_point;

struct AccountProfile {
  ChannelId Channel;
  AccountId Account;
  IntegrationMode Mode;
};

enum class ConversationKind {
  Private,
  Group,
};

// This envelope is created only after the Adapter verifies platform provenance.
// Core still checks account registration, routing, and StableDeduplicationId.
struct ChannelEnvelope {
  ChannelId Channel;
  IntegrationMode Mode;
  AccountId Account;
  ConversationId Conversation;
  ConversationKind ConversationType;
  SenderId Sender;
  MessageId Message;
  PlatformEventId PlatformEvent;
  Timestamp OccurredAt;
  StableDeduplicationId StableDeduplication;
};

enum class MediaReferenceKind {
  Image,
  Audio,
  Video,
  Sticker,
};

struct TextPart {
  std::string Utf8Text;
};

// A normalized opaque reference, never an arbitrary local path or platform object.
struct MediaReferencePart {
  MediaReferenceId Reference;
  MediaReferenceKind Kind;
};

using MessagePart = std::variant<TextPart, MediaReferencePart>;

struct PlatformMetadataEntry {
  // Adapters may emit only metadata keys/values allowed for the selected mode.
  std::string Key;
  std::string Value;
};

// Deliberately contains no channel, account, conversation, or sender identity.
struct CanonicalMessage {
  std::vector<MessagePart> MessageParts;
  std::optional<MessageId> ReplyToId;
  std::vector<SenderId> MentionedIds;
  std::vector<PlatformMetadataEntry> PlatformMetadata;
};

struct InboundChannelMessage {
  ChannelEnvelope Envelope;
  CanonicalMessage Message;
};

enum class ChannelCapability {
  ReceiveText,
  ReceiveMedia,
  ReceiveSticker,
  SendText,
  SendRichText,
  SendMedia,
  SendSticker,
  AddReaction,
  Poke,
  LookupDisplayName,
  SendProactiveMessage,
};

enum class CapabilityStatus {
  Unknown,
  Unsupported,
  Supported,
};

enum class VerificationStatus {
  Unknown,
  Pending,
  Verified,
  Rejected,
  Expired,
};

struct CapabilityObservation {
  ChannelCapability Capability;
  CapabilityStatus Status;
  VerificationStatus EvidenceStatus;
  Timestamp ObservedAt;
  std::optional<Timestamp> ValidUntil;
  std::optional<EvidenceReference> Evidence;
};

struct CapabilitySnapshot {
  ChannelId Channel;
  AccountId Account;
  IntegrationMode Mode;
  std::uint64_t SnapshotVersion;
  Timestamp CapturedAt;
  std::optional<Timestamp> ValidUntil;
  VerificationStatus AuthenticationStatus;
  VerificationStatus AuditStatus;
  std::vector<CapabilityObservation> Capabilities;
};

enum class ChannelErrorCode {
  None,
  InvalidProfile,
  AuthenticationFailure,
  InvalidRequest,
  UnsupportedCapability,
  Forbidden,
  RateLimited,
  PlatformRejected,
  TransportFailure,
  OutcomeUnknown,
};

enum class StartStatus {
  Started,
  AlreadyStarted,
  Failed,
};

struct StartResult {
  StartStatus Status;
  ChannelErrorCode ErrorCode;
};

enum class StopStatus {
  Stopped,
  AlreadyStopped,
  Failed,
};

struct StopResult {
  StopStatus Status;
  ChannelErrorCode ErrorCode;
};

enum class SubscriptionStatus {
  Subscribed,
  AlreadySubscribed,
  Failed,
};

struct SubscriptionResult {
  SubscriptionStatus Status;
  ChannelErrorCode ErrorCode;
};

enum class ReceiptStatus {
  Succeeded,
  Rejected,
  UnsupportedCapability,
  Forbidden,
  RateLimited,
  Failed,
  UnknownOutcome,
};

struct ChannelReceipt {
  ActionId Action;
  OperationId Operation;
  std::optional<PlatformReceiptId> PlatformReceipt;
  Timestamp OccurredAt;
  ReceiptStatus Status;
  ChannelErrorCode ErrorCode;
  std::optional<Timestamp> RetryAfter;
};

enum class ConnectionStatus {
  Unknown,
  Stopped,
  Starting,
  Connected,
  Degraded,
};

enum class AuthenticationStatus {
  Unknown,
  Valid,
  Invalid,
  Expired,
};

enum class RateLimitStatus {
  Unknown,
  Normal,
  Limited,
};

struct HealthSnapshot {
  ConnectionStatus Connection;
  AuthenticationStatus Authentication;
  RateLimitStatus RateLimit;
  Timestamp CapturedAt;
  std::optional<Timestamp> RetryAfter;
  ChannelErrorCode ErrorCode;
};

class ChannelEventSink {
public:
  virtual ~ChannelEventSink() = default;

  // Receives only the verified Envelope paired with its normalized, untrusted
  // message. The sink must remain alive until the Adapter is stopped.
  virtual void OnChannelMessage(const InboundChannelMessage& Message) = 0;
};

class ChannelAdapterPort {
public:
  virtual ~ChannelAdapterPort() = default;

  // Capabilities describe this exact account and integration mode. Missing or
  // unverified evidence must be reported as Unknown or Unsupported.
  virtual CapabilitySnapshot GetCapabilities(const AccountProfile& Profile) = 0;

  // The lease is borrowed for this call. Implementations must not persist it.
  virtual StartResult Start(const AccountProfile& Profile,
                            const Core::CredentialLease& Lease) = 0;

  // Stop releases the active connection and is required to be idempotent.
  virtual StopResult Stop() = 0;

  // Implementations publish only platform-source-verified envelopes and their
  // normalized message. Stop ends event delivery for this subscription.
  virtual SubscriptionResult SubscribeEvents(ChannelEventSink& Sink) = 0;

  // Only Core can provide a CoreAuthorizedAction. The Adapter executes one
  // action and returns exactly one sanitized receipt; it does not authorize it.
  virtual ChannelReceipt SendAction(
      const Core::CoreAuthorizedAction& Action,
      const Core::CredentialLease& Lease) = 0;

  // Health information contains only redacted connection/auth/rate-limit state.
  virtual HealthSnapshot GetHealthSnapshot() const = 0;
};

} // namespace EmulativeDialog::Channels