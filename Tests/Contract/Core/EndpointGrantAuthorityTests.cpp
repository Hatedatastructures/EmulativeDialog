#include "Source/Core/EndpointGrantAuthority.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace EmulativeDialog::Core {

class EndpointGrantAuthorityTestAccess final {
public:
  using TimePoint = EndpointGrantAuthority::Clock::time_point;
  using EntropyFunction =
      std::function<bool(std::array<std::byte, 32>& Bytes)>;
  using NowFunction = std::function<TimePoint()>;

  static EndpointGrantAuthority Create(
      const std::vector<Providers::ModelProviderProfile>& Profiles,
      EntropyFunction Entropy, NowFunction Now) {
    return EndpointGrantAuthority{Profiles, std::move(Entropy), std::move(Now)};
  }
};

} // namespace EmulativeDialog::Core

namespace {

using namespace EmulativeDialog;
using Core::EndpointGrant;
using Core::EndpointGrantAuthority;
using Core::EndpointGrantAuthorityTestAccess;
using Core::EndpointGrantIssueFailure;
using Core::EndpointGrantRevokeResult;
namespace Network = EmulativeDialog::Platform::Network;
using Network::EndpointGrantId;
using Network::EndpointGrantStatus;
using Network::HttpsDestination;
using Providers::ModelProviderProfile;
using Providers::ProviderProfileId;

void Check(bool Condition, std::string_view Expression, int Line) {
  if (!Condition) {
    throw std::runtime_error("line " + std::to_string(Line) + ": " +
                             std::string{Expression});
  }
}

#define CHECK(Condition) Check((Condition), #Condition, __LINE__)

ModelProviderProfile MakeProfile(std::string Id = "profile-01",
                                 std::string Endpoint =
                                     "https://API.Example.com:443/v1/chat") {
  ModelProviderProfile Profile;
  Profile.Id = ProviderProfileId{.Value = std::move(Id)};
  Profile.Endpoint = std::move(Endpoint);
  Profile.ModelName = "offline-test-model";
  Profile.Credential.Value = "fixture-only-reference";
  return Profile;
}

std::vector<ModelProviderProfile> OneProfile(
    std::string Endpoint = "https://API.Example.com:443/v1/chat") {
  return {MakeProfile("profile-01", std::move(Endpoint))};
}

auto SequenceEntropy() {
  const auto Next = std::make_shared<std::atomic<std::uint64_t>>(1);
  return [Next](std::array<std::byte, 32>& Bytes) {
    Bytes.fill(std::byte{0});
    const std::uint64_t Value = Next->fetch_add(1, std::memory_order_relaxed);
    for (std::size_t Index = 0; Index < sizeof(Value); ++Index) {
      Bytes[Index] = static_cast<std::byte>((Value >> (Index * 8U)) & 0xffU);
    }
    return true;
  };
}

struct FakeClock final {
  using Duration = EndpointGrantAuthority::Clock::duration;
  std::shared_ptr<std::atomic<Duration::rep>> Ticks =
      std::make_shared<std::atomic<Duration::rep>>(0);

  [[nodiscard]] EndpointGrantAuthority::Clock::time_point Now() const {
    return EndpointGrantAuthority::Clock::time_point{
        Duration{Ticks->load(std::memory_order_relaxed)}};
  }

  void Advance(std::chrono::steady_clock::duration Offset) const {
    Ticks->fetch_add(Offset.count(), std::memory_order_relaxed);
  }

