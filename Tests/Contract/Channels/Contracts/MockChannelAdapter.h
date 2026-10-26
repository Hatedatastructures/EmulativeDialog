#pragma once

#include "Source/Channels/Contracts/ChannelAdapterPort.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace EmulativeDialog::Core {

// Test-only stand-in exposing action correlation IDs. It has no authorization
// state or behavior and is not a product CoreAuthorizedAction definition.
struct CoreAuthorizedAction final {
  Channels::ActionId Action;
  Channels::OperationId Operation;
};

} // namespace EmulativeDialog::Core

namespace EmulativeDialog::Channels::Testing {

// A scripted receipt is associated with one exact CoreAuthorizedAction instance.
// The referenced action must outlive its queued result; the mock compares only its
// address and never reads or stores authorization fields.
struct MockActionReceipt {
  const Core::CoreAuthorizedAction* Action;
  ChannelReceipt Receipt;
};

// All values in this configuration are synthetic test data. The mock never
// interprets message text as identity or authorization evidence.
struct MockChannelConfiguration {
  AccountProfile Profile;
  CapabilitySnapshot Capabilities;
  std::vector<InboundChannelMessage> InboundEvents;
  std::vector<MockActionReceipt> SendActionResults;
};

// Deterministic, in-memory, offline ChannelAdapterPort implementation for tests.
// SendAction returns only the receipt scripted for the exact input action and
// never performs a retry or synthesizes a correlated success receipt.
class MockChannelAdapter final : public ChannelAdapterPort {
public:
  explicit MockChannelAdapter(MockChannelConfiguration Configuration);
  ~MockChannelAdapter() override = default;

  MockChannelAdapter(const MockChannelAdapter&) = delete;
  MockChannelAdapter& operator=(const MockChannelAdapter&) = delete;
  MockChannelAdapter(MockChannelAdapter&&) = delete;
  MockChannelAdapter& operator=(MockChannelAdapter&&) = delete;

  CapabilitySnapshot GetCapabilities(const AccountProfile& Profile) override;
  StartResult Start(const AccountProfile& Profile,
                    const Core::CredentialLease& Lease) override;
  StopResult Stop() override;
  SubscriptionResult SubscribeEvents(ChannelEventSink& Sink) override;
  ChannelReceipt SendAction(const Core::CoreAuthorizedAction& Action,
                            const Core::CredentialLease& Lease) override;
  HealthSnapshot GetHealthSnapshot() const override;

  std::size_t GetDeliveredEventCount() const noexcept;
  std::size_t GetSendActionCallCount() const noexcept;
  bool IsStarted() const noexcept;

private:
  bool HasValidMockConfiguration() const noexcept;
  bool MatchesProfile(const AccountProfile& Profile) const noexcept;
  bool IsValidInboundEvent(const InboundChannelMessage& Event) const noexcept;
  CapabilitySnapshot MakeUnknownCapabilitySnapshot(
      const AccountProfile& Profile) const;
  ChannelReceipt MakeUnscriptedFailureReceipt(ChannelErrorCode ErrorCode) const;

  MockChannelConfiguration Configuration;
  std::set<std::string> DeliveredDeduplicationIds;
  ChannelEventSink* ActiveSink = nullptr;
  std::size_t DeliveredEventCount = 0;
  std::size_t SendActionCallCount = 0;
  bool Started = false;
};

} // namespace EmulativeDialog::Channels::Testing
