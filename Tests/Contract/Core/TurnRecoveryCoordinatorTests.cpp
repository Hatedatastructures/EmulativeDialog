#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "Source/Core/TurnRecoveryCoordinator.hpp"

namespace {

using EmulativeDialog::Core::RecoveryDecision;
using EmulativeDialog::Core::RecoveryDisposition;
using EmulativeDialog::Core::RecoveryError;
using EmulativeDialog::Core::RecoveryErrorCode;
using EmulativeDialog::Core::RecoveryEvent;
using EmulativeDialog::Core::RecoveryReport;
using EmulativeDialog::Core::TurnRecoveryCoordinator;
using namespace EmulativeDialog::Storage;

void Expect(bool Condition, const char* File = __builtin_FILE(),
            int Line = __builtin_LINE()) {
  if (!Condition) {
    std::fprintf(stderr, "EXPECT failed at %s:%d\n", File, Line);
    std::abort();
  }
}

std::atomic<std::uint64_t> PathSequence{1};

std::filesystem::path TempPath(std::string_view Name) {
  const auto Stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("emulative-dialog-recovery-" + std::string(Name) + "-" +
          std::to_string(Stamp) + "-" +
          std::to_string(PathSequence.fetch_add(1)) + ".edcp");
}

void RemoveIfExists(const std::filesystem::path& Path) {
  std::error_code Error;
  std::filesystem::remove_all(Path, Error);
}

void WriteAll(const std::filesystem::path& Path, std::string_view Text) {
  std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
  Expect(Output.good());
  Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
  Output.flush();
  Expect(Output.good());
}

std::uint64_t Fnv1a(std::string_view Text) {
  std::uint64_t Value = 14695981039346656037ULL;
  for (const char Character : Text) {
    Value ^= static_cast<unsigned char>(Character);
    Value *= 1099511628211ULL;
  }
  return Value;
}

std::string HexHash(std::uint64_t Value) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Text(16, '0');
  for (std::size_t Index = 0; Index < Text.size(); ++Index) {
    const std::size_t Shift = (Text.size() - 1U - Index) * 4U;
    Text[Index] = Digits[(Value >> Shift) & 0x0FU];
  }
  return Text;
}

std::string OpaqueToken(std::string_view Prefix, std::string_view Seed) {
  std::string Material(Prefix);
  Material.append(Seed);
  const std::string First = HexHash(Fnv1a(Material));
  Material.append("|second");
  return std::string(Prefix) + First + HexHash(Fnv1a(Material));
}

std::string TurnToken(std::string_view Seed) {
  return OpaqueToken("turn_", Seed);
}

std::string RequestToken(std::string_view Seed) {
  return OpaqueToken("request_", Seed);
}

std::string ScopeToken(std::string_view Seed) {
  return OpaqueToken("scope_", Seed);
}

std::string ContextToken(std::string_view Seed) {
  return OpaqueToken("context_", Seed);
}

std::string OperationToken(std::string_view Seed) {
  return OpaqueToken("operation_", Seed);
}

std::string IdempotencyToken(std::string_view Seed) {
  return OpaqueToken("idem_", Seed);
}

std::string ActionToken(std::string_view Seed) {
  return OpaqueToken("action_", Seed);
}

std::string CompleteRecord(std::string Payload) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Checksum(16, '0');
  const std::uint64_t Value = Fnv1a(Payload);
  for (std::size_t Index = 0; Index < Checksum.size(); ++Index) {
    const std::size_t Shift = (Checksum.size() - 1U - Index) * 4U;
    Checksum[Index] = Digits[(Value >> Shift) & 0x0FU];
  }
  Payload.append("|").append(Checksum).append("\n");
  return Payload;
}

std::string PendingDispatchRecord(std::string_view Phase,
                                  bool OperationDispatch) {
  std::string Payload("EDCP|2|T|");
  Payload.append(TurnToken("turn-terminal-pending"))
      .append("|")
      .append(RequestToken("request-pending"))
      .append("|")
      .append(ScopeToken("scope-pending"))
      .append("|")
      .append(ContextToken("context-pending"))
      .append("|");
  Payload.append(Phase).append("|");
  if (OperationDispatch) {
    Payload.append("NotRequested|0|Dispatched|")
        .append(OperationToken("operation-pending"))
        .append("|")
        .append(IdempotencyToken("idempotency-pending"))
        .append("|")
        .append(ActionToken("action-pending"))
        .append("|");
  } else {
    Payload.append("Dispatched|0|None||||");
  }
  Payload.append("1700000000000|1");
  return CompleteRecord(std::move(Payload));
}

std::string InvalidOpaqueReferenceRecord(std::string_view TurnId,
                                         std::string_view RequestId,
                                         std::string_view ScopeRef,
                                         std::string_view ContextReference,
                                         std::string_view Phase) {
  std::string Payload("EDCP|2|T|");
  Payload.append(TurnId)
      .append("|")
      .append(RequestId)
      .append("|")
      .append(ScopeRef)
      .append("|")
      .append(ContextReference)
      .append("|")
      .append(Phase)
      .append("|NotRequested|0|None||||1700000000000|1");
  return CompleteRecord(std::move(Payload));
}

TurnCheckpoint MakeTurn(std::string TurnId, std::string ScopeRef,
                        TurnPhase Phase = TurnPhase::Created) {
  return TurnCheckpoint{
      .TurnId = TurnToken(TurnId),
      .RequestId = Phase >= TurnPhase::ProviderDispatched
                       ? RequestToken("request-a")
                       : "",
      .ScopeRef = ScopeToken(ScopeRef),
      .ContextReference = ContextToken("context-a"),
      .Phase = Phase,
      .ToolState = ToolExecutionState::NotRequested,
      .Operation = OperationState::None,
      .OperationId = {},
      .IdempotencyKey = {},
      .ActionId = {},
      .UnknownSideEffect = false,
      .UpdatedAt =
          std::chrono::system_clock::time_point{
              std::chrono::milliseconds{1'700'000'000'000LL}},
      .Revision = 1,
      .Version = 1,
  };
}

