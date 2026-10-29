#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "Source/Storage/TurnCheckpointStore.hpp"

namespace {

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
         ("emulative-dialog-checkpoint-" + std::string(Name) + "-" +
          std::to_string(Stamp) + "-" +
          std::to_string(PathSequence.fetch_add(1)) + ".edcp");
}

void RemoveIfExists(const std::filesystem::path& Path) {
  std::error_code Error;
  std::filesystem::remove_all(Path, Error);
}

std::string ReadAll(const std::filesystem::path& Path) {
  std::ifstream Input(Path, std::ios::binary);
  Expect(Input.good());
  return std::string(std::istreambuf_iterator<char>(Input),
                     std::istreambuf_iterator<char>());
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

std::string Checksum(std::uint64_t Value) {
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
  const std::string First = Checksum(Fnv1a(Material));
  Material.append("|second");
  return std::string(Prefix) + First + Checksum(Fnv1a(Material));
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
  const std::string RecordChecksum = Checksum(Fnv1a(Payload));
  Payload.append("|").append(RecordChecksum).append("\n");
  return Payload;
}

std::string TurnRecord(std::string_view Phase, std::string_view RequestId,
                       bool UnknownSideEffect, std::uint64_t Revision) {
  std::string Payload("EDCP|2|T|");
  Payload.append(TurnToken("turn-terminal")).append("|");
  if (!RequestId.empty()) {
    Payload.append(RequestToken(RequestId));
  }
  Payload.append("|")
      .append(ScopeToken("scope-a"))
      .append("|")
      .append(ContextToken("context-a"))
      .append("|")
      .append(Phase.data(), Phase.size())
      .append("|NotRequested|")
      .append(UnknownSideEffect ? "1" : "0")
      .append("|None||||1700000000123|")
      .append(std::to_string(Revision));
  return CompleteRecord(std::move(Payload));
}

std::string ToolStateTurnRecord(std::string_view Phase,
                                std::string_view RequestId,
                                std::string_view ToolState,
                                std::uint64_t Revision,
                                bool UnknownSideEffect = false) {
  std::string Payload("EDCP|2|T|");
  Payload.append(TurnToken("turn-tool-history")).append("|");
  if (!RequestId.empty()) {
    Payload.append(RequestToken(RequestId));
  }
  Payload.append("|")
      .append(ScopeToken("scope-tool-history"))
      .append("|")
      .append(ContextToken("context-tool-history"))
      .append("|")
      .append(Phase.data(), Phase.size())
      .append("|")
      .append(ToolState.data(), ToolState.size())
      .append(UnknownSideEffect ? "|1|None||||1700000000123|"
                                : "|0|None||||1700000000123|")
      .append(std::to_string(Revision));
  return CompleteRecord(std::move(Payload));
}

TurnCheckpoint MakeTurn(std::string TurnId = "turn-a",
                        std::string ScopeRef = "scope-a") {
  return TurnCheckpoint{
      .TurnId = TurnId.empty() ? std::string{} : TurnToken(TurnId),
      .RequestId = {},
      .ScopeRef = ScopeToken(ScopeRef),
      .ContextReference = ContextToken("context-a"),
      .Phase = TurnPhase::Created,
      .ToolState = ToolExecutionState::NotRequested,
      .Operation = OperationState::None,
      .OperationId = {},
      .IdempotencyKey = {},
      .ActionId = {},
      .UnknownSideEffect = false,
      .UpdatedAt =
          std::chrono::system_clock::time_point{
              std::chrono::milliseconds{1'700'000'000'123LL}},
      .Revision = 1,
      .Version = 1,
  };
}

ScopeCheckpoint MakeScope(std::string ScopeRef, ScopeState State,
                          std::uint64_t Revision) {
  return ScopeCheckpoint{
      .ScopeRef = ScopeToken(ScopeRef),
      .State = State,
      .UpdatedAt =
          std::chrono::system_clock::time_point{
              std::chrono::milliseconds{1'700'000'000'000LL}},
      .Revision = Revision,
      .Version = 1,
  };
}

CheckpointStoreError ExpectError(
    const CheckpointResult<std::monostate>& Result) {
  if (const auto* Error = std::get_if<CheckpointStoreError>(&Result)) {
    std::fprintf(stderr, "EXPECTED_ERROR code=%u description=%s\n",
                 static_cast<unsigned>(Error->Code),
                 Error->SanitizedDescription.c_str());
    return *Error;
  }
  std::fputs("EXPECTED_ERROR missing\n", stderr);
  Expect(std::holds_alternative<CheckpointStoreError>(Result));
  return std::get<CheckpointStoreError>(Result);
}

CheckpointStoreError ExpectLoadError(
    const CheckpointResult<CheckpointSnapshot>& Result) {
  if (const auto* Error = std::get_if<CheckpointStoreError>(&Result)) {
    std::fprintf(stderr, "EXPECTED_LOAD_ERROR code=%u description=%s\n",
                 static_cast<unsigned>(Error->Code),
                 Error->SanitizedDescription.c_str());
    return *Error;
  }
  std::fputs("EXPECTED_LOAD_ERROR missing\n", stderr);
  Expect(std::holds_alternative<CheckpointStoreError>(Result));
  return std::get<CheckpointStoreError>(Result);
}

void ExpectAppendSuccess(const CheckpointResult<std::monostate>& Result,
                         const char* Context = __builtin_FUNCTION()) {
  if (const auto* Error = std::get_if<CheckpointStoreError>(&Result)) {
    std::fprintf(stderr, "APPEND failed in %s: code=%u description=%s\n",
                 Context, static_cast<unsigned>(Error->Code),
                 Error->SanitizedDescription.c_str());
  }
  Expect(Result.index() == 0);
}

void TestRoundTripRestartAndIdempotentAppend() {
  const auto Path = TempPath("roundtrip");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);

  TurnCheckpoint Created = MakeTurn();
  Created.ContextReference = ContextToken("context-created");
  Expect(Store.Append(Created).index() == 0);

  TurnCheckpoint Ready = Created;
  Ready.Phase = TurnPhase::ReadyToDispatch;
  Ready.Revision = 2;
  ExpectAppendSuccess(Store.Append(Ready));

  TurnCheckpoint Dispatched = Ready;
  Dispatched.Phase = TurnPhase::ProviderDispatched;
  Dispatched.RequestId = RequestToken("request-a");
  Dispatched.Revision = 3;
  Expect(Store.Append(Dispatched).index() == 0);

  const auto BeforeDuplicate = std::get<CheckpointSnapshot>(Store.Load());
  Expect(BeforeDuplicate.RecordCount == 3);
  Expect(Store.Append(Dispatched).index() == 0);
  const auto AfterDuplicate = std::get<CheckpointSnapshot>(Store.Load());
  Expect(AfterDuplicate.RecordCount == 3);

  TurnCheckpointStore Restarted(Path);
  const auto Loaded = Restarted.Load();
  Expect(std::holds_alternative<CheckpointSnapshot>(Loaded));
  const auto& Snapshot = std::get<CheckpointSnapshot>(Loaded);
  const auto& Latest = Snapshot.LatestByTurn.at(TurnToken("turn-a"));
  Expect(Latest.Phase == TurnPhase::ProviderDispatched);
  Expect(Latest.RequestId == RequestToken("request-a"));
  Expect(Latest.ContextReference == ContextToken("context-created"));
  Expect(Latest.Revision == 3);
  Expect(Latest.UpdatedAt == Created.UpdatedAt);
  RemoveIfExists(Path);
}