  [[nodiscard]] auto Function() const {
    const auto ClockTicks = Ticks;
    return [ClockTicks] {
      return EndpointGrantAuthority::Clock::time_point{
          Duration{ClockTicks->load(std::memory_order_relaxed)}};
    };
  }
};

EndpointGrantAuthority MakeAuthority(
    const std::vector<ModelProviderProfile>& Profiles, const FakeClock& Clock,
    EndpointGrantAuthorityTestAccess::EntropyFunction Entropy =
        SequenceEntropy()) {
  return EndpointGrantAuthorityTestAccess::Create(
      Profiles, std::move(Entropy), Clock.Function());
}

EndpointGrant Issue(EndpointGrantAuthority& Authority,
                    std::string_view Profile = "profile-01") {
  const auto Result = Authority.IssueForProfile(
      ProviderProfileId{.Value = std::string{Profile}});
  const auto* Grant = std::get_if<EndpointGrant>(&Result);
  CHECK(Grant != nullptr);
  return *Grant;
}

bool IsLowerHexToken(std::string_view Token) {
  return Token.size() == 64 &&
         std::all_of(Token.begin(), Token.end(), [](char Value) {
           return (Value >= '0' && Value <= '9') ||
                  (Value >= 'a' && Value <= 'f');
         });
}

void TestSnapshotIsCopiedAndCanonicalized() {
  FakeClock Clock;
  auto Profiles = OneProfile("HTTPS://API.Example.COM.:443/v1/chat?x=%7e");
  EndpointGrantAuthority Authority = MakeAuthority(Profiles, Clock);
  Profiles.front().Endpoint = "http://attacker.invalid/changed";

  const EndpointGrant Grant = Issue(Authority);
  CHECK(Grant.Destination.Host == "api.example.com");
  CHECK(Grant.Destination.Port == 443);
  CHECK(Grant.Destination.Path == "/v1/chat?x=~");
  CHECK(IsLowerHexToken(Grant.Id.Value));
  CHECK(Authority.Validate(Grant.Id, Grant.Destination) ==
        EndpointGrantStatus::Valid);
}

void TestEndpointParsingNormalizesDefaultsAndIpLiterals() {
  FakeClock Clock;
  EndpointGrantAuthority Dns = MakeAuthority(
      OneProfile("https://example.test?model=a%3ab"), Clock);
  const EndpointGrant DnsGrant = Issue(Dns);
  CHECK(DnsGrant.Destination.Host == "example.test");
  CHECK(DnsGrant.Destination.Port == 443);
  CHECK(DnsGrant.Destination.Path == "/?model=a%3ab");

  EndpointGrantAuthority Ipv4 = MakeAuthority(
      OneProfile("https://192.0.2.1:8443/v1"), Clock);
  const EndpointGrant Ipv4Grant = Issue(Ipv4);
  CHECK(Ipv4Grant.Destination.Host == "192.0.2.1");
  CHECK(Ipv4Grant.Destination.Port == 8443);
  CHECK(Ipv4Grant.Destination.Path == "/v1");

  EndpointGrantAuthority Ipv6 = MakeAuthority(
      OneProfile("https://[2001:0DB8:0:0::1]:443/v1"), Clock);
  const EndpointGrant Ipv6Grant = Issue(Ipv6);
  CHECK(Ipv6Grant.Destination.Host == "2001:db8::1");
  CHECK(Ipv6Grant.Destination.Port == 443);

  EndpointGrantAuthority QueryOnly = MakeAuthority(
      OneProfile("https://example.test?"), Clock);
  CHECK(Issue(QueryOnly).Destination.Path == "/?");

  EndpointGrantAuthority RootPath = MakeAuthority(
      OneProfile("https://example.test"), Clock);
  CHECK(Issue(RootPath).Destination.Path == "/");
}

void TestInvalidSnapshotsRejectAtomically() {
  const std::vector<std::string> InvalidEndpoints{
      "",
      "http://example.test/path",
      "https://user@example.test/path",
      "https://example.test/path#fragment",
      "https://example.test/%GG",
      "https://example.test:0/path",
      "https://example.test:65536/path",
      "https://example.test:/path",
      "https://999.2.3.4/path",
      "https://[2001:db8::zz]/path",
      "https://2001:db8::1/path",
      "https://example.test/a/../b",
      "https://example.test/a%2fb",
      "https://example.test../path",
      "https://bad_host.test/path",
      "https://例子.测试/path",
      "https://example.test/a b",
      std::string{"https://example.test/a\r\n"},
      "https://example.test/a\\b",
  };

  for (const std::string& Endpoint : InvalidEndpoints) {
    FakeClock Clock;
    bool Rejected = false;
    try {
      (void)MakeAuthority(OneProfile(Endpoint), Clock);
    } catch (const std::invalid_argument&) {
      Rejected = true;
    }
    CHECK(Rejected);
  }

  FakeClock Clock;
  auto EmptyIdentity = OneProfile();
  EmptyIdentity.front().Id.Value.clear();
  bool EmptyRejected = false;
  try {
    (void)MakeAuthority(EmptyIdentity, Clock);
  } catch (const std::invalid_argument&) {
    EmptyRejected = true;
  }
  CHECK(EmptyRejected);

  auto Duplicate = OneProfile();
  Duplicate.push_back(MakeProfile("profile-01", "https://other.test"));
  bool DuplicateRejected = false;
  try {
    (void)MakeAuthority(Duplicate, Clock);
  } catch (const std::invalid_argument&) {
    DuplicateRejected = true;
  }
  CHECK(DuplicateRejected);

  const std::vector<ModelProviderProfile> EmptySnapshot;
  EndpointGrantAuthority EmptyAuthority = MakeAuthority(EmptySnapshot, Clock);
  const auto EmptyIssue = EmptyAuthority.IssueForProfile(
      ProviderProfileId{.Value = "not-registered"});
  CHECK(std::get<EndpointGrantIssueFailure>(EmptyIssue) ==
        EndpointGrantIssueFailure::UnknownProfile);
}

void TestWindowsCngIssuesAProductionToken() {
  const std::vector<ModelProviderProfile> Profiles = OneProfile();
  EndpointGrantAuthority Authority{Profiles};
  const EndpointGrant Grant = Issue(Authority);
  CHECK(IsLowerHexToken(Grant.Id.Value));
  CHECK(Authority.Validate(Grant.Id, Grant.Destination) ==
        EndpointGrantStatus::Valid);
}

void TestUnknownProfileAndUnknownGrantFailClosed() {
  FakeClock Clock;
  EndpointGrantAuthority Authority = MakeAuthority(OneProfile(), Clock);
  const auto Empty = Authority.IssueForProfile(ProviderProfileId{});
  CHECK(std::get<EndpointGrantIssueFailure>(Empty) ==
        EndpointGrantIssueFailure::UnknownProfile);
  const auto Unknown = Authority.IssueForProfile(
      ProviderProfileId{.Value = "unregistered"});
  CHECK(std::get<EndpointGrantIssueFailure>(Unknown) ==
        EndpointGrantIssueFailure::UnknownProfile);
  CHECK(Authority.Validate(EndpointGrantId{.Value = "short"},
                           HttpsDestination{.Host = "api.example.com",
                                            .Port = 443,
                                            .Path = "/v1/chat"}) ==
        EndpointGrantStatus::Missing);
  CHECK(Authority.Validate(EndpointGrantId{.Value = std::string(64, '0')},
                           HttpsDestination{.Host = "api.example.com",
                                            .Port = 443,
                                            .Path = "/v1/chat"}) ==
        EndpointGrantStatus::Missing);
}

void TestDestinationBindingChecksEveryField() {
  FakeClock Clock;
  EndpointGrantAuthority Authority = MakeAuthority(OneProfile(), Clock);
  const EndpointGrant Grant = Issue(Authority);
  HttpsDestination Changed = Grant.Destination;
  Changed.Host = "other.example.com";
  CHECK(Authority.Validate(Grant.Id, Changed) ==
        EndpointGrantStatus::DestinationMismatch);
  Changed = Grant.Destination;
  Changed.Port = 444;
  CHECK(Authority.Validate(Grant.Id, Changed) ==
        EndpointGrantStatus::DestinationMismatch);
  Changed = Grant.Destination;
  Changed.Path = "/v1/other";
  CHECK(Authority.Validate(Grant.Id, Changed) ==
        EndpointGrantStatus::DestinationMismatch);
}

void TestFiveMinuteSteadyClockExpiryBoundary() {
  FakeClock Clock;
  EndpointGrantAuthority Authority = MakeAuthority(OneProfile(), Clock);
  const EndpointGrant Grant = Issue(Authority);
  Clock.Advance(std::chrono::minutes{5} - std::chrono::nanoseconds{1});
  CHECK(Authority.Validate(Grant.Id, Grant.Destination) ==
        EndpointGrantStatus::Valid);
  Clock.Advance(std::chrono::nanoseconds{1});
  CHECK(Authority.Validate(Grant.Id, Grant.Destination) ==
        EndpointGrantStatus::Expired);
  Clock.Advance(std::chrono::nanoseconds{1});
  CHECK(Authority.Validate(Grant.Id, Grant.Destination) ==
        EndpointGrantStatus::Expired);
}

void TestRevocationInvalidatesAndPreventsReissuance() {
  FakeClock Clock;
  EndpointGrantAuthority Authority = MakeAuthority(OneProfile(), Clock);
  const EndpointGrant First = Issue(Authority);
  CHECK(Authority.RevokeProfile(ProviderProfileId{.Value = "profile-01"}) ==
        EndpointGrantRevokeResult::Revoked);
  CHECK(Authority.Validate(First.Id, First.Destination) ==
        EndpointGrantStatus::Missing);
  CHECK(Authority.RevokeProfile(ProviderProfileId{.Value = "profile-01"}) ==
        EndpointGrantRevokeResult::AlreadyRevoked);
  CHECK(Authority.RevokeProfile(ProviderProfileId{.Value = "unknown"}) ==
        EndpointGrantRevokeResult::UnknownProfile);
  const auto AfterRevoke = Authority.IssueForProfile(
      ProviderProfileId{.Value = "profile-01"});
  CHECK(std::get<EndpointGrantIssueFailure>(AfterRevoke) ==
        EndpointGrantIssueFailure::RevokedProfile);

  EndpointGrantAuthority Replacement = MakeAuthority(OneProfile(), Clock);
  CHECK(Replacement.Validate(First.Id, First.Destination) ==
        EndpointGrantStatus::Missing);
}

void TestEntropyFailureAndCollisionDoNotInsertOrOverwrite() {
  FakeClock Clock;
  auto FailedEntropy = [](std::array<std::byte, 32>& Bytes) {
    Bytes.fill(std::byte{0x7f});
    return false;
  };
  EndpointGrantAuthority Failing =
      MakeAuthority(OneProfile(), Clock, FailedEntropy);
  const auto FailedIssue = Failing.IssueForProfile(
      ProviderProfileId{.Value = "profile-01"});
  CHECK(std::get<EndpointGrantIssueFailure>(FailedIssue) ==
        EndpointGrantIssueFailure::EntropyUnavailable);
  CHECK(Failing.Validate(EndpointGrantId{.Value = std::string(64, '7')},
                         HttpsDestination{.Host = "api.example.com",
                                          .Port = 443,
                                          .Path = "/v1/chat"}) ==
        EndpointGrantStatus::Missing);

  auto SameEntropy = [](std::array<std::byte, 32>& Bytes) {
    Bytes.fill(std::byte{0x2a});
    return true;
  };
  EndpointGrantAuthority Colliding =
      MakeAuthority(OneProfile(), Clock, SameEntropy);
  const EndpointGrant First = Issue(Colliding);
  const auto Collision = Colliding.IssueForProfile(
      ProviderProfileId{.Value = "profile-01"});
  CHECK(std::get<EndpointGrantIssueFailure>(Collision) ==
        EndpointGrantIssueFailure::Collision);
  CHECK(Colliding.Validate(First.Id, First.Destination) ==
        EndpointGrantStatus::Valid);
}

void TestSourceExceptionsFailClosed() {
  auto ThrowingEntropy = [](std::array<std::byte, 32>&) -> bool {
    throw std::runtime_error("fixture entropy error");
  };
  FakeClock Clock;
  EndpointGrantAuthority EntropyFailure =
      MakeAuthority(OneProfile(), Clock, ThrowingEntropy);
  const auto IssueFailure = EntropyFailure.IssueForProfile(
      ProviderProfileId{.Value = "profile-01"});
  CHECK(std::get<EndpointGrantIssueFailure>(IssueFailure) ==
        EndpointGrantIssueFailure::InternalError);

  EndpointGrantAuthority ThrowingClock = EndpointGrantAuthorityTestAccess::Create(
      OneProfile(), SequenceEntropy(), []() -> EndpointGrantAuthority::Clock::time_point {
        throw std::runtime_error("fixture clock error");
      });
  const auto ClockIssue = ThrowingClock.IssueForProfile(
      ProviderProfileId{.Value = "profile-01"});
  CHECK(std::get<EndpointGrantIssueFailure>(ClockIssue) ==
        EndpointGrantIssueFailure::InternalError);
  CHECK(ThrowingClock.Validate(EndpointGrantId{.Value = std::string(64, '0')},
                               HttpsDestination{}) == EndpointGrantStatus::Missing);

  EndpointGrantAuthority OverflowClock = EndpointGrantAuthorityTestAccess::Create(
      OneProfile(), SequenceEntropy(), [] {
        return EndpointGrantAuthority::Clock::time_point::max() -
               std::chrono::minutes{1};
      });
  const auto OverflowIssue = OverflowClock.IssueForProfile(
      ProviderProfileId{.Value = "profile-01"});
  CHECK(std::get<EndpointGrantIssueFailure>(OverflowIssue) ==
        EndpointGrantIssueFailure::InternalError);
  const EndpointGrantId FirstDeterministicId{
      .Value = "0100000000000000" + std::string(48, '0')};
  CHECK(OverflowClock.Validate(FirstDeterministicId, HttpsDestination{}) ==
        EndpointGrantStatus::Missing);
}

enum class StreamStage : std::uint8_t {
  Validate,
  DoStream,
  Head,
  Chunk,
};

struct StreamTrace final {
  std::array<StreamStage, 8> Stages{};
  std::size_t Count{};

