#pragma once

#include "Source/Channels/Contracts/ChannelAdapterPort.h"
#include "Source/Core/CoreAuthorizedAction.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace EmulativeDialog::Channels::Testing {

// A synthetic receipt is one-shot and keyed by the exact Core-issued action and
// operation IDs. The adapter copies only the configured status payload into the
// returned receipt after validating that the receipt itself is correctly paired.
struct MockActionReceipt {
  ActionId Action;
  OperationId Operation;
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
// SendAction returns only the receipt scripted for the exact input action IDs and
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
  const std::optional<ChannelReceipt>& GetLastSendActionReceipt() const noexcept;
  bool IsStarted() const noexcept;

private:
  bool HasValidMockConfiguration() const noexcept;
  bool MatchesProfile(const AccountProfile& Profile) const noexcept;
  bool IsValidInboundEvent(const InboundChannelMessage& Event) const noexcept;
  CapabilitySnapshot MakeUnknownCapabilitySnapshot(
      const AccountProfile& Profile) const;
  ChannelReceipt MakeUnscriptedFailureReceipt(
      const Core::CoreAuthorizedAction& Action,
      ChannelErrorCode ErrorCode) const;
  ChannelReceipt StoreSendActionReceipt(ChannelReceipt Receipt);

  MockChannelConfiguration Configuration;
  std::set<std::string> DeliveredDeduplicationIds;
  ChannelEventSink* ActiveSink = nullptr;
  std::size_t DeliveredEventCount = 0;
  std::size_t SendActionCallCount = 0;
  std::optional<ChannelReceipt> LastSendActionReceipt;
  bool Started = false;
};

} // namespace EmulativeDialog::Channels::Testing