void TestRevisionIdentityAndTerminalRules() {
  const auto Path = TempPath("transition");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);

  TurnCheckpoint BadInitial = MakeTurn();
  BadInitial.Revision = 2;
  Expect(ExpectError(Store.Append(BadInitial)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  TurnCheckpoint Created = MakeTurn();
  Expect(Store.Append(Created).index() == 0);

  TurnCheckpoint ScopeMismatch = Created;
  ScopeMismatch.ScopeRef = ScopeToken("scope-b");
  ScopeMismatch.Revision = 2;
  Expect(ExpectError(Store.Append(ScopeMismatch)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  TurnCheckpoint RevisionGap = Created;
  RevisionGap.Revision = 3;
  Expect(ExpectError(Store.Append(RevisionGap)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  TurnCheckpoint Ready = Created;
  Ready.Phase = TurnPhase::ReadyToDispatch;
  Ready.Revision = 2;
  Expect(Store.Append(Ready).index() == 0);

  TurnCheckpoint ProviderDispatched = Ready;
  ProviderDispatched.Phase = TurnPhase::ProviderDispatched;
  ProviderDispatched.RequestId = RequestToken("request-complete");
  ProviderDispatched.Revision = 3;
  Expect(Store.Append(ProviderDispatched).index() == 0);

  TurnCheckpoint ProviderStreaming = ProviderDispatched;
  ProviderStreaming.Phase = TurnPhase::ProviderStreaming;
  ProviderStreaming.Revision = 4;
  Expect(Store.Append(ProviderStreaming).index() == 0);

  TurnCheckpoint ProviderCompleted = ProviderStreaming;
  ProviderCompleted.Phase = TurnPhase::ProviderCompleted;
  ProviderCompleted.Revision = 5;
  Expect(Store.Append(ProviderCompleted).index() == 0);

  TurnCheckpoint Completed = ProviderCompleted;
  Completed.Phase = TurnPhase::Completed;
  Completed.Revision = 6;
  Expect(Store.Append(Completed).index() == 0);

  TurnCheckpoint Reopen = Completed;
  Reopen.Phase = TurnPhase::Failed;
  Reopen.Revision = 7;
  const CheckpointStoreError ReopenError = ExpectError(Store.Append(Reopen));
  Expect(ReopenError.Code == CheckpointStoreErrorCode::InvalidTransition);

  TurnCheckpoint InvalidCompletion = MakeTurn("turn-invalid-completion");
  InvalidCompletion.Phase = TurnPhase::Completed;
  InvalidCompletion.RequestId = RequestToken("request-invalid");
  const CheckpointStoreError CompletionError =
      ExpectError(Store.Append(InvalidCompletion));
  Expect(CompletionError.Code == CheckpointStoreErrorCode::InvalidTransition);

  TurnCheckpoint Isolated = MakeTurn("turn-isolated");
  Expect(Store.Append(Isolated).index() == 0);
  Isolated.Phase = TurnPhase::ReadyToDispatch;
  Isolated.Revision = 2;
  Expect(Store.Append(Isolated).index() == 0);
  Isolated.Phase = TurnPhase::ProviderDispatched;
  Isolated.RequestId = RequestToken("request-isolated");
  Isolated.Revision = 3;
  Expect(Store.Append(Isolated).index() == 0);
  Isolated.UnknownSideEffect = true;
  Isolated.Revision = 4;
  Expect(Store.Append(Isolated).index() == 0);
  Isolated.UnknownSideEffect = false;
  Isolated.Revision = 5;
  const CheckpointStoreError Downgrade = ExpectError(Store.Append(Isolated));
  Expect(Downgrade.Code == CheckpointStoreErrorCode::InvalidTransition);
  RemoveIfExists(Path);
}

void TestDeterminateTerminalRejectsPendingDispatches() {
  const std::vector<TurnPhase> TerminalPhases{
      TurnPhase::Completed,
      TurnPhase::Cancelled,
      TurnPhase::Failed,
      TurnPhase::Unsupported,
  };
  std::uint64_t Case = 0;
  for (const TurnPhase Phase : TerminalPhases) {
    for (const bool OperationDispatch : {false, true}) {
      const auto Path =
          TempPath("terminal-pending-dispatch-" + std::to_string(Case++));
      RemoveIfExists(Path);
      TurnCheckpointStore Store(Path);
      TurnCheckpoint Checkpoint =
          MakeTurn("turn-terminal-pending-" + std::to_string(Case));
      Checkpoint.RequestId = RequestToken("request-terminal-pending");
      Checkpoint.Phase = Phase;
      if (OperationDispatch) {
        Checkpoint.Operation = OperationState::Dispatched;
        Checkpoint.OperationId = OperationToken("operation-pending");
        Checkpoint.IdempotencyKey = IdempotencyToken("idempotency-pending");
        Checkpoint.ActionId = ActionToken("action-pending");
      } else {
        Checkpoint.ToolState = ToolExecutionState::Dispatched;
      }

      const CheckpointStoreError Error = ExpectError(Store.Append(Checkpoint));
      Expect(Error.Code == CheckpointStoreErrorCode::InvalidCheckpoint);
      Expect(std::get<CheckpointSnapshot>(Store.Load()).RecordCount == 0);
      RemoveIfExists(Path);
    }
  }
}

void TestToolDispatchStateCannotBeClearedOrHiddenOnLoad() {
  {
    const auto Path = TempPath("tool-state-monotonic-append");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnCheckpoint Checkpoint = MakeTurn("turn-tool-monotonic");
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ReadyToDispatch;
    Checkpoint.Revision = 2;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderDispatched;
    Checkpoint.RequestId = RequestToken("request-tool-monotonic");
    Checkpoint.Revision = 3;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderStreaming;
    Checkpoint.Revision = 4;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderCompleted;
    Checkpoint.Revision = 5;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.ToolState = ToolExecutionState::Authorized;
    Checkpoint.Revision = 6;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ToolDispatched;
    Checkpoint.ToolState = ToolExecutionState::Dispatched;
    Checkpoint.Revision = 7;
    Expect(Store.Append(Checkpoint).index() == 0);

    for (const ToolExecutionState ErasedState :
         {ToolExecutionState::NotRequested, ToolExecutionState::Authorized}) {
      TurnCheckpoint Erased = Checkpoint;
      Erased.Phase = TurnPhase::OutcomeUnknown;
      Erased.ToolState = ErasedState;
      Erased.UnknownSideEffect = true;
      Erased.Revision = 8;
      Expect(ExpectError(Store.Append(Erased)).Code ==
             CheckpointStoreErrorCode::InvalidTransition);
      const CheckpointSnapshot Snapshot =
          std::get<CheckpointSnapshot>(Store.Load());
      Expect(Snapshot.RecordCount == 7);
      Expect(Snapshot.LatestByTurn.at(TurnToken("turn-tool-monotonic"))
                 .ToolState == ToolExecutionState::Dispatched);
    }
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("tool-state-monotonic-load");
    const std::string Log =
        ToolStateTurnRecord("Created", {}, "NotRequested", 1) +
        ToolStateTurnRecord("ReadyToDispatch", {}, "NotRequested", 2) +
        ToolStateTurnRecord("ProviderDispatched", "request-tool-history",
                            "NotRequested", 3) +
        ToolStateTurnRecord("ProviderStreaming", "request-tool-history",
                            "NotRequested", 4) +
        ToolStateTurnRecord("ProviderCompleted", "request-tool-history",
                            "Authorized", 5) +
        ToolStateTurnRecord("ToolDispatched", "request-tool-history",
                            "Dispatched", 6) +
        ToolStateTurnRecord("OutcomeUnknown", "request-tool-history",
                            "NotRequested", 7, true);
    WriteAll(Path, Log);
    const CheckpointStoreError Error =
        ExpectLoadError(TurnCheckpointStore(Path).Load());
    Expect(Error.Code == CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
}

void TestToolExecutionCannotSkipAuthorizationOrDispatch() {
  {
    const auto Path = TempPath("tool-state-skipped-initial");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnCheckpoint Initial = MakeTurn("turn-tool-auth-initial");
    Initial.ToolState = ToolExecutionState::Completed;
    Expect(ExpectError(Store.Append(Initial)).Code ==
           CheckpointStoreErrorCode::InvalidTransition);
    Expect(std::get<CheckpointSnapshot>(Store.Load()).RecordCount == 0);

    const std::string Log =
        CompleteRecord("EDCP|2|T|" + TurnToken("turn-terminal") + "||" +
                       ScopeToken("scope-a") + "|" + ContextToken("context-a") +
                       "|Created|Completed|0|None||||1700000000123|1");
    WriteAll(Path, Log);
    Expect(ExpectLoadError(Store.Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }

  for (const ToolExecutionState SkippedState :
       {ToolExecutionState::Dispatched, ToolExecutionState::Completed,
        ToolExecutionState::Failed}) {
    const auto Path = TempPath("tool-state-skipped-authorization");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnCheckpoint Checkpoint = MakeTurn("turn-tool-auth-skip");
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ReadyToDispatch;
    Checkpoint.Revision = 2;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderDispatched;
    Checkpoint.RequestId = RequestToken("request-tool-auth-skip");
    Checkpoint.Revision = 3;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderCompleted;
    Checkpoint.Revision = 4;
    Expect(Store.Append(Checkpoint).index() == 0);

    TurnCheckpoint Skipped = Checkpoint;
    Skipped.ToolState = SkippedState;
    if (SkippedState == ToolExecutionState::Dispatched) {
      Skipped.Phase = TurnPhase::ToolDispatched;
    }
    Skipped.Revision = 5;
    Expect(ExpectError(Store.Append(Skipped)).Code ==
           CheckpointStoreErrorCode::InvalidTransition);
    const CheckpointSnapshot Snapshot =
        std::get<CheckpointSnapshot>(Store.Load());
    Expect(Snapshot.RecordCount == 4);
    Expect(
        Snapshot.LatestByTurn.at(TurnToken("turn-tool-auth-skip")).ToolState ==
        ToolExecutionState::NotRequested);
    RemoveIfExists(Path);
  }

  const auto Path = TempPath("tool-state-skipped-authorization-load");
  const std::string Log =
      TurnRecord("Created", {}, false, 1) +
      TurnRecord("ReadyToDispatch", {}, false, 2) +
      TurnRecord("ProviderDispatched", "request-tool-auth-skip-load", false,
                 3) +
      CompleteRecord("EDCP|2|T|" + TurnToken("turn-terminal") + "|" +
                     RequestToken("request-tool-auth-skip-load") + "|" +
                     ScopeToken("scope-a") + "|" + ContextToken("context-a") +
                     "|ProviderCompleted|Completed|0|None||||1700000000123|4");
  WriteAll(Path, Log);
  const CheckpointStoreError Error =
      ExpectLoadError(TurnCheckpointStore(Path).Load());
  Expect(Error.Code == CheckpointStoreErrorCode::CorruptRecord);
  RemoveIfExists(Path);
}

void TestCompletedToolOperationCanAdvanceToANewProviderCycle() {
  const auto Path = TempPath("multi-tool-cycle");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = MakeTurn("turn-multi-cycle");
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ReadyToDispatch;
  Checkpoint.Revision = 2;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderDispatched;
  Checkpoint.RequestId = RequestToken("request-cycle-one");
  Checkpoint.Revision = 3;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ProviderCompleted;
  Checkpoint.Revision = 4;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.ToolState = ToolExecutionState::Authorized;
  Checkpoint.Operation = OperationState::Prepared;
  Checkpoint.OperationId = OperationToken("operation-cycle-one");
  Checkpoint.IdempotencyKey = IdempotencyToken("idem-cycle-one");
  Checkpoint.ActionId = ActionToken("action-cycle-one");
  Checkpoint.Revision = 5;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Operation = OperationState::Authorized;
  Checkpoint.Revision = 6;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ToolDispatched;
  Checkpoint.ToolState = ToolExecutionState::Dispatched;
  Checkpoint.Operation = OperationState::Dispatched;
  Checkpoint.Revision = 7;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.ToolState = ToolExecutionState::Completed;
  Checkpoint.Operation = OperationState::Succeeded;
  Checkpoint.Revision = 8;
  Expect(Store.Append(Checkpoint).index() == 0);

  TurnCheckpoint IllegalRollback = Checkpoint;
  IllegalRollback.Phase = TurnPhase::ProviderDispatched;
  IllegalRollback.RequestId = RequestToken("request-cycle-two");
  IllegalRollback.ToolState = ToolExecutionState::NotRequested;
  IllegalRollback.Revision = 9;
  Expect(ExpectError(Store.Append(IllegalRollback)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  Checkpoint.Phase = TurnPhase::ProviderDispatched;
  Checkpoint.RequestId = RequestToken("request-cycle-two");
  Checkpoint.Revision = 9;
  Expect(Store.Append(Checkpoint).index() == 0);
  TurnCheckpoint InflightRollback = Checkpoint;
  InflightRollback.ToolState = ToolExecutionState::NotRequested;
  InflightRollback.Revision = 10;
  Expect(ExpectError(Store.Append(InflightRollback)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);
  Checkpoint.Phase = TurnPhase::ProviderStreaming;
  Checkpoint.Revision = 10;
  Expect(Store.Append(Checkpoint).index() == 0);
  TurnCheckpoint ReusedOperation = Checkpoint;
  ReusedOperation.Phase = TurnPhase::ProviderCompleted;
  ReusedOperation.Operation = OperationState::Prepared;
  ReusedOperation.Revision = 11;
  Expect(ExpectError(Store.Append(ReusedOperation)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  Checkpoint.Phase = TurnPhase::ProviderCompleted;
  Checkpoint.Operation = OperationState::Prepared;
  Checkpoint.OperationId = OperationToken("operation-cycle-two");
  Checkpoint.IdempotencyKey = IdempotencyToken("idem-cycle-two");
  Checkpoint.ActionId = ActionToken("action-cycle-one");
  Checkpoint.Revision = 11;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Operation = OperationState::Authorized;
  Checkpoint.ToolState = ToolExecutionState::Authorized;
  Checkpoint.Revision = 12;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Phase = TurnPhase::ToolDispatched;
  Checkpoint.Operation = OperationState::Dispatched;
  Checkpoint.ToolState = ToolExecutionState::Dispatched;
  Checkpoint.Revision = 13;
  Expect(Store.Append(Checkpoint).index() == 0);
  Checkpoint.Operation = OperationState::Succeeded;
  Checkpoint.ToolState = ToolExecutionState::Completed;
  Checkpoint.Revision = 14;
  Expect(Store.Append(Checkpoint).index() == 0);

  const CheckpointSnapshot Loaded = std::get<CheckpointSnapshot>(Store.Load());
  Expect(Loaded.RecordCount == 14);
  const TurnCheckpoint& Latest =
      Loaded.LatestByTurn.at(TurnToken("turn-multi-cycle"));
  Expect(Latest.Phase == TurnPhase::ToolDispatched);
  Expect(Latest.Operation == OperationState::Succeeded);
  Expect(Latest.OperationId == OperationToken("operation-cycle-two"));
  Expect(Latest.IdempotencyKey == IdempotencyToken("idem-cycle-two"));
  Expect(Latest.ActionId == ActionToken("action-cycle-one"));
  Expect(Latest.ToolState == ToolExecutionState::Completed);
  RemoveIfExists(Path);
}

void TestScopeMarkersAndTombstoneMonotonicity() {
  const auto Path = TempPath("scope");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);

  ScopeCheckpoint Revoked = MakeScope("scope-a", ScopeState::Revoked, 1);
  Expect(Store.MarkScope(Revoked).index() == 0);
  const auto BeforeRetry = std::get<CheckpointSnapshot>(Store.Load());
  Expect(BeforeRetry.RecordCount == 1);
  Expect(Store.MarkScope(Revoked).index() == 0);
  Expect(std::get<CheckpointSnapshot>(Store.Load()).RecordCount == 1);

  ScopeCheckpoint WrongRetry = Revoked;
  WrongRetry.Revision = 2;
  Expect(ExpectError(Store.MarkScope(WrongRetry)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  ScopeCheckpoint Deleted = MakeScope("scope-a", ScopeState::Deleted, 2);
  Expect(Store.MarkScope(Deleted).index() == 0);
  ScopeCheckpoint Tombstoned = MakeScope("scope-a", ScopeState::Tombstoned, 3);
  Expect(Store.MarkScope(Tombstoned).index() == 0);
  ScopeCheckpoint Reactivate = MakeScope("scope-a", ScopeState::Active, 4);
  Expect(ExpectError(Store.MarkScope(Reactivate)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  ScopeCheckpoint BadInitial = MakeScope("scope-b", ScopeState::Deleted, 2);
  Expect(ExpectError(Store.MarkScope(BadInitial)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);

  const auto Snapshot = std::get<CheckpointSnapshot>(Store.Load());
  Expect(Snapshot.LatestScopeById.at(ScopeToken("scope-a")).State ==
         ScopeState::Tombstoned);
  RemoveIfExists(Path);
}

void TestValidationRejectsUnsafeInputs() {
  const auto Path = TempPath("validation");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);

  TurnCheckpoint EmptyTurn = MakeTurn("");
  Expect(ExpectError(Store.Append(EmptyTurn)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);
  TurnCheckpoint NewlineTurn = MakeTurn("turn-newline");
  NewlineTurn.TurnId = "turn\nbody";
  Expect(ExpectError(Store.Append(NewlineTurn)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);
  TurnCheckpoint LongTurn = MakeTurn("turn-long");
  LongTurn.TurnId = std::string(129, 'A');
  Expect(ExpectError(Store.Append(LongTurn)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);
  TurnCheckpoint InvalidStart = MakeTurn("turn-invalid-start");
  InvalidStart.TurnId = "-turn";
  Expect(ExpectError(Store.Append(InvalidStart)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);

  TurnCheckpoint Dispatched = MakeTurn("turn-dispatched");
  Dispatched.Phase = TurnPhase::ProviderDispatched;
  Expect(ExpectError(Store.Append(Dispatched)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);
  Dispatched.RequestId = RequestToken("request-a");
  Dispatched.ContextReference = {};
  Expect(ExpectError(Store.Append(Dispatched)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);
  Dispatched.ContextReference = ContextToken("context-a");
  Dispatched.Version = 2;
  Expect(ExpectError(Store.Append(Dispatched)).Code ==
         CheckpointStoreErrorCode::UnsupportedVersion);

  TurnCheckpoint ToolTooEarly = MakeTurn("turn-tool-early");
  ToolTooEarly.Phase = TurnPhase::ReadyToDispatch;
  ToolTooEarly.ToolState = ToolExecutionState::Dispatched;
  ToolTooEarly.ContextReference = ContextToken("context-a");
  Expect(ExpectError(Store.Append(ToolTooEarly)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);

  TurnCheckpoint UnknownTool = MakeTurn("turn-tool-unknown");
  UnknownTool.Phase = TurnPhase::ProviderCompleted;
  UnknownTool.RequestId = RequestToken("request-a");
  UnknownTool.ToolState = ToolExecutionState::UnknownOutcome;
  Expect(ExpectError(Store.Append(UnknownTool)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);

  TurnCheckpoint UnknownOutcome = MakeTurn("turn-unknown");
  UnknownOutcome.Phase = TurnPhase::OutcomeUnknown;
  UnknownOutcome.RequestId = RequestToken("request-a");
  UnknownOutcome.UnknownSideEffect = true;
  UnknownOutcome.ContextReference = {};
  Expect(ExpectError(Store.Append(UnknownOutcome)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);

  ScopeCheckpoint InvalidScope =
      MakeScope("scope with body", ScopeState::Revoked, 1);
  InvalidScope.ScopeRef = "private-body";
  Expect(ExpectError(Store.MarkScope(InvalidScope)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);
  ScopeCheckpoint ZeroRevision =
      MakeScope("scope-zero", ScopeState::Revoked, 0);
  Expect(ExpectError(Store.MarkScope(ZeroRevision)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);

  Expect(!std::filesystem::exists(Path));
  RemoveIfExists(Path);
}

void TestCorruptTruncatedAndUnsupportedRecordsFailClosed() {
  {
    const auto Path = TempPath("garbage");
    WriteAll(Path, "not-a-checkpoint\n");
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("checksum");
    std::string Record =
        CompleteRecord("EDCP|1|S|" + ScopeToken("scope-a") + "|Active|0|1");
    Record[Record.size() - 18U] = 'x';
    WriteAll(Path, Record);
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("version");
    WriteAll(Path, CompleteRecord("EDCP|2|S|" + ScopeToken("scope-a") +
                                  "|Active|0|1"));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::UnsupportedVersion);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("kind");
    WriteAll(Path, CompleteRecord("EDCP|1|X|" + ScopeToken("scope-a") +
                                  "|Active|0|1"));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("truncated");
    std::string Record =
        CompleteRecord("EDCP|1|S|" + ScopeToken("scope-a") + "|Active|0|1");
    Record.pop_back();
    WriteAll(Path, Record);
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("oversized");
    WriteAll(Path, std::string(2100, 'A'));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("blank");
    WriteAll(Path, "\n");
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
}

void TestCorruptFieldRangesAndRevisionHistory() {
  {
    const auto Path = TempPath("bad-time");
    WriteAll(Path, CompleteRecord("EDCP|1|S|" + ScopeToken("scope-a") +
                                  "|Active|not-a-time|1"));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("bad-phase");
    WriteAll(Path, CompleteRecord("EDCP|1|T|" + TurnToken("turn-a") + "||" +
                                  ScopeToken("scope-a") + "|" +
                                  ContextToken("context-a") +
                                  "|FuturePhase|NotRequested|0|0|1"));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("revision-gap");
    WriteAll(Path, CompleteRecord("EDCP|1|T|" + TurnToken("turn-a") + "||" +
                                  ScopeToken("scope-a") + "|" +
                                  ContextToken("context-a") +
                                  "|Created|NotRequested|0|0|1") +
                       CompleteRecord("EDCP|1|T|" + TurnToken("turn-a") + "||" +
                                      ScopeToken("scope-a") + "|" +
                                      ContextToken("context-a") +
                                      "|ReadyToDispatch|NotRequested|0|0|3"));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("scope-history");
    WriteAll(Path, CompleteRecord("EDCP|1|S|" + ScopeToken("scope-a") +
                                  "|Active|0|1") +
                       CompleteRecord("EDCP|1|S|" + ScopeToken("scope-a") +
                                      "|Active|0|2"));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
}

void TestIoFailureAndMissingFile() {
  {
    const auto Path = TempPath("missing");
    RemoveIfExists(Path);
    const auto Loaded = TurnCheckpointStore(Path).Load();
    Expect(std::holds_alternative<CheckpointSnapshot>(Loaded));
    Expect(std::get<CheckpointSnapshot>(Loaded).RecordCount == 0);
  }
  {
    const auto Directory = TempPath("directory");
    RemoveIfExists(Directory);
    std::error_code Error;
    std::filesystem::create_directories(Directory, Error);
    Expect(!Error);
    TurnCheckpointStore Store(Directory);
    Expect(ExpectError(Store.Append(MakeTurn("turn-io"))).Code ==
           CheckpointStoreErrorCode::IoFailure);
    Expect(ExpectLoadError(Store.Load()).Code ==
           CheckpointStoreErrorCode::IoFailure);
    RemoveIfExists(Directory);
  }
}

void TestPersistedFormatHasNoMessageOrSecretFields() {
  const auto Path = TempPath("sanitized");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = MakeTurn("turn-sanitized");
  Checkpoint.RequestId = RequestToken("request-sanitized");
  Checkpoint.ContextReference = ContextToken("context-sanitized");
  Expect(Store.Append(Checkpoint).index() == 0);
  const std::string Raw = ReadAll(Path);
  Expect(Raw.find("SECRET_SENTINEL") == std::string::npos);
  Expect(Raw.find("private body text") == std::string::npos);
  Expect(Raw.find("Authorization") == std::string::npos);
  Expect(Raw.find("prompt") == std::string::npos);
  Expect(Raw.find("tool-arguments") == std::string::npos);
  Expect(Raw.find('\n') != std::string::npos);
  RemoveIfExists(Path);
}

std::string RechecksumLine(std::string Line) {
  if (!Line.empty() && Line.back() == '\n') {
    Line.pop_back();
  }
  const std::size_t Separator = Line.rfind('|');
  Expect(Separator != std::string::npos);
  const std::string Payload = Line.substr(0, Separator);
  return Payload + "|" + Checksum(Fnv1a(Payload)) + "\n";
}

void TestOperationIdentityAndIdempotencyPersistence() {
  const auto Path = TempPath("operation");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  TurnCheckpoint Checkpoint = MakeTurn("turn-operation");
  Checkpoint.ContextReference = ContextToken("context-operation");
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
  Expect(Store.Append(Checkpoint).index() == 0);

  const CheckpointSnapshot Snapshot =
      std::get<CheckpointSnapshot>(Store.Load());
  const TurnCheckpoint& Latest =
      Snapshot.LatestByTurn.at(TurnToken("turn-operation"));
  Expect(Latest.Operation == OperationState::Dispatched);
  Expect(Latest.OperationId == OperationToken("operation-1"));
  Expect(Latest.IdempotencyKey == IdempotencyToken("idempotency-1"));
  Expect(Latest.ActionId == ActionToken("action-1"));

  const CheckpointSnapshot Restarted =
      std::get<CheckpointSnapshot>(TurnCheckpointStore(Path).Load());
  Expect(
      Restarted.LatestByTurn.at(TurnToken("turn-operation")).IdempotencyKey ==
      IdempotencyToken("idempotency-1"));

  TurnCheckpoint DriftedOperation = Checkpoint;
  DriftedOperation.Revision = 5;
  DriftedOperation.Phase = TurnPhase::OutcomeUnknown;
  DriftedOperation.UnknownSideEffect = true;
  DriftedOperation.Operation = OperationState::UnknownOutcome;
  DriftedOperation.OperationId = OperationToken("operation-2");
  Expect(ExpectError(Store.Append(DriftedOperation)).Code ==
         CheckpointStoreErrorCode::InvalidTransition);
  RemoveIfExists(Path);
}

void TestCrossRevisionCorrelationDriftRejected() {
  {
    const auto Path = TempPath("context-drift");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnCheckpoint Checkpoint = MakeTurn("turn-context-drift");
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ReadyToDispatch;
    Checkpoint.Revision = 2;
    Checkpoint.ContextReference = ContextToken("context-b");
    Expect(ExpectError(Store.Append(Checkpoint)).Code ==
           CheckpointStoreErrorCode::InvalidTransition);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("request-drift");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnCheckpoint Checkpoint = MakeTurn("turn-request-drift");
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ReadyToDispatch;
    Checkpoint.Revision = 2;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderDispatched;
    Checkpoint.RequestId = RequestToken("request-a");
    Checkpoint.Revision = 3;
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::ProviderStreaming;
    Checkpoint.RequestId = RequestToken("request-b");
    Checkpoint.Revision = 4;
    Expect(ExpectError(Store.Append(Checkpoint)).Code ==
           CheckpointStoreErrorCode::InvalidTransition);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("tampered-history");
    const std::string First =
        CompleteRecord("EDCP|2|T|" + TurnToken("turn-tampered") + "||" +
                       ScopeToken("scope-a") + "|" + ContextToken("context-a") +
                       "|Created|NotRequested|0|None||||1700000000000|1");
    const std::string Second = CompleteRecord(
        "EDCP|2|T|" + TurnToken("turn-tampered") + "||" +
        ScopeToken("scope-a") + "|" + ContextToken("context-b") +
        "|ReadyToDispatch|NotRequested|0|None||||1700000000001|2");
    WriteAll(Path, RechecksumLine(First) + RechecksumLine(Second));
    Expect(ExpectLoadError(TurnCheckpointStore(Path).Load()).Code ==
           CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
}

void TestSecretSentinelRejectedAcrossPersistedFields() {
  const auto Path = TempPath("secret-fields");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  const auto ExpectRejected = [&Store](auto Mutate) {
    TurnCheckpoint Checkpoint = MakeTurn("turn-secret");
    Mutate(Checkpoint);
    Expect(ExpectError(Store.Append(Checkpoint)).Code ==
           CheckpointStoreErrorCode::InvalidCheckpoint);
  };
  const std::string Secret = "SECRET_SENTINEL";
  ExpectRejected([&Secret](TurnCheckpoint& C) { C.TurnId = Secret; });
  ExpectRejected([&Secret](TurnCheckpoint& C) { C.RequestId = Secret; });
  ExpectRejected([&Secret](TurnCheckpoint& C) { C.ScopeRef = Secret; });
  ExpectRejected([&Secret](TurnCheckpoint& C) { C.ContextReference = Secret; });
  ExpectRejected([&Secret](TurnCheckpoint& C) {
    C.Operation = OperationState::Prepared;
    C.OperationId = Secret;
    C.IdempotencyKey = IdempotencyToken("idempotency-ok");
    C.ActionId = ActionToken("action-ok");
  });
  ExpectRejected([&Secret](TurnCheckpoint& C) {
    C.Operation = OperationState::Prepared;
    C.OperationId = OperationToken("operation-ok");
    C.IdempotencyKey = Secret;
    C.ActionId = ActionToken("action-ok");
  });
  ExpectRejected([&Secret](TurnCheckpoint& C) {
    C.Operation = OperationState::Authorized;
    C.OperationId = OperationToken("operation-ok");
    C.IdempotencyKey = IdempotencyToken("idempotency-ok");
    C.ActionId = Secret;
  });
  ScopeCheckpoint Scope = MakeScope("scope-a", ScopeState::Revoked, 1);
  Scope.ScopeRef = Secret;
  Expect(ExpectError(Store.MarkScope(Scope)).Code ==
         CheckpointStoreErrorCode::InvalidCheckpoint);
  const std::vector<std::string> UntrustedValues{
      "private-body", "Bearer sk-test-token-value",
      "eyJhbGciOiJIUzI1NiJ9.payload.signature"};
  for (const std::string& Untrusted : UntrustedValues) {
    ExpectRejected([&Untrusted](TurnCheckpoint& C) { C.TurnId = Untrusted; });
    ExpectRejected([&Untrusted](TurnCheckpoint& C) { C.ScopeRef = Untrusted; });
    ExpectRejected(
        [&Untrusted](TurnCheckpoint& C) { C.ContextReference = Untrusted; });
    ExpectRejected(
        [&Untrusted](TurnCheckpoint& C) { C.RequestId = Untrusted; });
    ExpectRejected([&Untrusted](TurnCheckpoint& C) {
      C.OperationId = Untrusted;
      C.Operation = OperationState::Prepared;
      C.IdempotencyKey = IdempotencyToken("safe-idempotency");
      C.ActionId = ActionToken("safe-action");
    });
    ExpectRejected([&Untrusted](TurnCheckpoint& C) {
      C.Operation = OperationState::Prepared;
      C.OperationId = OperationToken("safe-operation");
      C.IdempotencyKey = Untrusted;
      C.ActionId = ActionToken("safe-action");
    });
    ExpectRejected([&Untrusted](TurnCheckpoint& C) {
      C.Operation = OperationState::Prepared;
      C.OperationId = OperationToken("safe-operation");
      C.IdempotencyKey = IdempotencyToken("safe-idempotency");
      C.ActionId = Untrusted;
    });
  }
  Expect(!std::filesystem::exists(Path));
  RemoveIfExists(Path);
}

void TestConcurrentWritersSerializeRevisions() {
  const auto Path = TempPath("concurrent");
  RemoveIfExists(Path);
  TurnCheckpointStore Store(Path);
  Expect(Store.Append(MakeTurn("turn-concurrent")).index() == 0);
  constexpr int ThreadCount = 8;
  std::atomic<int> Successes{0};
  std::atomic<int> InvalidTransitions{0};
  std::vector<std::thread> Threads;
  for (int Index = 0; Index < ThreadCount; ++Index) {
    Threads.emplace_back([&Store, &Successes, &InvalidTransitions, Index] {
      TurnCheckpoint Checkpoint = MakeTurn("turn-concurrent");
      Checkpoint.Phase = TurnPhase::ReadyToDispatch;
      Checkpoint.Revision = 2;
      Checkpoint.RequestId = RequestToken("request-" + std::to_string(Index));
      const CheckpointResult<std::monostate> Result = Store.Append(Checkpoint);
      if (Result.index() == 0) {
        ++Successes;
      } else if (std::get<CheckpointStoreError>(Result).Code ==
                 CheckpointStoreErrorCode::InvalidTransition) {
        ++InvalidTransitions;
      }
    });
  }
  for (std::thread& Thread : Threads) {
    Thread.join();
  }
  Expect(Successes == 1);
  Expect(InvalidTransitions == ThreadCount - 1);
  const CheckpointSnapshot Snapshot =
      std::get<CheckpointSnapshot>(Store.Load());
  Expect(Snapshot.LatestByTurn.at(TurnToken("turn-concurrent")).Revision == 2);
  Expect(Snapshot.RecordCount == 2);
  RemoveIfExists(Path);
}

void TestUnsupportedTerminalAndLegacyRecord() {
  {
    const auto Path = TempPath("unsupported");
    RemoveIfExists(Path);
    TurnCheckpointStore Store(Path);
    TurnCheckpoint Checkpoint = MakeTurn("turn-unsupported");
    Expect(Store.Append(Checkpoint).index() == 0);
    Checkpoint.Phase = TurnPhase::Unsupported;
    Checkpoint.Revision = 2;
    Expect(Store.Append(Checkpoint).index() == 0);
    Expect(TurnCheckpointStore::IsTerminal(TurnPhase::Unsupported));
    Checkpoint.Phase = TurnPhase::Completed;
    Checkpoint.Revision = 3;
    Checkpoint.RequestId = RequestToken("request-after-terminal");
    Expect(ExpectError(Store.Append(Checkpoint)).Code ==
           CheckpointStoreErrorCode::InvalidTransition);
    RemoveIfExists(Path);
  }
  {
    const auto Path = TempPath("legacy-turn");
    const std::string Legacy =
        CompleteRecord("EDCP|1|T|" + TurnToken("turn-legacy") + "||" +
                       ScopeToken("scope-a") + "|" + ContextToken("context-a") +
                       "|Created|NotRequested|0|1700000000000|1");
    WriteAll(Path, Legacy);
    const CheckpointSnapshot Snapshot =
        std::get<CheckpointSnapshot>(TurnCheckpointStore(Path).Load());
    const TurnCheckpoint& Latest =
        Snapshot.LatestByTurn.at(TurnToken("turn-legacy"));
    Expect(Latest.Operation == OperationState::None);
    Expect(Latest.OperationId.empty());
    RemoveIfExists(Path);
  }
}

void TestChecksummedLogRejectsRecordsAfterTerminal() {
  const std::string TerminalLog =
      TurnRecord("Created", {}, false, 1) +
      TurnRecord("ReadyToDispatch", {}, false, 2) +
      TurnRecord("ProviderDispatched", "request-terminal", false, 3) +
      TurnRecord("ProviderCompleted", "request-terminal", false, 4) +
      TurnRecord("Completed", "request-terminal", false, 5);

  for (const auto& [Phase, UnknownSideEffect] :
       std::vector<std::pair<std::string_view, bool>>{
           {"Failed", false}, {"OutcomeUnknown", true}}) {
    const auto Path = TempPath("terminal-reopen-" + std::string(Phase));
    WriteAll(Path, TerminalLog);

    const CheckpointSnapshot BeforeAppend =
        std::get<CheckpointSnapshot>(TurnCheckpointStore(Path).Load());
    const TurnCheckpoint& Terminal =
        BeforeAppend.LatestByTurn.at(TurnToken("turn-terminal"));
    Expect(Terminal.Phase == TurnPhase::Completed);
    Expect(Terminal.Revision == 5);

    WriteAll(Path, TerminalLog + TurnRecord(Phase, "request-terminal",
                                            UnknownSideEffect, 6));
    const CheckpointStoreError Error =
        ExpectLoadError(TurnCheckpointStore(Path).Load());
    Expect(Error.Code == CheckpointStoreErrorCode::CorruptRecord);
    RemoveIfExists(Path);
  }
}

}  // namespace

int main() {
  TestRoundTripRestartAndIdempotentAppend();
  TestRevisionIdentityAndTerminalRules();
  TestDeterminateTerminalRejectsPendingDispatches();
  TestToolDispatchStateCannotBeClearedOrHiddenOnLoad();
  TestToolExecutionCannotSkipAuthorizationOrDispatch();
  TestCompletedToolOperationCanAdvanceToANewProviderCycle();
  TestScopeMarkersAndTombstoneMonotonicity();
  TestValidationRejectsUnsafeInputs();
  TestCorruptTruncatedAndUnsupportedRecordsFailClosed();
  TestCorruptFieldRangesAndRevisionHistory();
  TestIoFailureAndMissingFile();
  TestPersistedFormatHasNoMessageOrSecretFields();
  TestOperationIdentityAndIdempotencyPersistence();
  TestCrossRevisionCorrelationDriftRejected();
  TestSecretSentinelRejectedAcrossPersistedFields();
  TestConcurrentWritersSerializeRevisions();
  TestUnsupportedTerminalAndLegacyRecord();
  TestChecksummedLogRejectsRecordsAfterTerminal();
  std::puts("TurnCheckpointStore contract tests passed");
  return 0;
}