TurnCheckpoint ReachProviderCompleted(TurnCheckpointStore& Store,
                                      std::string TurnId, std::string ScopeRef,
                                      ToolExecutionState ToolState) {
  TurnCheckpoint Checkpoint = MakeTurn(std::move(TurnId), std::move(ScopeRef));
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ReadyToDispatch;
  Checkpoint.Revision = 2;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderDispatched;
  Checkpoint.RequestId = RequestToken("request-a");
  Checkpoint.Revision = 3;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderStreaming;
  Checkpoint.Revision = 4;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderCompleted;
  Checkpoint.Revision = 5;
  Expect(Store.Append(Checkpoint).index() == 0);

  if (ToolState == ToolExecutionState::NotRequested) {
    return Checkpoint;
  }

  Checkpoint.Operation = OperationState::Prepared;
  Checkpoint.OperationId = OperationToken("operation-recovery-tool");
  Checkpoint.IdempotencyKey = IdempotencyToken("idem-recovery-tool");
  Checkpoint.ActionId = ActionToken("action-recovery-tool");
  Checkpoint.ToolState = ToolExecutionState::Authorized;
  Checkpoint.Revision = 6;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Operation = OperationState::Authorized;
  Checkpoint.Revision = 7;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ToolDispatched;
  Checkpoint.ToolState = ToolExecutionState::Dispatched;
  Checkpoint.Operation = OperationState::Dispatched;
  Checkpoint.Revision = 8;
  Expect(Store.Append(Checkpoint).index() == 0);
  if (ToolState == ToolExecutionState::Dispatched) {
    return Checkpoint;
  }

  Checkpoint.ToolState = ToolExecutionState::Completed;
  Checkpoint.Operation = OperationState::Succeeded;
  Checkpoint.Revision = 9;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderDispatched;
  Checkpoint.RequestId = RequestToken("request-b");
  Checkpoint.Revision = 10;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderStreaming;
  Checkpoint.Revision = 11;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderCompleted;
  Checkpoint.Revision = 12;
  Expect(Store.Append(Checkpoint).index() == 0);
  return Checkpoint;
}

RecoveryReport ExpectReport(
    const std::variant<RecoveryReport, RecoveryError>& Result) {
  if (const auto* Error = std::get_if<RecoveryError>(&Result)) {
    std::fprintf(stderr, "RECOVERY failed: code=%u description=%s\n",
                 static_cast<unsigned>(Error->Code),
                 Error->SanitizedDescription.c_str());
  }
  Expect(std::holds_alternative<RecoveryReport>(Result));
  return std::get<RecoveryReport>(Result);
}

RecoveryError ExpectError(
    const std::variant<RecoveryReport, RecoveryError>& Result) {
  Expect(std::holds_alternative<RecoveryError>(Result));
  return std::get<RecoveryError>(Result);
}

CheckpointSnapshot Snapshot(
    const CheckpointResult<CheckpointSnapshot>& Loaded) {
  Expect(std::holds_alternative<CheckpointSnapshot>(Loaded));
  return std::get<CheckpointSnapshot>(Loaded);
}

void TestPreDispatchTurnResumesWithoutDispatch() {
  const auto Path = TempPath("safe");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  Expect(Store.Append(MakeTurn("turn-safe", "scope-safe")).index() == 0);

  std::vector<RecoveryEvent> Events;
  TurnRecoveryCoordinator Coordinator(
      Store,
      [&Events](const RecoveryEvent& Event) { Events.push_back(Event); });
  const auto Result = Coordinator.Recover(ScopeToken("scope-safe"));
  const RecoveryReport Report = ExpectReport(Result);
  Expect(Report.ScopeState == ScopeState::Active);
  Expect(Report.Decisions.size() == 1);
  Expect(Report.Decisions[0].TurnId == TurnToken("turn-safe"));
  Expect(Report.Decisions[0].ContextReference == ContextToken("context-a"));
  Expect(Report.Decisions[0].Disposition == RecoveryDisposition::SafeToResume);
  Expect(Report.Decisions[0].ReplayAllowed);
  Expect(Report.CheckpointsPersisted == 0);
  Expect(Report.DispatchCount == 0);
  Expect(Snapshot(Store.Load()).RecordCount == 1);

  Expect(Events.size() == 1);
  Expect(Events[0].SequenceNumber == 1);
  Expect(Events[0].ContextReference == ContextToken("context-a"));
  Expect(Events[0].StableCode == "SafeToResume");
  Expect(Events[0].ReplayAllowed);

  TurnRecoveryCoordinator Restarted(Store);
  const RecoveryReport Second =
      ExpectReport(Restarted.Recover(ScopeToken("scope-safe")));
  Expect(Second.Decisions.size() == 1);
  Expect(Second.Decisions[0].Disposition == RecoveryDisposition::SafeToResume);
  Expect(Snapshot(Store.Load()).RecordCount == 1);
  RemoveIfExists(Path);
}

void TestDispatchedProviderIsIsolatedOnce() {
  const auto Path = TempPath("provider-dispatched");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Created = MakeTurn("turn-provider", "scope-a");
  Expect(Store.Append(Created).index() == 0);
  TurnCheckpoint Ready = Created;
  Ready.Phase = TurnPhase::ReadyToDispatch;
  Ready.Revision = 2;
  Expect(Store.Append(Ready).index() == 0);
  TurnCheckpoint Dispatched = Ready;
  Dispatched.Phase = TurnPhase::ProviderDispatched;
  Dispatched.RequestId = RequestToken("request-provider");
  Dispatched.Revision = 3;
  Expect(Store.Append(Dispatched).index() == 0);

  TurnRecoveryCoordinator Coordinator(Store);
  const RecoveryReport First =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-a")));
  Expect(First.Decisions.size() == 1);
  Expect(First.Decisions[0].Disposition ==
         RecoveryDisposition::IsolatedOutcomeUnknown);
  Expect(!First.Decisions[0].ReplayAllowed);
  Expect(First.CheckpointsPersisted == 1);
  Expect(First.DispatchCount == 0);
  const CheckpointSnapshot AfterFirst = Snapshot(Store.Load());
  Expect(AfterFirst.RecordCount == 4);
  Expect(AfterFirst.LatestByTurn.at(TurnToken("turn-provider")).Phase ==
         TurnPhase::OutcomeUnknown);
  Expect(
      AfterFirst.LatestByTurn.at(TurnToken("turn-provider")).UnknownSideEffect);
  Expect(AfterFirst.LatestByTurn.at(TurnToken("turn-provider")).Revision == 4);

  const RecoveryReport Second =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-a")));
  Expect(Second.Decisions[0].Disposition ==
         RecoveryDisposition::OutcomeUnknown);
  Expect(Second.CheckpointsPersisted == 0);
  Expect(Snapshot(Store.Load()).RecordCount == 4);
  RemoveIfExists(Path);
}