  void Record(StreamStage Stage) noexcept {
    if (Count < Stages.size()) {
      Stages[Count++] = Stage;
    }
  }
};

class CountingGrantResolver final : public Network::EndpointGrantResolver {
public:
  CountingGrantResolver(EndpointGrantAuthority& Authority,
                        StreamTrace& Trace)
      : Authority(Authority), Trace(Trace),
        ProfileId{.Value = "profile-01"} {}

  [[nodiscard]] EndpointGrantStatus Validate(
      const EndpointGrantId& Grant,
      const HttpsDestination& Requested) const noexcept override {
    ++ValidateCalls;
    Trace.Record(StreamStage::Validate);
    const EndpointGrantStatus Status = Authority.Validate(Grant, Requested);
    if (RevokeAfterValidation) {
      PostValidationRevokeResult = Authority.RevokeProfile(ProfileId);
      RevokeAfterValidation = false;
    }
    return Status;
  }

  mutable std::size_t ValidateCalls{};
  mutable bool RevokeAfterValidation{};
  mutable EndpointGrantRevokeResult PostValidationRevokeResult{
      EndpointGrantRevokeResult::UnknownProfile};

private:
  EndpointGrantAuthority& Authority;
  StreamTrace& Trace;
  const ProviderProfileId ProfileId;
};

class GuardedFakeTransport final : public Network::HttpTransportPort {
public:
  GuardedFakeTransport(const Network::EndpointGrantResolver& Resolver,
                       StreamTrace& Trace)
      : HttpTransportPort(Resolver), Trace(Trace) {}

