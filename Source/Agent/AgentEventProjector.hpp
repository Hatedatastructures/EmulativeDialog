#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

#include "Source/Providers/Contracts/ModelProviderPort.h"

namespace EmulativeDialog::Agent {

enum class AgentEventType : std::uint8_t {
  Started,
  TextDelta,
  RefusalDelta,
  ToolCallStarted,
  ToolArgumentDelta,
  ToolCallReady,
  UsageUpdated,
  Final,
  Refusal,
  ToolHandoff,
  OutputLimited,
  Failed,
  Cancelled,
  OutcomeUnknown,
};

struct AgentEventMetadata final {
  Providers::RequestId Request;
  Providers::AgentTurnId Turn;
  std::uint64_t SequenceNumber{};

  friend bool operator==(const AgentEventMetadata&,
                         const AgentEventMetadata&) = default;
};

struct AgentStartedEvent final {
  AgentEventMetadata Metadata;
  std::optional<Providers::ProviderResponseId> Response;
};

struct AgentTextDeltaEvent final {
  AgentEventMetadata Metadata;
  Providers::ItemId Item;
  std::string Utf8Delta;
};

struct AgentRefusalDeltaEvent final {
  AgentEventMetadata Metadata;
  Providers::ItemId Item;
  std::string Utf8Delta;
  std::optional<Providers::RefusalCategory> Category;
};

struct AgentToolCallStartedEvent final {
  AgentEventMetadata Metadata;
  Providers::ItemId Item;
  Providers::ToolCallId Call;
  std::string ToolName;
};

struct AgentToolArgumentDeltaEvent final {
  AgentEventMetadata Metadata;
  Providers::ItemId Item;
  Providers::ToolCallId Call;
  std::string JsonTextDelta;
};

struct AgentToolCallReadyEvent final {
  AgentEventMetadata Metadata;
  Providers::ItemId Item;
  Providers::ToolCallId Call;
  std::string ToolName;
  Providers::JsonValue Arguments;
};

struct AgentUsageUpdatedEvent final {
  AgentEventMetadata Metadata;
  Providers::UsageSnapshot Usage;
};

struct AgentFinalEvent final {
  AgentEventMetadata Metadata;
};

struct AgentRefusalEvent final {
  AgentEventMetadata Metadata;
  std::optional<Providers::RefusalCategory> Category;
};

struct AgentToolHandoffEvent final {
  AgentEventMetadata Metadata;
};

// Partial output produced before an output limit is never deliverable. Only
// the presence of unsendable text is projected; the bytes stay behind.
struct AgentOutputLimitedEvent final {
  AgentEventMetadata Metadata;
  Providers::LimitKind Kind{Providers::LimitKind::OutputTokens};
  Providers::UsageSnapshot Usage;
  bool HasUnsendablePartialText{false};
};

// Only the stable failure code and retryability are projected. Provider
// descriptions and raw bodies never reach agent clients.
struct AgentFailedEvent final {
  AgentEventMetadata Metadata;
  Providers::FailureCode Code{Providers::FailureCode::ProviderRejected};
  bool Retryable{false};
};

struct AgentCancelledEvent final {
  AgentEventMetadata Metadata;
  std::string ReasonCode;
};

struct AgentOutcomeUnknownEvent final {
  AgentEventMetadata Metadata;
  std::string ReasonCode;
};

using AgentLifecycleEvent = std::variant<
    AgentStartedEvent, AgentTextDeltaEvent, AgentRefusalDeltaEvent,
    AgentToolCallStartedEvent, AgentToolArgumentDeltaEvent,
    AgentToolCallReadyEvent, AgentUsageUpdatedEvent, AgentFinalEvent,
    AgentRefusalEvent, AgentToolHandoffEvent, AgentOutputLimitedEvent,
    AgentFailedEvent, AgentCancelledEvent, AgentOutcomeUnknownEvent>;

enum class AgentProjectionErrorCode : std::uint8_t {
  WrongRequest,
  WrongTurn,
  NonMonotonicSequence,
  DuplicateConflict,
  EventAfterTerminal,
  SecondTerminal,
  InvalidEvent,
};

struct AgentProjectionError final {
  AgentProjectionErrorCode Code{AgentProjectionErrorCode::InvalidEvent};

  friend bool operator==(const AgentProjectionError&,
                         const AgentProjectionError&) = default;
};

// Incremental canonical-event projector. It validates request/turn/sequence
// correlation, projects only documented V1.1 public fields, ignores identical
// duplicate deliveries, and enforces exactly one terminal event.
class AgentEventProjector final {
public:
  using Sink = std::function<void(const AgentLifecycleEvent&)>;

  AgentEventProjector(Providers::RequestId ExpectedRequest,
                      Providers::AgentTurnId ExpectedTurn,
                      Sink EventSink);

  [[nodiscard]] std::optional<AgentProjectionError> Accept(
      const Providers::CanonicalNonTerminalEvent& Event);
  [[nodiscard]] std::optional<AgentProjectionError> Accept(
      const Providers::CanonicalTerminalEvent& Event);

  [[nodiscard]] bool HasTerminal() const noexcept { return TerminalSeen; }
  [[nodiscard]] std::uint64_t NextExpectedSequence() const noexcept {
    return NextSequence;
  }
  [[nodiscard]] std::size_t EmittedCount() const noexcept { return Emitted; }

private:
  [[nodiscard]] std::optional<AgentProjectionError> ValidateMetadata(
      const Providers::ProviderEventMetadata& Metadata) const noexcept;
  [[nodiscard]] std::optional<AgentProjectionError> Deliver(
      AgentLifecycleEvent Event);

  Providers::RequestId ExpectedRequest;
  Providers::AgentTurnId ExpectedTurn;
  Sink EventSink;
  std::uint64_t NextSequence{1};
  bool TerminalSeen{false};
  bool SequenceExhausted{false};
  std::size_t Emitted{0};
  std::string LastFingerprint;
};

} // namespace EmulativeDialog::Agent