void TestToolAndStreamStatesAreNeverReplayed() {
  const auto Path = TempPath("tools");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);

  static_cast<void>(ReachProviderCompleted(Store, "turn-tool", "scope-tools",
                                           ToolExecutionState::Dispatched));
  static_cast<void>(ReachProviderCompleted(Store, "turn-tool-completed",
                                           "scope-tools",
                                           ToolExecutionState::Completed));
  TurnCheckpoint CompletedToolDispatched =
      ReachProviderCompleted(Store, "turn-tool-phase-completed", "scope-tools",
                             ToolExecutionState::Completed);
  CompletedToolDispatched.Phase = TurnPhase::ToolDispatched;
  CompletedToolDispatched.Revision += 1U;
  Expect(Store.Append(CompletedToolDispatched).index() == 0);

  TurnCheckpoint Streaming = MakeTurn("turn-stream", "scope-tools");
  Expect(Store.Append(Streaming).index() == 0);
  Streaming.Phase = TurnPhase::ReadyToDispatch;
  Streaming.Revision = 2;
  Expect(Store.Append(Streaming).index() == 0);
  Streaming.Phase = TurnPhase::ProviderDispatched;
  Streaming.RequestId = RequestToken("request-a");
  Streaming.Revision = 3;
  Expect(Store.Append(Streaming).index() == 0);
  Streaming.Phase = TurnPhase::ProviderStreaming;
  Streaming.Revision = 4;
  Expect(Store.Append(Streaming).index() == 0);

  TurnRecoveryCoordinator Coordinator(Store);
  const RecoveryReport Report =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-tools")));
  Expect(Report.Decisions.size() == 4);
  const auto FindDecision =
      [&Report](std::string_view TurnId) -> const RecoveryDecision& {
    const std::string OpaqueTurnId = TurnToken(TurnId);
    for (const RecoveryDecision& Decision : Report.Decisions) {
      if (Decision.TurnId == OpaqueTurnId) {
        return Decision;
      }
    }
    Expect(false);
    return Report.Decisions.front();
  };
  Expect(FindDecision("turn-tool").Disposition ==
         RecoveryDisposition::IsolatedOutcomeUnknown);
  Expect(!FindDecision("turn-tool").ReplayAllowed);
  const RecoveryDecision& CompletedTool = FindDecision("turn-tool-completed");
  Expect(CompletedTool.Disposition ==
         RecoveryDisposition::OperationReconciliationRequired);
  Expect(CompletedTool.OriginalToolState == ToolExecutionState::Completed);
  Expect(!CompletedTool.ReplayAllowed);
  const RecoveryDecision& CompletedToolPhase =
      FindDecision("turn-tool-phase-completed");
  Expect(CompletedToolPhase.Disposition ==
         RecoveryDisposition::OperationReconciliationRequired);
  Expect(CompletedToolPhase.OriginalToolState == ToolExecutionState::Completed);
  Expect(!CompletedToolPhase.ReplayAllowed);
  Expect(FindDecision("turn-stream").Disposition ==
         RecoveryDisposition::IsolatedOutcomeUnknown);
  Expect(Report.CheckpointsPersisted == 2);
  Expect(Report.DispatchCount == 0);
  const CheckpointSnapshot LoadedAfterIsolation = Snapshot(Store.Load());
  const auto& Latest = LoadedAfterIsolation.LatestByTurn;
  Expect(Latest.at(TurnToken("turn-tool")).ToolState ==
         ToolExecutionState::UnknownOutcome);
  Expect(Latest.at(TurnToken("turn-tool-completed")).ToolState ==
         ToolExecutionState::Completed);
  Expect(Latest.at(TurnToken("turn-tool-completed")).Phase ==
         TurnPhase::ProviderCompleted);
  Expect(Latest.at(TurnToken("turn-tool-phase-completed")).Phase ==
         TurnPhase::ToolDispatched);
  Expect(Latest.at(TurnToken("turn-stream")).ToolState ==
         ToolExecutionState::NotRequested);
  RemoveIfExists(Path);
}

void TestExplicitUnknownSideEffectIsolatedAfterProviderDispatch() {
  const auto Path = TempPath("unknown-flag");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = MakeTurn("turn-unknown", "scope-unknown");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ReadyToDispatch;
  Checkpoint.Revision = 2;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderDispatched;
  Checkpoint.RequestId = RequestToken("request-unknown");
  Checkpoint.Revision = 3;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.UnknownSideEffect = true;
  Checkpoint.Revision = 4;
  Expect(Store.Append(Checkpoint).index() == 0);
  TurnRecoveryCoordinator Coordinator(Store);
  const RecoveryReport Report =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-unknown")));
  Expect(Report.Decisions.size() == 1);
  Expect(Report.Decisions[0].Disposition ==
         RecoveryDisposition::IsolatedOutcomeUnknown);
  Expect(Report.Decisions[0].ReplayAllowed == false);
  RemoveIfExists(Path);
}