  [[nodiscard]] Network::TransportHealthState Health(
      std::stop_token) noexcept override {
    return Network::TransportHealthState::Ready;
  }

  int DoCalls{};

protected:
  [[nodiscard]] Network::TransportTerminal DoStream(
      const Network::TransportRequest&,
      const Network::ResponseHeadSink& OnHead,
      const Network::ResponseChunkSink& OnChunk,
      std::stop_token) override {
    ++DoCalls;
    Trace.Record(StreamStage::DoStream);
    if (OnHead) {
      OnHead(Network::ResponseHeadMetadata{.StatusCode = 200,
                                           .ContentType = "application/json",
                                           .RetryAfter = std::nullopt});
    }
    if (OnChunk) {
      constexpr std::array<std::byte, 2> Chunk{std::byte{'o'}, std::byte{'k'}};
      OnChunk(Chunk);
    }
    return Network::CompletedTransport{.StatusCode = 200,
                                       .ReceivedBodyBytes = 2};
  }

private:
  StreamTrace& Trace;
};

void TestGuardedStreamRejectsBeforeAnyTransportOrSinkCall() {
  auto AssertDenied = [&](EndpointGrantAuthority& Authority,
                          EndpointGrantId GrantId,
                          HttpsDestination Destination) {
    StreamTrace Trace;
    CountingGrantResolver Resolver{Authority, Trace};
    GuardedFakeTransport Transport{Resolver, Trace};
    int HeadCalls = 0;
    int ChunkCalls = 0;
    Network::TransportRequest Request;
    Request.Grant = std::move(GrantId);
    Request.Destination = std::move(Destination);
    const Network::TransportTerminal Result = Transport.Stream(
        Request,
        [&](const Network::ResponseHeadMetadata&) {
          Trace.Record(StreamStage::Head);
          ++HeadCalls;
        },
        [&](std::span<const std::byte>) {
          Trace.Record(StreamStage::Chunk);
          ++ChunkCalls;
        }, {});
    const auto* Failure = std::get_if<Network::FailedTransport>(&Result);
    CHECK(Failure != nullptr);
    CHECK(Failure->Code == Network::TransportFailureCode::EndpointNotGranted);
    CHECK(Resolver.ValidateCalls == 1);
    CHECK(Transport.DoCalls == 0);
    CHECK(HeadCalls == 0);
    CHECK(ChunkCalls == 0);
    CHECK(Trace.Count == 1);
    CHECK(Trace.Stages[0] == StreamStage::Validate);
  };

  FakeClock Clock;
  EndpointGrantAuthority Authority = MakeAuthority(OneProfile(), Clock);
  const HttpsDestination Expected{.Host = "api.example.com",
                                  .Port = 443,
                                  .Path = "/v1/chat"};
  AssertDenied(Authority, EndpointGrantId{.Value = "missing"}, Expected);
  const EndpointGrant Expired = Issue(Authority);
  Clock.Advance(std::chrono::minutes{5});
  AssertDenied(Authority, Expired.Id, Expired.Destination);

  FakeClock RevokeClock;
  EndpointGrantAuthority RevokeAuthority = MakeAuthority(OneProfile(), RevokeClock);
  const EndpointGrant Revoked = Issue(RevokeAuthority);
  CHECK(RevokeAuthority.RevokeProfile(
            ProviderProfileId{.Value = "profile-01"}) ==
        EndpointGrantRevokeResult::Revoked);
  AssertDenied(RevokeAuthority, Revoked.Id, Revoked.Destination);

  FakeClock MismatchClock;
  EndpointGrantAuthority MismatchAuthority =
      MakeAuthority(OneProfile(), MismatchClock);
  const EndpointGrant Bound = Issue(MismatchAuthority);
  HttpsDestination Wrong = Bound.Destination;
  Wrong.Path = "/mutated";
  AssertDenied(MismatchAuthority, Bound.Id, Wrong);

  FakeClock ValidClock;
  EndpointGrantAuthority ValidAuthority = MakeAuthority(OneProfile(), ValidClock);
  const EndpointGrant ValidGrant = Issue(ValidAuthority);
  StreamTrace ValidTrace;
  CountingGrantResolver ValidResolver{ValidAuthority, ValidTrace};
  GuardedFakeTransport ValidTransport{ValidResolver, ValidTrace};
  Network::TransportRequest ValidRequest;
  ValidRequest.Grant = ValidGrant.Id;
  ValidRequest.Destination = ValidGrant.Destination;
  int HeadCalls = 0;
  int ChunkCalls = 0;
  const auto ValidResult = ValidTransport.Stream(
      ValidRequest,
      [&](const Network::ResponseHeadMetadata&) {
        ValidTrace.Record(StreamStage::Head);
        ++HeadCalls;
      },
      [&](std::span<const std::byte> Chunk) {
        CHECK(Chunk.size() == 2);
        ValidTrace.Record(StreamStage::Chunk);
        ++ChunkCalls;
      }, {});
  CHECK(std::holds_alternative<Network::CompletedTransport>(ValidResult));
  CHECK(ValidResolver.ValidateCalls == 1);
  CHECK(ValidTransport.DoCalls == 1);
  CHECK(HeadCalls == 1);
  CHECK(ChunkCalls == 1);
  CHECK(ValidTrace.Count == 4);
  CHECK(ValidTrace.Stages[0] == StreamStage::Validate);
  CHECK(ValidTrace.Stages[1] == StreamStage::DoStream);
  CHECK(ValidTrace.Stages[2] == StreamStage::Head);
  CHECK(ValidTrace.Stages[3] == StreamStage::Chunk);
}

void TestRevocationAfterValidationDoesNotCancelInflightStream() {
  FakeClock Clock;
  EndpointGrantAuthority Authority = MakeAuthority(OneProfile(), Clock);
  const EndpointGrant Grant = Issue(Authority);
  StreamTrace Trace;
  CountingGrantResolver Resolver{Authority, Trace};
  Resolver.RevokeAfterValidation = true;
  GuardedFakeTransport Transport{Resolver, Trace};
  Network::TransportRequest Request;
  Request.Grant = Grant.Id;
  Request.Destination = Grant.Destination;
  int HeadCalls = 0;
  int ChunkCalls = 0;

  const auto Result = Transport.Stream(
      Request,
      [&](const Network::ResponseHeadMetadata&) {
        Trace.Record(StreamStage::Head);
        ++HeadCalls;
      },
      [&](std::span<const std::byte>) {
        Trace.Record(StreamStage::Chunk);
        ++ChunkCalls;
      }, {});

  CHECK(std::holds_alternative<Network::CompletedTransport>(Result));
  CHECK(Resolver.ValidateCalls == 1);
  CHECK(Resolver.PostValidationRevokeResult ==
        EndpointGrantRevokeResult::Revoked);
  CHECK(Transport.DoCalls == 1);
  CHECK(HeadCalls == 1);
  CHECK(ChunkCalls == 1);
  CHECK(Trace.Count == 4);
  CHECK(Trace.Stages[0] == StreamStage::Validate);
  CHECK(Trace.Stages[1] == StreamStage::DoStream);
  CHECK(Authority.Validate(Grant.Id, Grant.Destination) ==
        EndpointGrantStatus::Missing);
}

void TestConcurrentIssuanceIsUniqueAndRevokeFailsClosed() {
  FakeClock Clock;
  EndpointGrantAuthority Authority = MakeAuthority(OneProfile(), Clock);
  std::mutex IdMutex;
  std::vector<EndpointGrantId> Issued;
  std::atomic<bool> Start{false};
  std::vector<std::thread> Workers;
  for (int Worker = 0; Worker < 8; ++Worker) {
    Workers.emplace_back([&] {
      while (!Start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int Index = 0; Index < 12; ++Index) {
        const auto Result = Authority.IssueForProfile(
            ProviderProfileId{.Value = "profile-01"});
        if (const auto* Grant = std::get_if<EndpointGrant>(&Result)) {
          const std::lock_guard Lock{IdMutex};
          Issued.push_back(Grant->Id);
        }
      }
    });
  }
  Start.store(true, std::memory_order_release);
  for (std::thread& Worker : Workers) {
    Worker.join();
  }
  CHECK(Issued.size() == 96);
  std::unordered_set<std::string> Unique;
  for (const EndpointGrantId& Id : Issued) {
    CHECK(Unique.insert(Id.Value).second);
    CHECK(Authority.Validate(
              Id, HttpsDestination{.Host = "api.example.com",
                                   .Port = 443,
                                   .Path = "/v1/chat"}) ==
          EndpointGrantStatus::Valid);
  }

  constexpr int ValidatorCount = 4;
  constexpr int ValidationRounds = 8;
  std::barrier StartGate{ValidatorCount + 1};
  std::atomic<int> ValidResults{0};
  std::atomic<int> MissingResults{0};
  std::atomic<bool> UnexpectedResult{false};
  std::vector<std::thread> ConcurrentValidators;
  for (int Worker = 0; Worker < ValidatorCount; ++Worker) {
    ConcurrentValidators.emplace_back([&] {
      StartGate.arrive_and_wait();
      for (int Round = 0; Round < ValidationRounds; ++Round) {
        for (const EndpointGrantId& Id : Issued) {
          const EndpointGrantStatus Status = Authority.Validate(
              Id, HttpsDestination{.Host = "api.example.com",
                                   .Port = 443,
                                   .Path = "/v1/chat"});
          if (Status == EndpointGrantStatus::Valid) {
            ValidResults.fetch_add(1, std::memory_order_relaxed);
          } else if (Status == EndpointGrantStatus::Missing) {
            MissingResults.fetch_add(1, std::memory_order_relaxed);
          } else {
            UnexpectedResult.store(true, std::memory_order_relaxed);
          }
        }
      }
    });
  }
  EndpointGrantRevokeResult ConcurrentRevokeResult =
      EndpointGrantRevokeResult::UnknownProfile;
  std::thread Revoker([&] {
    StartGate.arrive_and_wait();
    ConcurrentRevokeResult =
        Authority.RevokeProfile(ProviderProfileId{.Value = "profile-01"});
  });
  for (std::thread& Worker : ConcurrentValidators) {
    Worker.join();
  }
  Revoker.join();
  CHECK(ConcurrentRevokeResult == EndpointGrantRevokeResult::Revoked);
  CHECK(!UnexpectedResult.load(std::memory_order_relaxed));
  CHECK(ValidResults.load(std::memory_order_relaxed) +
            MissingResults.load(std::memory_order_relaxed) ==
        ValidatorCount * ValidationRounds * static_cast<int>(Issued.size()));

  for (const EndpointGrantId& Id : Issued) {
    CHECK(Authority.Validate(
              Id, HttpsDestination{.Host = "api.example.com",
                                   .Port = 443,
                                   .Path = "/v1/chat"}) ==
          EndpointGrantStatus::Missing);
  }
  const auto NewIssue = Authority.IssueForProfile(
      ProviderProfileId{.Value = "profile-01"});
  CHECK(std::get<EndpointGrantIssueFailure>(NewIssue) ==
        EndpointGrantIssueFailure::RevokedProfile);
}

} // namespace