void TestTerminalStatesRemainTerminal() {
  const auto Path = TempPath("terminal");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);

  TurnCheckpoint Completed =
      ReachProviderCompleted(Store, "turn-completed", "scope-terminal",
                             ToolExecutionState::NotRequested);
  Completed.Phase = TurnPhase::Completed;
  Completed.Revision = 6;
  Expect(Store.Append(Completed).index() == 0);

  TurnCheckpoint Cancelled = MakeTurn("turn-cancelled", "scope-terminal");
  Expect(Store.Append(Cancelled).index() == 0);
  Cancelled.Phase = TurnPhase::ReadyToDispatch;
  Cancelled.Revision = 2;
  Expect(Store.Append(Cancelled).index() == 0);
  Cancelled.Phase = TurnPhase::Cancelled;
  Cancelled.Revision = 3;
  Expect(Store.Append(Cancelled).index() == 0);

  TurnCheckpoint Failed = MakeTurn("turn-failed", "scope-terminal");
  Expect(Store.Append(Failed).index() == 0);
  Failed.Phase = TurnPhase::Failed;
  Failed.Revision = 2;
  Expect(Store.Append(Failed).index() == 0);

  TurnCheckpoint Unknown = MakeTurn("turn-unknown", "scope-terminal");
  Expect(Store.Append(Unknown).index() == 0);
  Unknown.Phase = TurnPhase::ReadyToDispatch;
  Unknown.Revision = 2;
  Expect(Store.Append(Unknown).index() == 0);
  Unknown.Phase = TurnPhase::ProviderDispatched;
  Unknown.RequestId = RequestToken("request-terminal-unknown");
  Unknown.Revision = 3;
  Expect(Store.Append(Unknown).index() == 0);
  Unknown.Phase = TurnPhase::OutcomeUnknown;
  Unknown.UnknownSideEffect = true;
  Unknown.Revision = 4;
  Expect(Store.Append(Unknown).index() == 0);

  TurnCheckpoint UnsupportedState =
      MakeTurn("turn-unsupported", "scope-terminal");
  Expect(Store.Append(UnsupportedState).index() == 0);
  UnsupportedState.Phase = TurnPhase::Unsupported;
  UnsupportedState.Revision = 2;
  Expect(Store.Append(UnsupportedState).index() == 0);

  TurnRecoveryCoordinator Coordinator(Store);
  const RecoveryReport Report =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-terminal")));
  Expect(Report.Decisions.size() == 5);
  Expect(Report.CheckpointsPersisted == 0);
  const auto FindDecision =
      [&Report](std::string_view TurnId) -> const RecoveryDecision& {
    const std::string OpaqueTurnId = TurnToken(TurnId);
    for (const RecoveryDecision& Decision : Report.Decisions) {
      if (Decision.TurnId == OpaqueTurnId) {
        return Decision;
      }
    }
    Expect(false);
    return Report.Decisions.front();
  };
  const RecoveryDecision& CompletedDecision = FindDecision("turn-completed");
  Expect(CompletedDecision.Disposition == RecoveryDisposition::Completed);
  Expect(CompletedDecision.OriginalPhase == TurnPhase::Completed);
  Expect(!CompletedDecision.ReplayAllowed);
  const RecoveryDecision& CancelledDecision = FindDecision("turn-cancelled");
  Expect(CancelledDecision.Disposition == RecoveryDisposition::Cancelled);
  Expect(CancelledDecision.OriginalPhase == TurnPhase::Cancelled);
  const RecoveryDecision& FailedDecision = FindDecision("turn-failed");
  Expect(FailedDecision.Disposition == RecoveryDisposition::Failed);
  Expect(FailedDecision.OriginalPhase == TurnPhase::Failed);
  const RecoveryDecision& UnknownDecision = FindDecision("turn-unknown");
  Expect(UnknownDecision.Disposition == RecoveryDisposition::OutcomeUnknown);
  Expect(UnknownDecision.OriginalPhase == TurnPhase::OutcomeUnknown);
  const RecoveryDecision& UnsupportedDecision =
      FindDecision("turn-unsupported");
  Expect(UnsupportedDecision.Disposition == RecoveryDisposition::Unsupported);
  Expect(UnsupportedDecision.OriginalPhase == TurnPhase::Unsupported);
  RemoveIfExists(Path);
}

void TestRecoveryRejectsDeterminateTerminalsWithPendingDispatches() {
  const std::vector<std::string_view> TerminalPhases{"Completed", "Cancelled",
                                                     "Failed", "Unsupported"};
  std::uint64_t Case = 0;
  for (const std::string_view Phase : TerminalPhases) {
    for (const bool OperationDispatch : {false, true}) {
      const auto Path =
          TempPath("terminal-pending-dispatch-" + std::to_string(Case++));
      RemoveIfExists(Path);
      WriteAll(Path, PendingDispatchRecord(Phase, OperationDispatch));

      TurnCheckpointStore Store(Path);
      std::vector<RecoveryEvent> Events;
      TurnRecoveryCoordinator Coordinator(
          Store,
          [&Events](const RecoveryEvent& Event) { Events.push_back(Event); });
      const RecoveryError Error =
          ExpectError(Coordinator.Recover(ScopeToken("scope-pending")));
      Expect(Error.Code == RecoveryErrorCode::StoreFailure);
      Expect(Events.empty());
      RemoveIfExists(Path);
    }
  }
}

void TestRecoveryRejectsPlainTextAndCredentialsBeforeEvents() {
  struct InvalidReference final {
    std::string TurnId;
    std::string RequestId;
    std::string ScopeRef;
    std::string ContextReference;
    std::string Phase;
    std::string Sentinel;
  };
  const std::vector<InvalidReference> Cases{
      {.TurnId = TurnToken("turn-body-context"),
       .RequestId = {},
       .ScopeRef = ScopeToken("scope-body-context"),
       .ContextReference = "private-body",
       .Phase = "Created",
       .Sentinel = "private-body"},
      {.TurnId = TurnToken("turn-token-request"),
       .RequestId = "Bearer sk-test-token-value",
       .ScopeRef = ScopeToken("scope-token-request"),
       .ContextReference = ContextToken("context-token-request"),
       .Phase = "ProviderDispatched",
       .Sentinel = "sk-test-token-value"},
  };
  std::uint64_t Case = 0;
  for (const InvalidReference& Invalid : Cases) {
    const auto Path =
        TempPath("invalid-opaque-recovery-" + std::to_string(Case++));
    RemoveIfExists(Path);
    WriteAll(Path, InvalidOpaqueReferenceRecord(
                       Invalid.TurnId, Invalid.RequestId, Invalid.ScopeRef,
                       Invalid.ContextReference, Invalid.Phase));
    TurnCheckpointStore Store(Path);
    std::vector<RecoveryEvent> Events;
    TurnRecoveryCoordinator Coordinator(
        Store,
        [&Events](const RecoveryEvent& Event) { Events.push_back(Event); });
    const RecoveryError Error =
        ExpectError(Coordinator.Recover(Invalid.ScopeRef));
    Expect(Error.Code == RecoveryErrorCode::StoreFailure);
    Expect(Error.SanitizedDescription.find(Invalid.Sentinel) ==
           std::string::npos);
    Expect(Events.empty());
    RemoveIfExists(Path);
  }
}

void TestInactiveScopesAreSkippedWithoutAppend() {
  const auto Path = TempPath("inactive");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  Expect(Store.Append(MakeTurn("turn-revoked", "scope-revoked")).index() == 0);
  Expect(Store.Append(MakeTurn("turn-deleted", "scope-deleted")).index() == 0);
  Expect(
      Store.Append(MakeTurn("turn-tombstoned", "scope-tombstoned")).index() ==
      0);
  Expect(Store
             .MarkScope(ScopeCheckpoint{
                 .ScopeRef = ScopeToken("scope-revoked"),
                 .State = ScopeState::Revoked,
                 .UpdatedAt = std::chrono::system_clock::now(),
                 .Revision = 1,
                 .Version = 1,
             })
             .index() == 0);
  Expect(Store
             .MarkScope(ScopeCheckpoint{
                 .ScopeRef = ScopeToken("scope-deleted"),
                 .State = ScopeState::Deleted,
                 .UpdatedAt = std::chrono::system_clock::now(),
                 .Revision = 1,
                 .Version = 1,
             })
             .index() == 0);
  Expect(Store
             .MarkScope(ScopeCheckpoint{
                 .ScopeRef = ScopeToken("scope-tombstoned"),
                 .State = ScopeState::Tombstoned,
                 .UpdatedAt = std::chrono::system_clock::now(),
                 .Revision = 1,
                 .Version = 1,
             })
             .index() == 0);

  TurnRecoveryCoordinator Coordinator(Store);
  for (const std::string& Scope :
       {ScopeToken("scope-revoked"), ScopeToken("scope-deleted"),
        ScopeToken("scope-tombstoned")}) {
    const RecoveryReport Report = ExpectReport(Coordinator.Recover(Scope));
    Expect(Report.ScopeState != ScopeState::Active);
    Expect(Report.Decisions.size() == 1);
    Expect(Report.Decisions[0].Disposition ==
           RecoveryDisposition::SkippedInactiveScope);
    Expect(!Report.Decisions[0].ReplayAllowed);
    Expect(Report.CheckpointsPersisted == 0);
  }
  Expect(Snapshot(Store.Load()).RecordCount == 6);
  RemoveIfExists(Path);
}

void TestScopeFilteringAndEventOrdering() {
  const auto Path = TempPath("filter");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  Expect(Store.Append(MakeTurn("turn-a", "scope-a")).index() == 0);
  Expect(Store.Append(MakeTurn("turn-b", "scope-b")).index() == 0);
  Expect(Store.Append(MakeTurn("turn-c", "scope-a")).index() == 0);

  std::vector<RecoveryEvent> Events;
  TurnRecoveryCoordinator Coordinator(
      Store,
      [&Events](const RecoveryEvent& Event) { Events.push_back(Event); });
  const RecoveryReport Report =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-a")));
  Expect(Report.Decisions.size() == 2);
  const std::string FirstTurn = Report.Decisions[0].TurnId;
  const std::string SecondTurn = Report.Decisions[1].TurnId;
  Expect(
      (FirstTurn == TurnToken("turn-a") && SecondTurn == TurnToken("turn-c")) ||
      (FirstTurn == TurnToken("turn-c") && SecondTurn == TurnToken("turn-a")));
  Expect(Events.size() == 2);
  Expect(Events[0].SequenceNumber == 1);
  Expect(Events[1].SequenceNumber == 2);
  RemoveIfExists(Path);
}

void TestEventSinkFailureIsReportedWithoutDispatch() {
  const auto Path = TempPath("sink-failure");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  Expect(Store.Append(MakeTurn("turn-sink", "scope-sink")).index() == 0);
  TurnRecoveryCoordinator Coordinator(Store,
                                      [](const RecoveryEvent&) { throw 7; });
  const RecoveryError Error =
      ExpectError(Coordinator.Recover(ScopeToken("scope-sink")));
  Expect(Error.Code == RecoveryErrorCode::EventDeliveryFailure);
  Expect(Snapshot(Store.Load()).RecordCount == 1);
  RemoveIfExists(Path);
}

void TestInvalidScopeAndCorruptStoreFailClosed() {
  {
    const auto Path = TempPath("invalid-scope");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnRecoveryCoordinator Coordinator(Store);
    const RecoveryError Error =
        ExpectError(Coordinator.Recover("scope with body"));
    Expect(Error.Code == RecoveryErrorCode::InvalidScope);
  }
  {
    const auto Path = TempPath("corrupt");
    RemoveIfExists(Path);
    {
      std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
      Output << "SECRET_SENTINEL private body\n";
    }
    TurnCheckpointStore Store(Path);
    TurnRecoveryCoordinator Coordinator(Store);
    const RecoveryError Error =
        ExpectError(Coordinator.Recover(ScopeToken("scope-a")));
    Expect(Error.Code == RecoveryErrorCode::StoreFailure);
    Expect(Error.SanitizedDescription.find("SECRET_SENTINEL") ==
           std::string::npos);
    Expect(Error.SanitizedDescription.find("private body") ==
           std::string::npos);
    RemoveIfExists(Path);
  }
}