int main() {
  const std::vector<std::pair<std::string_view, void (*)()>> Tests{
      {"snapshot canonicalization", TestSnapshotIsCopiedAndCanonicalized},
      {"endpoint parsing", TestEndpointParsingNormalizesDefaultsAndIpLiterals},
      {"invalid profile snapshots", TestInvalidSnapshotsRejectAtomically},
      {"production CNG issuance", TestWindowsCngIssuesAProductionToken},
      {"unknown profile and grant", TestUnknownProfileAndUnknownGrantFailClosed},
      {"destination binding", TestDestinationBindingChecksEveryField},
      {"five minute expiry", TestFiveMinuteSteadyClockExpiryBoundary},
      {"revocation", TestRevocationInvalidatesAndPreventsReissuance},
      {"entropy and collision", TestEntropyFailureAndCollisionDoNotInsertOrOverwrite},
      {"source exceptions", TestSourceExceptionsFailClosed},
      {"guarded transport ordering", TestGuardedStreamRejectsBeforeAnyTransportOrSinkCall},
      {"inflight revoke semantics", TestRevocationAfterValidationDoesNotCancelInflightStream},
      {"concurrent issuance", TestConcurrentIssuanceIsUniqueAndRevokeFailsClosed},
  };

  int Failures = 0;
  for (const auto& [Name, Test] : Tests) {
    try {
      Test();
      std::cout << "PASS " << Name << '\n';
    } catch (const std::exception& Error) {
      ++Failures;
      std::cerr << "FAIL " << Name << ": " << Error.what() << '\n';
    }
  }
  std::cout << "EndpointGrantAuthorityTests "
            << (Tests.size() - static_cast<std::size_t>(Failures)) << "/"
            << Tests.size() << " passed\n";
  return Failures == 0 ? 0 : 1;
}