void TestOperationRecoveryCarriesOperationState() {
  const auto Path = TempPath("operation");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = MakeTurn("turn-operation", "scope-operation");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ReadyToDispatch;
  Checkpoint.Revision = 2;
  Checkpoint.Operation = OperationState::Prepared;
  Checkpoint.OperationId = OperationToken("operation-1");
  Checkpoint.IdempotencyKey = IdempotencyToken("idempotency-1");
  Checkpoint.ActionId = ActionToken("action-1");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderDispatched;
  Checkpoint.RequestId = RequestToken("request-operation");
  Checkpoint.Revision = 3;
  Checkpoint.Operation = OperationState::Authorized;
  Checkpoint.ActionId = ActionToken("action-1");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderCompleted;
  Checkpoint.Revision = 4;
  Checkpoint.Operation = OperationState::Dispatched;
  Expect(Store.Append(Checkpoint).index() == 0);

  TurnRecoveryCoordinator Coordinator(Store);
  const RecoveryReport First =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-operation")));
  Expect(First.Decisions.size() == 1);
  const RecoveryDecision& Decision = First.Decisions.front();
  Expect(Decision.Disposition == RecoveryDisposition::IsolatedOutcomeUnknown);
  Expect(Decision.OriginalPhase == TurnPhase::ProviderCompleted);
  Expect(Decision.OriginalToolState == ToolExecutionState::NotRequested);
  Expect(Decision.Operation == OperationState::Dispatched);
  Expect(Decision.OperationId == OperationToken("operation-1"));
  Expect(Decision.IdempotencyKey == IdempotencyToken("idempotency-1"));
  Expect(Decision.ActionId == ActionToken("action-1"));
  Expect(!Decision.ReplayAllowed);
  const CheckpointSnapshot Isolated = Snapshot(Store.Load());
  Expect(Isolated.LatestByTurn.at(TurnToken("turn-operation")).Phase ==
         TurnPhase::OutcomeUnknown);
  Expect(Isolated.LatestByTurn.at(TurnToken("turn-operation")).Operation ==
         OperationState::UnknownOutcome);
  Expect(
      Isolated.LatestByTurn.at(TurnToken("turn-operation")).UnknownSideEffect);

  const RecoveryReport Second =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-operation")));
  Expect(Second.Decisions.front().Disposition ==
         RecoveryDisposition::OutcomeUnknown);
  Expect(Second.Decisions.front().Operation == OperationState::UnknownOutcome);
  Expect(Second.Decisions.front().IdempotencyKey ==
         IdempotencyToken("idempotency-1"));
  RemoveIfExists(Path);
}

void TestKnownOperationOutcomeIsReconciledWithoutReplay() {
  for (const OperationState KnownOutcome :
       {OperationState::Succeeded, OperationState::Rejected,
        OperationState::Unsupported, OperationState::Failed}) {
    const auto Path = TempPath("known-operation-outcome");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnCheckpoint Checkpoint =
        MakeTurn("turn-known-outcome", "scope-known-outcome");
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ReadyToDispatch;
    Checkpoint.Revision = 2;
    Checkpoint.Operation = OperationState::Prepared;
    Checkpoint.OperationId = OperationToken("operation-known");
    Checkpoint.IdempotencyKey = IdempotencyToken("idempotency-known");
    Checkpoint.ActionId = ActionToken("action-known");
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderDispatched;
    Checkpoint.RequestId = RequestToken("request-known");
    Checkpoint.Revision = 3;
    Checkpoint.Operation = OperationState::Authorized;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderCompleted;
    Checkpoint.Revision = 4;
    Checkpoint.Operation = OperationState::Dispatched;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Revision = 5;
    Checkpoint.Operation = KnownOutcome;
    Expect(Store.Append(Checkpoint).index() == 0);

    TurnRecoveryCoordinator Coordinator(Store);
    const RecoveryReport First =
        ExpectReport(Coordinator.Recover(ScopeToken("scope-known-outcome")));
    Expect(First.Decisions.size() == 1);
    const RecoveryDecision& Decision = First.Decisions.front();
    Expect(Decision.Disposition ==
           RecoveryDisposition::OperationReconciliationRequired);
    Expect(Decision.Operation == KnownOutcome);
    Expect(Decision.OperationId == OperationToken("operation-known"));
    Expect(Decision.IdempotencyKey == IdempotencyToken("idempotency-known"));
    Expect(Decision.ActionId == ActionToken("action-known"));
    Expect(!Decision.ReplayAllowed);
    Expect(First.CheckpointsPersisted == 0);

    const CheckpointSnapshot AfterFirst = Snapshot(Store.Load());
    const TurnCheckpoint& Persisted =
        AfterFirst.LatestByTurn.at(TurnToken("turn-known-outcome"));
    Expect(Persisted.Operation == KnownOutcome);
    Expect(Persisted.Phase == TurnPhase::ProviderCompleted);
    Expect(!Persisted.UnknownSideEffect);

    const RecoveryReport Second =
        ExpectReport(Coordinator.Recover(ScopeToken("scope-known-outcome")));
    Expect(Second.Decisions.front().Disposition ==
           RecoveryDisposition::OperationReconciliationRequired);
    Expect(Second.Decisions.front().IdempotencyKey ==
           IdempotencyToken("idempotency-known"));
    Expect(Second.CheckpointsPersisted == 0);
    RemoveIfExists(Path);
  }
}

void TestSuccessfulOperationCannotMaskUnknownProviderOutcome() {
  for (const TurnPhase ProviderPhase :
       {TurnPhase::ProviderDispatched, TurnPhase::ProviderStreaming}) {
    for (const bool UnknownSideEffect : {false, true}) {
      const auto Path = TempPath("provider-inflight-known-operation");
      RemoveIfExists(Path);
      TurnCheckpointStore Store(Path);
      TurnCheckpoint Checkpoint =
          MakeTurn("turn-provider-known-operation", "scope-provider-known");
      Expect(Store.Append(Checkpoint).index() == 0);
      Checkpoint.Phase = TurnPhase::ReadyToDispatch;
      Checkpoint.Revision = 2;
      Checkpoint.Operation = OperationState::Prepared;
      Checkpoint.OperationId = OperationToken("operation-provider-known");
      Checkpoint.IdempotencyKey = IdempotencyToken("idem-provider-known");
      Checkpoint.ActionId = ActionToken("action-provider-known");
      Expect(Store.Append(Checkpoint).index() == 0);
      Checkpoint.Phase = TurnPhase::ProviderDispatched;
      Checkpoint.RequestId = RequestToken("request-provider-known");
      Checkpoint.Revision = 3;
      Checkpoint.Operation = OperationState::Authorized;
      Expect(Store.Append(Checkpoint).index() == 0);
      if (ProviderPhase == TurnPhase::ProviderStreaming) {
        Checkpoint.Phase = TurnPhase::ProviderStreaming;
        Checkpoint.Revision = 4;
        Expect(Store.Append(Checkpoint).index() == 0);
      }
      ++Checkpoint.Revision;
      Checkpoint.Operation = OperationState::Dispatched;
      Expect(Store.Append(Checkpoint).index() == 0);
      ++Checkpoint.Revision;
      Checkpoint.Operation = OperationState::Succeeded;
      Expect(Store.Append(Checkpoint).index() == 0);
      if (UnknownSideEffect) {
        ++Checkpoint.Revision;
        Checkpoint.UnknownSideEffect = true;
        Expect(Store.Append(Checkpoint).index() == 0);
      }

      const CheckpointSnapshot BeforeRecovery = Snapshot(Store.Load());
      const TurnCheckpoint& BeforeIsolation =
          BeforeRecovery.LatestByTurn.at(
              TurnToken("turn-provider-known-operation"));
      Expect(BeforeIsolation.Operation == OperationState::Succeeded);
      Expect(BeforeIsolation.UnknownSideEffect == UnknownSideEffect);

      TurnRecoveryCoordinator Coordinator(Store);
      const RecoveryReport First =
          ExpectReport(Coordinator.Recover(ScopeToken("scope-provider-known")));
      Expect(First.Decisions.size() == 1);
      const RecoveryDecision& Decision = First.Decisions.front();
      Expect(Decision.Disposition == RecoveryDisposition::IsolatedOutcomeUnknown);
      Expect(Decision.OriginalPhase == ProviderPhase);
      Expect(Decision.Operation == OperationState::Succeeded);
      Expect(Decision.OperationId == OperationToken("operation-provider-known"));
      Expect(!Decision.ReplayAllowed);
      Expect(First.DispatchCount == 0);
      Expect(First.CheckpointsPersisted == 1);

      const CheckpointSnapshot AfterIsolation = Snapshot(Store.Load());
      const TurnCheckpoint& Isolated = AfterIsolation.LatestByTurn.at(
          TurnToken("turn-provider-known-operation"));
      Expect(Isolated.Phase == TurnPhase::OutcomeUnknown);
      Expect(Isolated.Operation == OperationState::Succeeded);
      Expect(Isolated.UnknownSideEffect);

      const RecoveryReport Second =
          ExpectReport(Coordinator.Recover(ScopeToken("scope-provider-known")));
      Expect(Second.Decisions.front().Disposition ==
             RecoveryDisposition::OutcomeUnknown);
      Expect(Second.Decisions.front().Operation == OperationState::Succeeded);
      Expect(!Second.Decisions.front().ReplayAllowed);
      Expect(Second.CheckpointsPersisted == 0);
      Expect(Second.DispatchCount == 0);
      const std::size_t ExpectedRecords =
          (ProviderPhase == TurnPhase::ProviderStreaming ? 7U : 6U) +
          (UnknownSideEffect ? 1U : 0U);
      Expect(Snapshot(Store.Load()).RecordCount == ExpectedRecords);
      RemoveIfExists(Path);
    }
  }
}

void TestSuccessfulOperationCannotMaskUnknownChannelSend() {
  const auto Path = TempPath("successful-operation-unknown-channel-send");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = ReachProviderCompleted(
      Store, "turn-send-known-operation", "scope-send-known-operation",
      ToolExecutionState::Completed);
  Checkpoint.Phase = TurnPhase::Sending;
  ++Checkpoint.Revision;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.UnknownSideEffect = true;
  ++Checkpoint.Revision;
  Expect(Store.Append(Checkpoint).index() == 0);

  const CheckpointSnapshot BeforeRecovery = Snapshot(Store.Load());
  const TurnCheckpoint& Sending = BeforeRecovery.LatestByTurn.at(
      TurnToken("turn-send-known-operation"));
  Expect(Sending.Phase == TurnPhase::Sending);
  Expect(Sending.ToolState == ToolExecutionState::Completed);
  Expect(Sending.Operation == OperationState::Succeeded);
  Expect(Sending.UnknownSideEffect);

  TurnRecoveryCoordinator Coordinator(Store);
  const RecoveryReport First =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-send-known-operation")));
  Expect(First.Decisions.size() == 1);
  const RecoveryDecision& FirstDecision = First.Decisions.front();
  Expect(FirstDecision.Disposition ==
         RecoveryDisposition::IsolatedOutcomeUnknown);
  Expect(FirstDecision.OriginalPhase == TurnPhase::Sending);
  Expect(FirstDecision.OriginalToolState == ToolExecutionState::Completed);
  Expect(FirstDecision.Operation == OperationState::Succeeded);
  Expect(!FirstDecision.ReplayAllowed);
  Expect(First.DispatchCount == 0);
  Expect(First.CheckpointsPersisted == 1);

  const CheckpointSnapshot AfterFirst = Snapshot(Store.Load());
  const TurnCheckpoint& Isolated = AfterFirst.LatestByTurn.at(
      TurnToken("turn-send-known-operation"));
  Expect(Isolated.Phase == TurnPhase::OutcomeUnknown);
  Expect(Isolated.ToolState == ToolExecutionState::Completed);
  Expect(Isolated.Operation == OperationState::Succeeded);
  Expect(Isolated.UnknownSideEffect);

  const RecoveryReport Second =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-send-known-operation")));
  Expect(Second.Decisions.size() == 1);
  const RecoveryDecision& SecondDecision = Second.Decisions.front();
  Expect(SecondDecision.Disposition == RecoveryDisposition::OutcomeUnknown);
  Expect(SecondDecision.OriginalToolState == ToolExecutionState::Completed);
  Expect(SecondDecision.Operation == OperationState::Succeeded);
  Expect(!SecondDecision.ReplayAllowed);
  Expect(Second.DispatchCount == 0);
  Expect(Second.CheckpointsPersisted == 0);
  Expect(Snapshot(Store.Load()).RecordCount == AfterFirst.RecordCount);
  RemoveIfExists(Path);
}

void TestAuthorizedUndispatchedOperationCanResumeWithSameKey() {
  const auto Path = TempPath("authorized-operation");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = MakeTurn("turn-authorized", "scope-authorized");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ReadyToDispatch;
  Checkpoint.Revision = 2;
  Checkpoint.Operation = OperationState::Prepared;
  Checkpoint.OperationId = OperationToken("operation-authorized");
  Checkpoint.IdempotencyKey = IdempotencyToken("idempotency-authorized");
  Checkpoint.ActionId = ActionToken("action-authorized");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Revision = 3;
  Checkpoint.Operation = OperationState::Authorized;
  Expect(Store.Append(Checkpoint).index() == 0);

  TurnRecoveryCoordinator Coordinator(Store);
  const RecoveryReport Report =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-authorized")));
  Expect(Report.Decisions.size() == 1);
  const RecoveryDecision& Decision = Report.Decisions.front();
  Expect(Decision.Disposition == RecoveryDisposition::SafeToResume);
  Expect(Decision.ReplayAllowed);
  Expect(Decision.Operation == OperationState::Authorized);
  Expect(Decision.OperationId == OperationToken("operation-authorized"));
  Expect(Decision.IdempotencyKey == IdempotencyToken("idempotency-authorized"));
  Expect(Decision.ActionId == ActionToken("action-authorized"));
  Expect(Report.DispatchCount == 0);
  Expect(Report.CheckpointsPersisted == 0);
  RemoveIfExists(Path);
}

void TestRecoveryEventsNeverContainForbiddenContent() {
  const auto Path = TempPath("event-sanitized");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = MakeTurn("turn-event", "scope-event");
  Checkpoint.ContextReference = ContextToken("context-event");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ReadyToDispatch;
  Checkpoint.Revision = 2;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderDispatched;
  Checkpoint.RequestId = RequestToken("request-event");
  Checkpoint.Revision = 3;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderCompleted;
  Checkpoint.Operation = OperationState::Prepared;
  Checkpoint.OperationId = OperationToken("operation-event");
  Checkpoint.IdempotencyKey = IdempotencyToken("idempotency-event");
  Checkpoint.ActionId = ActionToken("action-event");
  Checkpoint.Revision = 4;
  Expect(Store.Append(Checkpoint).index() == 0);

  std::vector<RecoveryEvent> Events;
  TurnRecoveryCoordinator Coordinator(
      Store,
      [&Events](const RecoveryEvent& Event) { Events.push_back(Event); });
  const RecoveryReport Report =
      ExpectReport(Coordinator.Recover(ScopeToken("scope-event")));
  Expect(Report.Decisions.size() == 1);
  Expect(Events.size() == 1);
  const auto HasSentinel = [](const RecoveryEvent& Event) {
    return Event.TurnId.find(TurnCheckpointStore::SecretSentinel) !=
               std::string::npos ||
           Event.RequestId.find(TurnCheckpointStore::SecretSentinel) !=
               std::string::npos ||
           Event.ContextReference.find(TurnCheckpointStore::SecretSentinel) !=
               std::string::npos ||
           Event.OperationId.find(TurnCheckpointStore::SecretSentinel) !=
               std::string::npos ||
           Event.IdempotencyKey.find(TurnCheckpointStore::SecretSentinel) !=
               std::string::npos ||
           Event.ActionId.find(TurnCheckpointStore::SecretSentinel) !=
               std::string::npos;
  };
  Expect(!HasSentinel(Events.front()));
  Expect(Events.front().OperationId == OperationToken("operation-event"));
  Expect(Events.front().IdempotencyKey ==
         IdempotencyToken("idempotency-event"));
  Expect(Events.front().ActionId == ActionToken("action-event"));

  TurnCheckpoint Forbidden = MakeTurn("turn-forbidden", "scope-event");
  Forbidden.TurnId = "SECRET_SENTINEL";
  Expect(Store.Append(Forbidden).index() != 0);
  Expect(Store
             .Append([&] {
               TurnCheckpoint Bad = MakeTurn("turn-forbidden", "scope-event");
               Bad.ContextReference = "SECRET_SENTINEL";
               return Bad;
             }())
             .index() != 0);
  RemoveIfExists(Path);
}

}  // namespace

int main() {
  TestPreDispatchTurnResumesWithoutDispatch();
  TestDispatchedProviderIsIsolatedOnce();
  TestToolAndStreamStatesAreNeverReplayed();
  TestExplicitUnknownSideEffectIsolatedAfterProviderDispatch();
  TestTerminalStatesRemainTerminal();
  TestRecoveryRejectsDeterminateTerminalsWithPendingDispatches();
  TestRecoveryRejectsPlainTextAndCredentialsBeforeEvents();
  TestInactiveScopesAreSkippedWithoutAppend();
  TestScopeFilteringAndEventOrdering();
  TestEventSinkFailureIsReportedWithoutDispatch();
  TestInvalidScopeAndCorruptStoreFailClosed();
  TestOperationRecoveryCarriesOperationState();
  TestKnownOperationOutcomeIsReconciledWithoutReplay();
  TestSuccessfulOperationCannotMaskUnknownProviderOutcome();
  TestSuccessfulOperationCannotMaskUnknownChannelSend();
  TestAuthorizedUndispatchedOperationCanResumeWithSameKey();
  TestRecoveryEventsNeverContainForbiddenContent();
  std::puts("TurnRecoveryCoordinator contract tests passed");
  return 0;
}
