#include "WorkflowController.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace EmulativeDialog::WorkflowController {
namespace {

std::string RequiredString(const YAML::Node& Node, const std::string& Key) {
  const YAML::Node Value = Node[Key];
  if (!Value || !Value.IsScalar()) {
    throw WorkflowError("Missing or invalid TaskSpec field: " + Key);
  }
  return Value.as<std::string>();
}

std::string OptionalString(const YAML::Node& Node,
                           const std::string& Key,
                           const std::string& Fallback = {}) {
  const YAML::Node Value = Node[Key];
  if (!Value || Value.IsNull()) {
    return Fallback;
  }
  if (!Value.IsScalar()) {
    throw WorkflowError("Invalid scalar field: " + Key);
  }
  return Value.as<std::string>();
}

std::vector<std::string> StringSequence(const YAML::Node& Node,
                                        const std::string& Key) {
  const YAML::Node Value = Node[Key];
  if (!Value || Value.IsNull()) {
    return {};
  }
  if (!Value.IsSequence()) {
    throw WorkflowError("Expected a sequence for field: " + Key);
  }
  std::vector<std::string> Result;
  Result.reserve(Value.size());
  for (const YAML::Node& Item : Value) {
    if (!Item.IsScalar()) {
      throw WorkflowError("Expected scalar entries in field: " + Key);
    }
    Result.push_back(Item.as<std::string>());
  }
  return Result;
}

std::string OptionalEventString(const YAML::Node& Node,
                                const std::string& PascalKey,
                                const std::string& SnakeKey) {
  if (Node[PascalKey]) {
    return OptionalString(Node, PascalKey);
  }
  return OptionalString(Node, SnakeKey);
}

int EventInteger(const YAML::Node& Node,
                 const std::string& PascalKey,
                 const std::string& SnakeKey,
                 int Fallback = 0) {
  const YAML::Node Value = Node[PascalKey] ? Node[PascalKey] : Node[SnakeKey];
  return Value && !Value.IsNull() ? Value.as<int>() : Fallback;
}

std::size_t EventSize(const YAML::Node& Node,
                      const std::string& PascalKey,
                      const std::string& SnakeKey,
                      std::size_t Fallback = 0) {
  const YAML::Node Value = Node[PascalKey] ? Node[PascalKey] : Node[SnakeKey];
  return Value && !Value.IsNull() ? Value.as<std::size_t>() : Fallback;
}

long EventLong(const YAML::Node& Node,
               const std::string& PascalKey,
               const std::string& SnakeKey,
               long Fallback = 0) {
  const YAML::Node Value = Node[PascalKey] ? Node[PascalKey] : Node[SnakeKey];
  return Value && !Value.IsNull() ? Value.as<long>() : Fallback;
}

std::string LowerAscii(std::string Value) {
  std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char C) {
    return static_cast<char>(std::tolower(C));
  });
  return Value;
}

std::wstring LowerWide(std::wstring Value) {
  std::transform(Value.begin(), Value.end(), Value.begin(), [](wchar_t C) {
    return static_cast<wchar_t>(std::towlower(C));
  });
  return Value;
}

bool IsWithinPath(const std::filesystem::path& Candidate,
                  const std::filesystem::path& Root) {
  const std::wstring CandidateText =
      LowerWide(Candidate.lexically_normal().native());
  std::wstring RootText = LowerWide(Root.lexically_normal().native());
  if (CandidateText == RootText) {
    return true;
  }
  if (!RootText.empty() && RootText.back() != L'\\' && RootText.back() != L'/') {
    RootText.push_back(L'\\');
  }
  return CandidateText.starts_with(RootText);
}

std::filesystem::path FinalPathForExistingPath(const std::filesystem::path& ExistingPath) {
  HANDLE Handle = CreateFileW(ExistingPath.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (Handle == INVALID_HANDLE_VALUE) {
    throw WorkflowError("Unable to resolve existing or reparse path: " + ExistingPath.string());
  }
  const DWORD Required = GetFinalPathNameByHandleW(Handle, nullptr, 0,
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (Required == 0) {
    CloseHandle(Handle);
    throw WorkflowError("GetFinalPathNameByHandleW failed for: " + ExistingPath.string());
  }
  std::vector<wchar_t> Buffer(static_cast<std::size_t>(Required) + 1, L'\0');
  const DWORD Length = GetFinalPathNameByHandleW(Handle, Buffer.data(),
                                                static_cast<DWORD>(Buffer.size()),
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(Handle);
  if (Length == 0 || Length >= Buffer.size()) {
    throw WorkflowError("Unable to retrieve the canonical path for: " + ExistingPath.string());
  }
  std::wstring FinalText(Buffer.data(), Length);
  if (FinalText.starts_with(L"\\\\?\\UNC\\")) {
    FinalText = L"\\\\" + FinalText.substr(8);
  } else if (FinalText.starts_with(L"\\\\?\\")) {
    FinalText.erase(0, 4);
  }
  return std::filesystem::path(FinalText).lexically_normal();
}

std::filesystem::path CanonicalExistingDirectory(
    const std::filesystem::path& Directory) {
  std::error_code Error;
  if (!std::filesystem::is_directory(Directory, Error) || Error) {
    throw WorkflowError("Directory does not exist or is not accessible: " +
                        Directory.string());
  }
  return FinalPathForExistingPath(Directory);
}

std::filesystem::path CanonicalizeFuturePath(const std::filesystem::path& InputPath) {
  std::error_code Error;
  const auto AbsolutePath = std::filesystem::absolute(InputPath, Error).lexically_normal();
  if (Error) {
    throw WorkflowError("Unable to make output path absolute: " + InputPath.string());
  }
  std::filesystem::path ExistingPrefix = AbsolutePath;
  std::vector<std::filesystem::path> MissingParts;
  while (!std::filesystem::exists(ExistingPrefix, Error)) {
    if (Error || ExistingPrefix.empty() || ExistingPrefix == ExistingPrefix.root_path()) {
      throw WorkflowError("Unable to resolve an existing parent for output path: " +
                          InputPath.string());
    }
    MissingParts.push_back(ExistingPrefix.filename());
    ExistingPrefix = ExistingPrefix.parent_path();
  }
  auto Canonical = FinalPathForExistingPath(ExistingPrefix);
  for (auto It = MissingParts.rbegin(); It != MissingParts.rend(); ++It) {
    Canonical /= *It;
  }
  return Canonical.lexically_normal();
}

std::string NormalizeRelativePath(const std::string& RelativePath) {
  if (RelativePath.empty()) {
    throw WorkflowError("Path must not be empty");
  }
  std::filesystem::path Path(RelativePath);
  if (Path.is_absolute() || Path.has_root_name() || Path.has_root_directory()) {
    throw WorkflowError("Absolute paths are not allowed by a TaskSpec: " +
                        RelativePath);
  }
  for (const auto& Part : Path) {
    if (Part == "..") {
      throw WorkflowError("Parent traversal is not allowed by a TaskSpec: " +
                          RelativePath);
    }
  }
  const std::string Normalized = Path.lexically_normal().generic_string();
  if (Normalized.empty() || Normalized == ".") {
    throw WorkflowError("Path resolves to an empty or root path");
  }
  return Normalized;
}

std::filesystem::path ResolveRepoPath(
    const std::filesystem::path& RepositoryRoot,
    const std::string& RelativePath) {
  const std::string Normalized = NormalizeRelativePath(RelativePath);
  const auto Root = CanonicalExistingDirectory(RepositoryRoot);
  const auto Candidate = CanonicalizeFuturePath(Root / Normalized);
  if (!IsWithinPath(Candidate, Root) || Candidate == Root) {
    throw WorkflowError("Path escapes the repository root: " + RelativePath);
  }
  return Candidate;
}

YAML::Node LoadYaml(const std::filesystem::path& Path) {
  std::ifstream Input(Path, std::ios::binary);
  if (!Input) {
    throw WorkflowError("Unable to open YAML file: " + Path.string());
  }
  const std::string Contents((std::istreambuf_iterator<char>(Input)),
                             std::istreambuf_iterator<char>());
  try {
    return YAML::Load(Contents);
  } catch (const YAML::Exception& Error) {
    throw WorkflowError("Invalid YAML in " + Path.string() + ": " + Error.what());
  }
}

std::filesystem::path ResolveTaskPlanPath(
    const std::filesystem::path& RepositoryRoot,
    const std::filesystem::path& TaskIndexPath,
    const std::string& PhaseId) {
  const auto IndexPath = ResolveRepoPath(RepositoryRoot, TaskIndexPath.generic_string());
  const YAML::Node Index = LoadYaml(IndexPath);
  const YAML::Node Plans = Index["PhasePlans"];
  if (!Plans || !Plans.IsSequence()) {
    throw WorkflowError("TaskIndex has no PhasePlans sequence");
  }
  for (const YAML::Node& Plan : Plans) {
    if (OptionalString(Plan, "PhaseId") != PhaseId) {
      continue;
    }
    const std::string RelativePath = OptionalString(Plan, "Path");
    if (RelativePath.empty()) {
      throw WorkflowError("The routed phase is not expanded: " + PhaseId);
    }
    return ResolveRepoPath(RepositoryRoot, RelativePath);
  }
  throw WorkflowError("Phase is not routed by TaskIndex: " + PhaseId);
}

YAML::Node FindTaskNode(const YAML::Node& PhasePlan,
                        const std::string& TaskId) {
  const YAML::Node Tasks = PhasePlan["Tasks"];
  if (!Tasks || !Tasks.IsSequence()) {
    throw WorkflowError("Routed phase plan has no Tasks sequence");
  }
  for (const YAML::Node& Candidate : Tasks) {
    if (OptionalString(Candidate, "TaskId") == TaskId) {
      return Candidate;
    }
  }
  throw WorkflowError("TaskId is absent from its routed phase plan: " + TaskId);
}

void AppendValidationCommands(const YAML::Node& Validation,
                              const std::string& Key,
                              std::vector<std::string>& Commands) {
  const YAML::Node Values = Validation[Key];
  if (!Values || Values.IsNull()) {
    return;
  }
  if (!Values.IsSequence()) {
    throw WorkflowError("Validation field must be a sequence: " + Key);
  }
  for (const YAML::Node& Value : Values) {
    if (!Value.IsScalar()) {
      throw WorkflowError("Validation command must be a scalar: " + Key);
    }
    Commands.push_back(Value.as<std::string>());
  }
}

TaskSpec ParseTaskSpec(const YAML::Node& Node, const std::string& PhaseId) {
  TaskSpec Task;
  Task.TaskId = RequiredString(Node, "TaskId");
  Task.TaskRevision = Node["TaskRevision"].as<int>(0);
  if (Task.TaskRevision <= 0) {
    throw WorkflowError("TaskRevision must be a positive integer: " + Task.TaskId);
  }
  Task.PhaseId = PhaseId;
  Task.Title = OptionalString(Node, "Title");
  Task.Goal = OptionalString(Node, "Goal");
  Task.State = RequiredString(Node, "State");
  Task.OwnerAgentId = OptionalString(Node, "OwnerAgentId");
  Task.Branch = OptionalString(Node, "Branch");
  Task.Worktree = OptionalString(Node, "Worktree");
  Task.BaselineCommit = OptionalString(Node, "BaselineCommit");
  const YAML::Node Deliverable = Node["Deliverable"];
  Task.EventShard = Deliverable ? OptionalString(Deliverable, "EventShard") : std::string{};
  Task.DependsOn = StringSequence(Node, "DependsOn");
  Task.ReadSet = StringSequence(Node, "ReadSet");
  if (Task.ReadSet.empty() && Node["Context"]) {
    Task.ReadSet = StringSequence(Node["Context"], "ReadSet");
  }
  Task.WriteSet = StringSequence(Node, "WriteSet");
  Task.ForbiddenSet = StringSequence(Node, "ForbiddenSet");
  Task.SharedLocks = StringSequence(Node, "SharedLocks");

  const YAML::Node Context = Node["Context"];
  const YAML::Node Budget = Context ? Context["Budget"] : YAML::Node{};
  const YAML::Node MaxReferenceFiles = Budget ? Budget["MaxReferenceFiles"] : YAML::Node{};
  if (MaxReferenceFiles && !MaxReferenceFiles.IsNull()) {
    Task.MaxReferenceFiles = MaxReferenceFiles.as<std::size_t>();
  }
  const YAML::Node BudgetOverride = Node["ContextBudgetOverride"];
  if (BudgetOverride && !BudgetOverride.IsNull()) {
    ContextBudgetOverride Override;
    Override.TaskId = RequiredString(BudgetOverride, "TaskId");
    Override.TaskRevision = BudgetOverride["TaskRevision"].as<int>(0);
    Override.MaxReferenceFiles = BudgetOverride["MaxReferenceFiles"].as<std::size_t>(0);
    Override.ApprovedByRole = OptionalString(BudgetOverride, "ApprovedByRole");
    Override.ApprovalEventId = OptionalString(BudgetOverride, "ApprovalEventId");
    Override.Reason = OptionalString(BudgetOverride, "Reason");
    Task.RegisteredBudgetOverride = std::move(Override);
  }

  const YAML::Node Validation = Node["Validation"];
  if (Validation) {
    AppendValidationCommands(Validation, "TargetedBuild", Task.ValidationCommands);
    AppendValidationCommands(Validation, "ContractFixtures", Task.ValidationCommands);
  }
  return Task;
}

bool PathPatternMatches(const std::string& PatternValue,
                        const std::string& CandidateValue) {
  std::string UnwrappedPattern = PatternValue;
  const std::string Candidate = LowerAscii(NormalizeRelativePath(CandidateValue));
  bool DescendantsOnly = UnwrappedPattern.ends_with("/") ||
                         UnwrappedPattern.ends_with("\\") ||
                         UnwrappedPattern.ends_with("/**") ||
                         UnwrappedPattern.ends_with("\\**");
  if (UnwrappedPattern.ends_with("/**") || UnwrappedPattern.ends_with("\\**")) {
    UnwrappedPattern.resize(UnwrappedPattern.size() - 3);
  } else if (UnwrappedPattern.ends_with("/") || UnwrappedPattern.ends_with("\\")) {
    UnwrappedPattern.pop_back();
  }
  const std::string Pattern = LowerAscii(NormalizeRelativePath(UnwrappedPattern));
  if (Candidate == Pattern) {
    return !DescendantsOnly;
  }
  return Candidate.starts_with(Pattern + "/");
}

std::string RequiredSharedLock(const std::string& RelativePath) {
  const std::string Path = LowerAscii(NormalizeRelativePath(RelativePath));
  if (Path == "cmakelists.txt" || Path == "cmakepresets.json" ||
      Path == "cmake" || Path.starts_with("cmake/")) {
    return "RootCMakeAndPresets";
  }
  return {};
}

std::vector<std::string> RequiredSharedLocks(const std::string& RelativePath) {
  std::vector<std::string> Locks;
  const std::string Path = LowerAscii(NormalizeRelativePath(RelativePath));
  const std::string RootLock = RequiredSharedLock(RelativePath);
  if (!RootLock.empty()) {
    Locks.push_back(RootLock);
  }
  if (Path == "cmake/dependencypins.cmake") {
    Locks.push_back("DependencyLock");
  }
  return Locks;
}

bool HasSharedLock(const TaskSpec& Task, const std::string& Lock) {
  return std::find(Task.SharedLocks.begin(), Task.SharedLocks.end(), Lock) !=
         Task.SharedLocks.end();
}

bool IsCommitId(const std::string& Value) {
  if (Value.size() != 40) {
    return false;
  }
  return std::all_of(Value.begin(), Value.end(), [](unsigned char C) {
    return std::isxdigit(C) != 0;
  });
}

bool IsTaskId(const std::string& Value) {
  if (Value.size() != 7 || Value.rfind("ED-", 0) != 0) {
    return false;
  }
  return std::all_of(Value.begin() + 3, Value.end(), [](unsigned char C) {
    return std::isdigit(C) != 0;
  });
}

bool IsAllowedBranch(const std::string& TaskId, const std::string& Branch) {
  const std::string Prefix = "Task/" + TaskId + "-";
  if (!Branch.starts_with(Prefix) || Branch.size() <= Prefix.size()) {
    return false;
  }
  const std::string Topic = Branch.substr(Prefix.size());
  if (!std::isupper(static_cast<unsigned char>(Topic.front()))) {
    return false;
  }
  return std::all_of(Topic.begin(), Topic.end(), [](unsigned char C) {
    return std::isalnum(C) != 0;
  });
}

bool StateIsDependencyReady(const std::string& State) {
  return State == "Merged";
}

std::string JsonEscape(const std::string& Value) {
  std::ostringstream Output;
  Output << '"';
  static constexpr char Hex[] = "0123456789abcdef";
  for (const unsigned char Character : Value) {
    switch (Character) {
      case '"':
        Output << "\\\"";
        break;
      case '\\':
        Output << "\\\\";
        break;
      case '\b':
        Output << "\\b";
        break;
      case '\f':
        Output << "\\f";
        break;
      case '\n':
        Output << "\\n";
        break;
      case '\r':
        Output << "\\r";
        break;
      case '\t':
        Output << "\\t";
        break;
      default:
        if (Character < 0x20) {
          Output << "\\u00" << Hex[(Character >> 4) & 0x0f] << Hex[Character & 0x0f];
        } else {
          Output << static_cast<char>(Character);
        }
    }
  }
  Output << '"';
  return Output.str();
}

std::string JsonStringArray(const std::vector<std::string>& Values) {
  std::ostringstream Output;
  Output << '[';
  for (std::size_t Index = 0; Index < Values.size(); ++Index) {
    if (Index != 0) {
      Output << ',';
    }
    Output << JsonEscape(RedactSensitiveText(Values[Index]));
  }
  Output << ']';
  return Output.str();
}

std::string UtcNow() {
  const std::time_t Now = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  std::tm Time{};
  if (gmtime_s(&Time, &Now) != 0) {
    throw WorkflowError("Unable to format UTC event time");
  }
  std::array<char, 32> Buffer{};
  if (std::strftime(Buffer.data(), Buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &Time) == 0) {
    throw WorkflowError("Unable to format UTC event time");
  }
  return Buffer.data();
}

std::string NewRecordId() {
  static std::atomic<unsigned long long> Sequence{0};
  const auto Tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(Tick) + "-" + std::to_string(++Sequence);
}

std::string SerializeEvent(const WorkflowEvent& Event) {
  std::ostringstream Output;
  Output << '{'
         << "\"EventId\":" << JsonEscape(RedactSensitiveText(Event.EventId)) << ','
         << "\"EventType\":" << JsonEscape(RedactSensitiveText(Event.EventType)) << ','
         << "\"OccurredAt\":" << JsonEscape(Event.OccurredAt.empty() ? UtcNow() : Event.OccurredAt) << ','
         << "\"ReleaseId\":" << JsonEscape(RedactSensitiveText(Event.ReleaseId)) << ','
         << "\"PhaseId\":" << JsonEscape(RedactSensitiveText(Event.PhaseId)) << ','
         << "\"TaskId\":" << JsonEscape(RedactSensitiveText(Event.TaskId)) << ','
         << "\"TaskRevision\":" << Event.TaskRevision << ','
         << "\"AgentId\":" << JsonEscape(RedactSensitiveText(Event.AgentId)) << ','
         << "\"AgentRole\":" << JsonEscape(RedactSensitiveText(Event.AgentRole)) << ','
         << "\"Branch\":" << JsonEscape(RedactSensitiveText(Event.Branch)) << ','
         << "\"CorrelationId\":" << JsonEscape(RedactSensitiveText(Event.CorrelationId)) << ','
         << "\"Result\":" << JsonEscape(RedactSensitiveText(Event.Result)) << ','
         << "\"EvidenceRefs\":" << JsonStringArray(Event.EvidenceRefs) << ','
         << "\"Command\":" << JsonEscape(RedactSensitiveText(Event.Command)) << ','
         << "\"WorkingDirectory\":" << JsonEscape(RedactSensitiveText(Event.WorkingDirectory)) << ','
         << "\"ExitCode\":" << Event.ExitCode << ','
         << "\"EvidencePath\":" << JsonEscape(RedactSensitiveText(Event.EvidencePath)) << ','
         << "\"ApprovalReason\":" << JsonEscape(RedactSensitiveText(Event.ApprovalReason)) << ','
         << "\"MaxReferenceFiles\":" << Event.MaxReferenceFiles
         << '}';
  return Output.str();
}

void WriteExclusive(const std::filesystem::path& Path, const std::string& Contents) {
  std::filesystem::create_directories(Path.parent_path());
  HANDLE Handle = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (Handle == INVALID_HANDLE_VALUE) {
    throw WorkflowError("Refusing to overwrite an existing output: " + Path.string());
  }
  const auto Close = [&Handle]() {
    if (Handle != INVALID_HANDLE_VALUE) {
      CloseHandle(Handle);
      Handle = INVALID_HANDLE_VALUE;
    }
  };
  std::size_t Offset = 0;
  try {
    while (Offset < Contents.size()) {
      const DWORD ChunkSize = static_cast<DWORD>(
          std::min<std::size_t>(Contents.size() - Offset, 1024U * 1024U));
      DWORD Written = 0;
      if (!WriteFile(Handle, Contents.data() + Offset, ChunkSize, &Written, nullptr) ||
          Written == 0) {
        throw WorkflowError("Failed writing output file: " + Path.string());
      }
      Offset += Written;
    }
    if (!FlushFileBuffers(Handle)) {
      throw WorkflowError("Failed flushing output file: " + Path.string());
    }
    Close();
  } catch (...) {
    Close();
    throw;
  }
}

void AtomicReplace(const std::filesystem::path& Path, const std::string& Contents) {
  std::filesystem::create_directories(Path.parent_path());
  const auto TemporaryPath = Path.parent_path() /
      (Path.filename().string() + ".tmp." + NewRecordId());
  WriteExclusive(TemporaryPath, Contents);
  if (!MoveFileExW(TemporaryPath.c_str(), Path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(TemporaryPath.c_str());
    throw WorkflowError("Failed to atomically replace checkpoint: " + Path.string());
  }
}

void AppendLocked(const std::filesystem::path& Path, const std::string& Line) {
  std::filesystem::create_directories(Path.parent_path());
  HANDLE Handle = CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (Handle == INVALID_HANDLE_VALUE) {
    throw WorkflowError("Unable to open append-only log: " + Path.string());
  }
  OVERLAPPED Overlap{};
  if (!LockFileEx(Handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &Overlap)) {
    CloseHandle(Handle);
    throw WorkflowError("Unable to lock append-only log: " + Path.string());
  }
  try {
    LARGE_INTEGER End{};
    if (!SetFilePointerEx(Handle, End, nullptr, FILE_END)) {
      throw WorkflowError("Unable to seek append-only log: " + Path.string());
    }
    std::size_t Offset = 0;
    while (Offset < Line.size()) {
      const DWORD ChunkSize = static_cast<DWORD>(
          std::min<std::size_t>(Line.size() - Offset, 1024U * 1024U));
      DWORD Written = 0;
      if (!WriteFile(Handle, Line.data() + Offset, ChunkSize, &Written, nullptr) ||
          Written == 0) {
        throw WorkflowError("Unable to append JSONL record: " + Path.string());
      }
      Offset += Written;
    }
    FlushFileBuffers(Handle);
    UnlockFileEx(Handle, 0, MAXDWORD, MAXDWORD, &Overlap);
    CloseHandle(Handle);
  } catch (...) {
    UnlockFileEx(Handle, 0, MAXDWORD, MAXDWORD, &Overlap);
    CloseHandle(Handle);
    throw;
  }
}

std::string SafeYamlScalar(const std::string& Value) {
  return RedactSensitiveText(Value);
}

YAML::Node CheckpointNode(const Checkpoint& Value) {
  YAML::Node Node;
  Node["TaskId"] = SafeYamlScalar(Value.TaskId);
  Node["TaskRevision"] = Value.TaskRevision;
  Node["Branch"] = SafeYamlScalar(Value.Branch);
  Node["Worktree"] = SafeYamlScalar(Value.Worktree);
  Node["BaseCommit"] = SafeYamlScalar(Value.BaseCommit);
  Node["HeadCommit"] = SafeYamlScalar(Value.HeadCommit);
  Node["LastCompletedAcceptance"] = SafeYamlScalar(Value.LastCompletedAcceptance);
  Node["CurrentFindingOrFailure"] = SafeYamlScalar(Value.CurrentFindingOrFailure);
  Node["TriedApproaches"] = YAML::Node(YAML::NodeType::Sequence);
  for (const auto& Item : Value.TriedApproaches) {
    Node["TriedApproaches"].push_back(SafeYamlScalar(Item));
  }
  Node["NextAction"] = SafeYamlScalar(Value.NextAction);
  Node["OpenMessageIds"] = YAML::Node(YAML::NodeType::Sequence);
  for (const auto& Item : Value.OpenMessageIds) {
    Node["OpenMessageIds"].push_back(SafeYamlScalar(Item));
  }
  Node["LastValidationResult"] = SafeYamlScalar(Value.LastValidationResult);
  Node["UpdatedAt"] = UtcNow();
  return Node;
}

std::string SerializeTaskMessage(const TaskScopedMessage& Message) {
  std::ostringstream Output;
  Output << '{'
         << "\"MessageId\":" << JsonEscape(RedactSensitiveText(Message.MessageId)) << ','
         << "\"TaskId\":" << JsonEscape(RedactSensitiveText(Message.TaskId)) << ','
         << "\"TaskRevision\":" << Message.TaskRevision << ','
         << "\"SenderAgentId\":" << JsonEscape(RedactSensitiveText(Message.SenderAgentId)) << ','
         << "\"RecipientAgentId\":" << JsonEscape(RedactSensitiveText(Message.RecipientAgentId)) << ','
         << "\"MessageType\":" << JsonEscape(RedactSensitiveText(Message.MessageType)) << ','
         << "\"Status\":" << JsonEscape(RedactSensitiveText(Message.Status)) << ','
         << "\"Subject\":" << JsonEscape(RedactSensitiveText(Message.Subject)) << ','
         << "\"Summary\":" << JsonEscape(RedactSensitiveText(Message.Summary)) << ','
         << "\"EvidenceRefs\":" << JsonStringArray(Message.EvidenceRefs)
         << '}';
  return Output.str();
}

std::string SerializeReviewBundle(const TaskSpec& Task,
                                  const ReviewBundleData& Bundle) {
  std::ostringstream Output;
  Output << '{'
         << "\"bundle_type\":\"LocalReviewBundle\","
         << "\"remote_pr_created\":false,"
         << "\"task_id\":" << JsonEscape(Task.TaskId) << ','
         << "\"task_revision\":" << Task.TaskRevision << ','
         << "\"phase_id\":" << JsonEscape(Task.PhaseId) << ','
         << "\"title\":" << JsonEscape(RedactSensitiveText(Task.Title)) << ','
         << "\"goal\":" << JsonEscape(RedactSensitiveText(Task.Goal)) << ','
         << "\"author_agent_id\":" << JsonEscape(RedactSensitiveText(Bundle.AuthorAgentId)) << ','
         << "\"reviewer_agent_id\":" << JsonEscape(RedactSensitiveText(Bundle.ReviewerAgentId)) << ','
         << "\"base_commit\":" << JsonEscape(Bundle.BaseCommit) << ','
         << "\"head_commit\":" << JsonEscape(Bundle.HeadCommit) << ','
         << "\"changed_paths\":" << JsonStringArray(Bundle.ChangedPaths) << ','
         << "\"changed_files\":[";
  for (std::size_t Index = 0; Index < Bundle.ChangedPaths.size(); ++Index) {
    if (Index != 0) {
      Output << ',';
    }
    const std::string Kind = Index < Bundle.ChangeKinds.size()
                                 ? Bundle.ChangeKinds[Index]
                                 : "Changed";
    Output << "{\"Path\":" << JsonEscape(RedactSensitiveText(Bundle.ChangedPaths[Index]))
           << ",\"ChangeKind\":" << JsonEscape(RedactSensitiveText(Kind)) << '}';
  }
  Output << "],"
         << "\"validations\":[";
  for (std::size_t Index = 0; Index < Bundle.Validations.size(); ++Index) {
    if (Index != 0) {
      Output << ',';
    }
    const auto& Validation = Bundle.Validations[Index];
    Output << '{'
           << "\"command\":" << JsonEscape(RedactSensitiveText(Validation.Command)) << ','
           << "\"working_directory\":" << JsonEscape(Validation.WorkingDirectory.string()) << ','
           << "\"exit_code\":" << Validation.ExitCode << ','
           << "\"evidence_path\":" << JsonEscape(Validation.EvidencePath.string())
           << '}';
  }
  Output << "],\"events\":[";
  for (std::size_t Index = 0; Index < Bundle.Events.size(); ++Index) {
    if (Index != 0) {
      Output << ',';
    }
    Output << SerializeEvent(Bundle.Events[Index]);
  }
  Output << "]}";
  return Output.str();
}

std::vector<std::string> TokenizeFrozenCommand(const std::string& Command) {
  if (Command.empty() || Command.find_first_of("\r\n;&|<>^%`$") != std::string::npos) {
    throw WorkflowError("Validation command contains shell syntax or is empty");
  }
  std::vector<std::string> Arguments;
  std::string Current;
  bool InQuotes = false;
  for (std::size_t Index = 0; Index < Command.size(); ++Index) {
    const char Character = Command[Index];
    if (Character == '"') {
      InQuotes = !InQuotes;
      continue;
    }
    if (Character == '\\' && InQuotes && Index + 1 < Command.size() &&
        Command[Index + 1] == '"') {
      Current.push_back('"');
      ++Index;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(Character)) && !InQuotes) {
      if (!Current.empty()) {
        Arguments.push_back(std::move(Current));
        Current.clear();
      }
      continue;
    }
    Current.push_back(Character);
  }
  if (InQuotes) {
    throw WorkflowError("Validation command contains an unmatched quote");
  }
  if (!Current.empty()) {
    Arguments.push_back(std::move(Current));
  }
  if (Arguments.empty()) {
    throw WorkflowError("Validation command has no executable");
  }

  const std::string Executable = LowerAscii(std::filesystem::path(Arguments.front()).filename().string());
  static const std::vector<std::string> Allowed{
      "cmake", "ctest", "clang-format", "clang-tidy", "workflowcontroller",
      "workflowcontroller.exe", "workflowcontrollercontracttests",
      "workflowcontrollercontracttests.exe", "buildprobe", "buildprobe.exe"};
  if (std::find(Allowed.begin(), Allowed.end(), Executable) == Allowed.end()) {
    throw WorkflowError("Validation executable is not registered for controller use: " + Executable);
  }
  return Arguments;
}

std::string TrimCommandOutput(std::string Value) {
  while (!Value.empty() &&
         (Value.back() == '\r' || Value.back() == '\n' || Value.back() == ' ' ||
          Value.back() == '\t')) {
    Value.pop_back();
  }
  return Value;
}

ProcessResult RunGit(ProcessRunner& Runner,
                     const std::filesystem::path& RepositoryRoot,
                     std::vector<std::string> GitArguments) {
  std::vector<std::string> Arguments{"git", "-C", RepositoryRoot.string()};
  Arguments.insert(Arguments.end(), GitArguments.begin(), GitArguments.end());
  ProcessResult Result = Runner.Run(Arguments, RepositoryRoot);
  if (Result.ExitCode != 0) {
    throw WorkflowError("Git operation failed (exit " + std::to_string(Result.ExitCode) +"): " +
                        RedactSensitiveText(Result.Output));
  }
  return Result;
}

bool PathsEqual(const std::filesystem::path& Left,
                const std::filesystem::path& Right);

bool IsRegisteredWorktree(ProcessRunner& Runner,
                          const std::filesystem::path& SourceRoot,
                          const std::filesystem::path& TargetPath,
                          const std::string& Branch) {
  const std::string Listing = RunGit(
      Runner, SourceRoot, {"worktree", "list", "--porcelain"}).Output;
  std::filesystem::path CurrentPath;
  std::string CurrentBranch;
  std::istringstream Lines(Listing);
  std::string Line;
  while (std::getline(Lines, Line)) {
    if (!Line.empty() && Line.back() == '\r') {
      Line.pop_back();
    }
    if (Line.starts_with("worktree ")) {
      CurrentPath = std::filesystem::path(Line.substr(9));
      CurrentBranch.clear();
      continue;
    }
    if (Line.starts_with("branch ")) {
      CurrentBranch = Line.substr(7);
      if (PathsEqual(CurrentPath, TargetPath) &&
          CurrentBranch == "refs/heads/" + Branch) {
        return true;
      }
    }
  }
  return false;
}

bool PathsEqual(const std::filesystem::path& Left,
                const std::filesystem::path& Right) {
  return LowerWide(Left.lexically_normal().native()) ==
         LowerWide(Right.lexically_normal().native());
}

}  // namespace

TaskSpec LoadTaskSpec(const std::filesystem::path& RepositoryRoot,
                      const std::filesystem::path& TaskIndexPath,
                      const std::string& PhaseId,
                      const std::string& TaskId) {
  if (!IsTaskId(TaskId)) {
    throw WorkflowError("TaskId must use the ED-0000 format");
  }
  const auto PlanPath = ResolveTaskPlanPath(RepositoryRoot, TaskIndexPath, PhaseId);
  TaskSpec Parsed = ParseTaskSpec(FindTaskNode(LoadYaml(PlanPath), TaskId), PhaseId);
  const auto CanonicalRoot = CanonicalExistingDirectory(RepositoryRoot);
  Parsed.TaskPlanPath = PlanPath.lexically_relative(CanonicalRoot).generic_string();
  const YAML::Node Index = LoadYaml(ResolveRepoPath(RepositoryRoot, TaskIndexPath.generic_string()));
  TaskSpec Task = Parsed;
  Task.ReleaseId = OptionalString(Index, "CurrentReleaseId", "Bootstrap");
  const YAML::Node CommunicationStore = Index["CommunicationStore"];
  const std::string CommunicationRoot = CommunicationStore
                                            ? OptionalString(CommunicationStore, "Root")
                                            : std::string{};
  if (!CommunicationRoot.empty()) {
    Task.CommunicationShard = NormalizeRelativePath(
        CommunicationRoot + "/" + Task.ReleaseId + "/" + PhaseId + ".jsonl");
  }
  return Task;
}

std::string LoadTaskState(const std::filesystem::path& RepositoryRoot,
                          const std::filesystem::path& TaskIndexPath,
                          const std::string& TaskId) {
  if (!IsTaskId(TaskId)) {
    throw WorkflowError("TaskId must use the ED-0000 format");
  }
  const auto IndexPath = ResolveRepoPath(RepositoryRoot, TaskIndexPath.generic_string());
  const YAML::Node Index = LoadYaml(IndexPath);
  const YAML::Node Plans = Index["PhasePlans"];
  if (!Plans || !Plans.IsSequence()) {
    throw WorkflowError("TaskIndex has no PhasePlans sequence");
  }
  for (const YAML::Node& Plan : Plans) {
    const std::string RelativePath = OptionalString(Plan, "Path");
    if (RelativePath.empty()) {
      continue;
    }
    const YAML::Node Phase = LoadYaml(ResolveRepoPath(RepositoryRoot, RelativePath));
    const YAML::Node Tasks = Phase["Tasks"];
    if (!Tasks || !Tasks.IsSequence()) {
      continue;
    }
    for (const YAML::Node& Candidate : Tasks) {
      if (OptionalString(Candidate, "TaskId") == TaskId) {
        return RequiredString(Candidate, "State");
      }
    }
  }
  throw WorkflowError("Dependency TaskId is not present in any routed phase plan: " + TaskId);
}

bool IsAllowedStateTransition(const std::string& CurrentState,
                              const std::string& NextState) {
  static const std::unordered_map<std::string, std::vector<std::string>> Allowed{
      {"Proposed", {"Ready", "Blocked", "Superseded"}},
      {"Ready", {"Assigned", "Blocked", "Superseded"}},
      {"Assigned", {"Active", "Blocked"}},
      {"Active", {"Review", "Blocked"}},
      {"Review", {"Rework", "Verified", "Blocked"}},
      {"Rework", {"Active", "Blocked"}},
      {"Verified", {"MergeQueued", "Blocked"}},
      {"MergeQueued", {"Merged", "Blocked"}},
      {"Blocked", {"Ready", "Assigned", "Active", "Superseded"}},
  };
  const auto It = Allowed.find(CurrentState);
  return It != Allowed.end() &&
         std::find(It->second.begin(), It->second.end(), NextState) != It->second.end();
}

void ValidateAssignment(const TaskSpec& Task,
                        const AssignmentRequest& Request,
                        const std::map<std::string, std::string>& DependencyStates) {
  if (Task.TaskId != Request.TaskId) {
    throw WorkflowError("Assignment TaskId does not match the routed TaskSpec");
  }
  if (Task.TaskRevision != Request.TaskRevision) {
    throw WorkflowError("Stale TaskRevision; assignment must use the frozen revision");
  }
  if (Task.State == "Ready") {
    if (!IsAllowedStateTransition(Task.State, "Assigned")) {
      throw WorkflowError("Invalid state transition for assignment: Ready -> Assigned");
    }
  } else if (Task.State == "Assigned") {
    if (Task.OwnerAgentId != Request.OwnerAgentId || Task.Branch != Request.Branch ||
        Task.BaselineCommit != Request.BaseCommit) {
      throw WorkflowError("Assigned owner, branch, or baseline does not match TaskSpec");
    }
    if (!Task.Worktree.empty()) {
      const auto SpecPath = std::filesystem::path(Task.Worktree).lexically_normal();
      const auto RequestPath = Request.Worktree.lexically_normal();
      if (LowerWide(SpecPath.native()) != LowerWide(RequestPath.native())) {
        throw WorkflowError("Assigned worktree does not match TaskSpec");
      }
    }
  } else {
    throw WorkflowError("Task state cannot be assigned: " + Task.State);
  }

  if (Request.OwnerAgentId.empty() || !IsAllowedBranch(Task.TaskId, Request.Branch) ||
      !IsCommitId(Request.BaseCommit) || !Request.Worktree.is_absolute()) {
    throw WorkflowError("Assignment is missing a valid agent, task branch, absolute worktree, or BaseCommit");
  }
  for (const std::string& DependencyId : Task.DependsOn) {
    const auto It = DependencyStates.find(DependencyId);
    if (It == DependencyStates.end() || !StateIsDependencyReady(It->second)) {
      throw WorkflowError("Unsatisfied dependency: " + DependencyId);
    }
  }
}

void ValidateReadSetPath(const TaskSpec& Task, const std::string& RelativePath) {
  const std::string Candidate = LowerAscii(NormalizeRelativePath(RelativePath));
  const bool Authorized = std::any_of(
      Task.ReadSet.begin(), Task.ReadSet.end(), [&](const std::string& Allowed) {
        return LowerAscii(NormalizeRelativePath(Allowed)) == Candidate;
      });
  if (!Authorized) {
    throw WorkflowError("Context path is outside the frozen ReadSet: " + RelativePath);
  }
}

void RequestAdditionalRead(const TaskSpec& Task,
                           const std::string& RelativePath,
                           const std::string& Reason,
                           const EventStore& Events,
                           const std::string& AgentId,
                           const std::string& CorrelationId) {
  try {
    ValidateReadSetPath(Task, RelativePath);
    return;
  } catch (const WorkflowError&) {
    WorkflowEvent Event;
    Event.EventType = "ContextExpanded";
    Event.ReleaseId = Task.ReleaseId;
    Event.PhaseId = Task.PhaseId;
    Event.TaskId = Task.TaskId;
    Event.TaskRevision = Task.TaskRevision;
    Event.AgentId = AgentId;
    Event.Branch = Task.Branch;
    Event.CorrelationId = CorrelationId;
    Event.Result = Reason.empty() ? "RejectedMissingExpansionReason"
                                  : "RejectedOutsideFrozenReadSet; reason=" +
                                        RedactSensitiveText(Reason);
    Event.EvidenceRefs = {RedactSensitiveText(RelativePath)};
    Events.Append(Event);
    if (Reason.empty()) {
      throw WorkflowError("Additional context request requires a reason");
    }
    throw WorkflowError("Path expansion was recorded and rejected outside the frozen ReadSet");
  }
}

std::vector<ContextFile> AssembleReadSetContext(
    const std::filesystem::path& RepositoryRoot,
    const TaskSpec& Task,
    std::size_t MaximumFileBytes,
    std::size_t MaximumTotalBytes,
    const std::optional<ContextBudgetOverride>& BudgetOverride) {
  std::size_t AllowedReferences = Task.MaxReferenceFiles;
  if (AllowedReferences > 12) {
    if (!BudgetOverride || BudgetOverride->TaskId != Task.TaskId ||
        BudgetOverride->TaskRevision != Task.TaskRevision ||
        BudgetOverride->MaxReferenceFiles < AllowedReferences ||
        BudgetOverride->ApprovedByRole != "Coordinator" ||
        BudgetOverride->Reason.empty() ||
        !BudgetOverride->ApprovalEventRef.starts_with("Workflow/Events/") ||
        BudgetOverride->ApprovalEventRef.find("..") != std::string::npos) {
      throw WorkflowError("ReadSet budget above 12 requires a matching Coordinator exception event");
    }
    AllowedReferences = BudgetOverride->MaxReferenceFiles;
  } else if (BudgetOverride) {
    if (BudgetOverride->TaskId != Task.TaskId ||
        BudgetOverride->TaskRevision != Task.TaskRevision ||
        BudgetOverride->MaxReferenceFiles < Task.ReadSet.size() ||
        BudgetOverride->ApprovedByRole != "Coordinator" ||
        BudgetOverride->Reason.empty() ||
        !BudgetOverride->ApprovalEventRef.starts_with("Workflow/Events/") ||
        BudgetOverride->ApprovalEventRef.find("..") != std::string::npos) {
      throw WorkflowError("Invalid Coordinator ReadSet budget exception");
    }
    AllowedReferences = BudgetOverride->MaxReferenceFiles;
  }
  if (Task.ReadSet.size() > AllowedReferences || AllowedReferences > 64) {
    throw WorkflowError("ReadSet exceeds the registered reference-file budget");
  }

  std::vector<ContextFile> Context;
  Context.reserve(Task.ReadSet.size());
  std::size_t TotalBytes = 0;
  for (const std::string& RelativePath : Task.ReadSet) {
    ValidateReadSetPath(Task, RelativePath);
    const auto FullPath = ResolveRepoPath(RepositoryRoot, RelativePath);
    std::error_code Error;
    if (!std::filesystem::is_regular_file(FullPath, Error) || Error) {
      throw WorkflowError("ReadSet entries must resolve to existing files: " + RelativePath);
    }
    const auto Size = std::filesystem::file_size(FullPath, Error);
    if (Error || Size > MaximumFileBytes || TotalBytes + Size > MaximumTotalBytes) {
      throw WorkflowError("ReadSet file or bundle exceeds the configured context byte limit: " +
                          RelativePath);
    }
    std::ifstream Input(FullPath, std::ios::binary);
    if (!Input) {
      throw WorkflowError("Unable to read ReadSet file: " + RelativePath);
    }
    std::string Content((std::istreambuf_iterator<char>(Input)),
                        std::istreambuf_iterator<char>());
    TotalBytes += Content.size();
    Context.push_back({NormalizeRelativePath(RelativePath),
                       RedactSensitiveText(Content)});
  }
  return Context;
}

void ValidateWriteSetPaths(const TaskSpec& Task,
                           const std::vector<std::string>& ChangedPaths) {
  for (const std::string& ChangedPath : ChangedPaths) {
    const std::string Normalized = NormalizeRelativePath(ChangedPath);
    const bool Authorized = std::any_of(
        Task.WriteSet.begin(), Task.WriteSet.end(), [&](const std::string& Allowed) {
          return PathPatternMatches(Allowed, Normalized);
        });
    if (!Authorized) {
      throw WorkflowError("Changed path is outside the frozen WriteSet: " + ChangedPath);
    }
    const bool Forbidden = std::any_of(
        Task.ForbiddenSet.begin(), Task.ForbiddenSet.end(), [&](const std::string& Denied) {
          return PathPatternMatches(Denied, Normalized);
        });
    if (Forbidden) {
      throw WorkflowError("Changed path is in the frozen ForbiddenSet: " + ChangedPath);
    }
    const std::string RequiredLock = RequiredSharedLock(Normalized);
    if (!RequiredLock.empty() && !HasSharedLock(Task, RequiredLock)) {
      throw WorkflowError("Shared lock is not held for changed path " + ChangedPath +
                          ": " + RequiredLock);
    }
    for (const std::string& Lock : RequiredSharedLocks(Normalized)) {
      if (!HasSharedLock(Task, Lock)) {
        throw WorkflowError("Shared lock is not held for changed path " + ChangedPath +
                            ": " + Lock);
      }
    }
  }
}

std::vector<std::string> FindWriteSetConflicts(const TaskSpec& Left,
                                               const TaskSpec& Right) {
  std::vector<std::string> Conflicts;
  for (const std::string& LeftPath : Left.WriteSet) {
    for (const std::string& RightPath : Right.WriteSet) {
      if (PathPatternMatches(LeftPath, RightPath) ||
          PathPatternMatches(RightPath, LeftPath)) {
        Conflicts.push_back("WriteSet overlap: " + LeftPath + " <> " + RightPath);
      }
    }
  }
  for (const std::string& LeftLock : Left.SharedLocks) {
    if (std::find(Right.SharedLocks.begin(), Right.SharedLocks.end(), LeftLock) !=
        Right.SharedLocks.end()) {
      Conflicts.push_back("SharedLock overlap: " + LeftLock);
    }
  }
  return Conflicts;
}

WorktreeResult CreateOrVerifyWorktree(const WorktreeRequest& Request,
                                      ProcessRunner& Runner) {
  const auto RepositoryRoot = CanonicalExistingDirectory(Request.RepositoryRoot);
  const auto RequestedWorktree = Request.Assignment.Worktree;
  if (!IsTaskId(Request.Assignment.TaskId) ||
      !IsAllowedBranch(Request.Assignment.TaskId, Request.Assignment.Branch) ||
      !IsCommitId(Request.Assignment.BaseCommit) || !RequestedWorktree.is_absolute()) {
    throw WorkflowError("Worktree request lacks a valid TaskId, task branch, path, or BaseCommit");
  }

  const ProcessResult SourceTopLevel = RunGit(
      Runner, RepositoryRoot, {"rev-parse", "--show-toplevel"});
  const auto CanonicalSource = CanonicalExistingDirectory(
      std::filesystem::path(TrimCommandOutput(SourceTopLevel.Output)));
  if (!PathsEqual(CanonicalSource, RepositoryRoot)) {
    throw WorkflowError("RepositoryRoot must be the source worktree top-level directory");
  }
  const auto CanonicalWorktree = CanonicalizeFuturePath(RequestedWorktree);
  if (IsWithinPath(CanonicalWorktree, CanonicalSource)) {
    throw WorkflowError("Assigned worktree must be outside the original source worktree");
  }

  const std::string SourceBranchBefore = TrimCommandOutput(
      RunGit(Runner, CanonicalSource, {"branch", "--show-current"}).Output);
  const std::string SourceHeadBefore = TrimCommandOutput(
      RunGit(Runner, CanonicalSource, {"rev-parse", "HEAD"}).Output);
  const std::string SourceStatusBefore = RunGit(
      Runner, CanonicalSource, {"status", "--porcelain", "--untracked-files=all"}).Output;

  bool Created = false;
  if (!std::filesystem::exists(CanonicalWorktree)) {
    std::filesystem::create_directories(CanonicalWorktree.parent_path());
    (void)RunGit(Runner, CanonicalSource,
                 {"worktree", "add", "-b", Request.Assignment.Branch,
                  CanonicalWorktree.string(), Request.Assignment.BaseCommit});
    Created = true;
  }

  const ProcessResult WorktreeTopLevel = RunGit(
      Runner, CanonicalWorktree, {"rev-parse", "--show-toplevel"});
  const auto CanonicalRegisteredTopLevel = CanonicalExistingDirectory(
      std::filesystem::path(TrimCommandOutput(WorktreeTopLevel.Output)));
  if (!PathsEqual(CanonicalRegisteredTopLevel, CanonicalWorktree)) {
    throw WorkflowError("Assigned path is not the registered worktree top-level directory");
  }
  const std::string ActualBranch = TrimCommandOutput(
      RunGit(Runner, CanonicalWorktree, {"branch", "--show-current"}).Output);
  if (ActualBranch != Request.Assignment.Branch) {
    throw WorkflowError("Worktree branch does not match the frozen assignment");
  }
  if (!IsRegisteredWorktree(Runner, CanonicalSource, CanonicalWorktree, ActualBranch)) {
    throw WorkflowError("Assigned checkout is not registered as a Git worktree of the source repository");
  }
  const std::string HeadCommit = TrimCommandOutput(
      RunGit(Runner, CanonicalWorktree, {"rev-parse", "HEAD"}).Output);
  if (!IsCommitId(HeadCommit)) {
    throw WorkflowError("Worktree HEAD is not a valid commit id");
  }
  (void)RunGit(Runner, CanonicalWorktree,
               {"merge-base", "--is-ancestor", Request.Assignment.BaseCommit, HeadCommit});

  const std::string SourceBranchAfter = TrimCommandOutput(
      RunGit(Runner, CanonicalSource, {"branch", "--show-current"}).Output);
  const std::string SourceHeadAfter = TrimCommandOutput(
      RunGit(Runner, CanonicalSource, {"rev-parse", "HEAD"}).Output);
  const std::string SourceStatusAfter = RunGit(
      Runner, CanonicalSource, {"status", "--porcelain", "--untracked-files=all"}).Output;
  if (SourceBranchBefore != SourceBranchAfter || SourceHeadBefore != SourceHeadAfter ||
      SourceStatusBefore != SourceStatusAfter) {
    throw WorkflowError("Worktree operation changed the original worktree branch, HEAD, or files");
  }

  return {CanonicalWorktree, ActualBranch, Request.Assignment.BaseCommit, HeadCommit, Created};
}

ValidationResult RunFrozenValidation(const TaskSpec& Task,
                                     const std::string& ExactCommand,
                                     const std::filesystem::path& WorkingDirectory,
                                     const std::filesystem::path& EvidencePath,
                                     ProcessRunner& Runner,
                                     const EventStore& Events,
                                     const std::string& AgentId,
                                     const std::string& CorrelationId) {
  if (std::find(Task.ValidationCommands.begin(), Task.ValidationCommands.end(), ExactCommand) ==
      Task.ValidationCommands.end()) {
    throw WorkflowError("Validation command is not listed in the frozen TaskSpec");
  }
  const std::vector<std::string> Arguments = TokenizeFrozenCommand(ExactCommand);
  if (!WorkingDirectory.is_absolute() || !std::filesystem::is_directory(WorkingDirectory)) {
    throw WorkflowError("Validation working directory must be an existing absolute directory");
  }
  if (!EvidencePath.is_absolute() || std::filesystem::exists(EvidencePath)) {
    throw WorkflowError("Validation evidence path must be absolute and must not overwrite existing data");
  }
  const auto CanonicalWorkingDirectory = CanonicalExistingDirectory(WorkingDirectory);
  const auto CanonicalEvidencePath = CanonicalizeFuturePath(EvidencePath);
  if (IsWithinPath(CanonicalEvidencePath, CanonicalWorkingDirectory)) {
    throw WorkflowError("Validation evidence must be written outside the assigned worktree");
  }
  const auto BuildSibling = CanonicalWorkingDirectory.parent_path() /
                            (CanonicalWorkingDirectory.filename().string() + ".build");
  const auto CanonicalBuildSibling = CanonicalizeFuturePath(BuildSibling);
  if (!IsWithinPath(CanonicalEvidencePath, CanonicalBuildSibling)) {
    throw WorkflowError("Validation evidence must remain under the assigned worktree's sibling build directory");
  }
  ProcessResult Process = Runner.Run(Arguments, WorkingDirectory);
  WriteExclusive(EvidencePath, RedactSensitiveText(Process.Output));

  WorkflowEvent Event;
  Event.ReleaseId = Task.ReleaseId;
  Event.EventType = "CommandExecuted";
  Event.PhaseId = Task.PhaseId;
  Event.TaskId = Task.TaskId;
  Event.TaskRevision = Task.TaskRevision;
  Event.AgentId = AgentId;
  Event.Branch = Task.Branch;
  Event.CorrelationId = CorrelationId;
  Event.Result = Process.ExitCode == 0 ? "Succeeded" : "Failed";
  Event.Command = ExactCommand;
  Event.WorkingDirectory = WorkingDirectory.string();
  Event.ExitCode = static_cast<long>(Process.ExitCode);
  Event.EvidencePath = EvidencePath.string();
  Event.EvidenceRefs.push_back(EvidencePath.string());
  Events.Append(Event);

  return {ExactCommand, WorkingDirectory, Process.ExitCode, EvidencePath};
}

std::string RedactSensitiveText(const std::string& Text) {
  std::string Result = Text;
  static const std::regex KeyValue(
      R"(((?:api[_-]?(?:key|token)|(?:access|refresh)[_-]?token|token|password|passwd|secret|authorization)\s*[:=]\s*)(?:"[^"]*"|'[^']*'|[^\s,;&]+))",
      std::regex::icase);
  static const std::regex Bearer(R"((Bearer\s+)[A-Za-z0-9._~+/=-]+)",
                                 std::regex::icase);
  static const std::regex FlagValue(
      R"(((?:--api-key|--access-token|--token|--password|--secret)\s+)(?:"[^"]*"|'[^']*'|[^\s,;&]+))",
      std::regex::icase);
  Result = std::regex_replace(Result, KeyValue, "$1[REDACTED]");
  Result = std::regex_replace(Result, Bearer, "$1[REDACTED]");
  Result = std::regex_replace(Result, FlagValue, "$1[REDACTED]");
  return Result;
}

EventStore::EventStore(std::filesystem::path EventPath)
    : EventPath(std::move(EventPath)) {}

void EventStore::Append(const WorkflowEvent& Event) const {
  if (!IsTaskId(Event.TaskId) || Event.TaskRevision <= 0 || Event.EventType.empty()) {
    throw WorkflowError("Structured event requires TaskId, TaskRevision, and EventType");
  }
  WorkflowEvent Copy = Event;
  if (Copy.EventId.empty()) {
    Copy.EventId = NewRecordId();
  }
  if (Copy.PhaseId.empty() || Copy.ReleaseId.empty()) {
    throw WorkflowError("Structured events require ReleaseId and PhaseId");
  }
  AppendLocked(EventPath, SerializeEvent(Copy) + "\n");
}

std::vector<WorkflowEvent> EventStore::ReadTaskEvents(const std::string& TaskId,
                                                     std::size_t MaximumEvents) const {
  if (!IsTaskId(TaskId)) {
    throw WorkflowError("Task-scoped event query requires a valid TaskId");
  }
  std::ifstream Input(EventPath, std::ios::binary);
  if (!Input) {
    if (!std::filesystem::exists(EventPath)) {
      return {};
    }
    throw WorkflowError("Unable to read event log: " + EventPath.string());
  }
  std::vector<WorkflowEvent> Results;
  std::string Line;
  std::size_t Scanned = 0;
  constexpr std::size_t MaximumScannedEvents = 100000;
  while (std::getline(Input, Line)) {
    if (++Scanned > MaximumScannedEvents) {
      throw WorkflowError("Event log exceeds the bounded scan limit");
    }
    if (Line.empty()) {
      continue;
    }
    YAML::Node Node;
    try {
      Node = YAML::Load(Line);
    } catch (const YAML::Exception& Error) {
      throw WorkflowError("Invalid JSONL event record: " + std::string(Error.what()));
    }
    if (OptionalEventString(Node, "TaskId", "task_id") != TaskId) {
      continue;
    }
    WorkflowEvent Event;
    Event.EventId = OptionalEventString(Node, "EventId", "event_id");
    Event.EventType = OptionalEventString(Node, "EventType", "event_type");
    Event.OccurredAt = OptionalEventString(Node, "OccurredAt", "occurred_at");
    Event.ReleaseId = OptionalEventString(Node, "ReleaseId", "release_id");
    Event.PhaseId = OptionalEventString(Node, "PhaseId", "phase_id");
    Event.TaskId = TaskId;
    Event.TaskRevision = EventInteger(Node, "TaskRevision", "task_revision");
    Event.AgentId = OptionalEventString(Node, "AgentId", "agent_id");
    Event.AgentRole = OptionalEventString(Node, "AgentRole", "agent_role");
    Event.Branch = OptionalEventString(Node, "Branch", "branch");
    Event.CorrelationId = OptionalEventString(Node, "CorrelationId", "correlation_id");
    Event.Result = OptionalEventString(Node, "Result", "result");
    const YAML::Node Evidence = Node["EvidenceRefs"] ? Node["EvidenceRefs"] : Node["evidence_refs"];
    if (Evidence && Evidence.IsSequence()) {
      for (const YAML::Node& Reference : Evidence) {
        if (Reference.IsScalar()) {
          Event.EvidenceRefs.push_back(Reference.as<std::string>());
        }
      }
    }
    Event.Command = OptionalEventString(Node, "Command", "command");
    Event.WorkingDirectory = OptionalEventString(Node, "WorkingDirectory", "working_directory");
    Event.ExitCode = EventLong(Node, "ExitCode", "exit_code");
    Event.EvidencePath = OptionalEventString(Node, "EvidencePath", "evidence_path");
    Event.ApprovalReason = OptionalEventString(Node, "ApprovalReason", "approval_reason");
    Event.MaxReferenceFiles = EventSize(Node, "MaxReferenceFiles", "max_reference_files");
    Results.push_back(std::move(Event));
    if (Results.size() > MaximumEvents) {
      Results.erase(Results.begin(), Results.begin() +
                    static_cast<std::ptrdiff_t>(Results.size() - MaximumEvents));
    }
  }
  return Results;
}

std::optional<ContextBudgetOverride> EventStore::FindContextBudgetOverride(
    const TaskSpec& Task) const {
  if (!Task.RegisteredBudgetOverride) {
    return std::nullopt;
  }
  const ContextBudgetOverride& Registered = *Task.RegisteredBudgetOverride;
  if (Task.EventShard.empty() || !Task.EventShard.starts_with("Workflow/Events/") ||
      Task.TaskPlanPath.empty()) {
    return std::nullopt;
  }
  const std::vector<WorkflowEvent> TaskEvents = ReadTaskEvents(Task.TaskId);
  for (auto It = TaskEvents.rbegin(); It != TaskEvents.rend(); ++It) {
    const std::string TaskEvidenceRef =
        Task.TaskPlanPath + "#" + Task.TaskId + ".ContextBudgetOverride";
    if (It->EventType != "ContextBudgetExceptionApproved" ||
        It->TaskRevision != Task.TaskRevision || It->AgentRole != "Coordinator" ||
        It->Result != "Approved" || It->ApprovalReason.empty() ||
        It->EventId != Registered.ApprovalEventId ||
        std::find(It->EvidenceRefs.begin(), It->EvidenceRefs.end(), TaskEvidenceRef) ==
            It->EvidenceRefs.end() ||
        Registered.TaskId != Task.TaskId ||
        Registered.TaskRevision != Task.TaskRevision ||
        Registered.ApprovedByRole != "Coordinator" ||
        Registered.Reason.empty() ||
        Registered.MaxReferenceFiles < Task.ReadSet.size() ||
        Registered.MaxReferenceFiles != It->MaxReferenceFiles ||
        Registered.Reason != It->ApprovalReason) {
      continue;
    }
    ContextBudgetOverride Override = Registered;
    if (Task.EventShard.empty() || !Task.EventShard.starts_with("Workflow/Events/")) {
      throw WorkflowError("TaskSpec must route context budget approvals to its EventShard");
    }
    Override.ApprovalEventRef = Task.EventShard + "#" + It->EventId;
    return Override;
  }
  return std::nullopt;
}

void CheckpointStore::Save(const std::filesystem::path& CheckpointPath,
                           const Checkpoint& Value) {
  if (!IsTaskId(Value.TaskId) || Value.TaskRevision <= 0 ||
      !IsCommitId(Value.BaseCommit) || !IsCommitId(Value.HeadCommit)) {
    throw WorkflowError("Checkpoint must bind a task revision and BaseCommit/HeadCommit");
  }
  if (std::filesystem::exists(CheckpointPath)) {
    (void)Recover(CheckpointPath, Value.TaskId, Value.TaskRevision);
  }
  AtomicReplace(CheckpointPath, YAML::Dump(CheckpointNode(Value)) + "\n");
}

TaskScopedMessageStore::TaskScopedMessageStore(std::filesystem::path MessagePath)
    : MessagePath(std::move(MessagePath)) {}

Checkpoint CheckpointStore::Recover(const std::filesystem::path& CheckpointPath,
                                    const std::string& ExpectedTaskId,
                                    int ExpectedTaskRevision) {
  const YAML::Node Node = LoadYaml(CheckpointPath);
  Checkpoint Value;
  Value.TaskId = RequiredString(Node, "TaskId");
  Value.TaskRevision = Node["TaskRevision"].as<int>(0);
  Value.Branch = OptionalString(Node, "Branch");
  Value.Worktree = OptionalString(Node, "Worktree");
  Value.BaseCommit = OptionalString(Node, "BaseCommit");
  Value.HeadCommit = OptionalString(Node, "HeadCommit");
  Value.LastCompletedAcceptance = OptionalString(Node, "LastCompletedAcceptance");
  Value.CurrentFindingOrFailure = OptionalString(Node, "CurrentFindingOrFailure");
  Value.TriedApproaches = StringSequence(Node, "TriedApproaches");
  Value.NextAction = OptionalString(Node, "NextAction");
  Value.OpenMessageIds = StringSequence(Node, "OpenMessageIds");
  Value.LastValidationResult = OptionalString(Node, "LastValidationResult");
  if (Value.TaskId != ExpectedTaskId || Value.TaskRevision != ExpectedTaskRevision) {
    throw WorkflowError("Checkpoint does not match the requested TaskId/TaskRevision");
  }
  return Value;
}

void TaskScopedMessageStore::Append(const TaskScopedMessage& Message) const {
  if (!IsTaskId(Message.TaskId) || Message.TaskRevision <= 0 ||
      Message.MessageId.empty() || Message.MessageType.empty()) {
    throw WorkflowError("Coordination message is missing its task scope or identity");
  }
  static const std::vector<std::string> AllowedTypes{
      "Question", "Finding", "Proposal", "Blocker", "Handoff",
      "Acknowledgement", "DecisionLink"};
  if (std::find(AllowedTypes.begin(), AllowedTypes.end(), Message.MessageType) ==
          AllowedTypes.end() ||
      (Message.Status != "Open" && Message.Status != "Closed")) {
    throw WorkflowError("Coordination message type/status is not allowed");
  }
  AppendLocked(MessagePath, SerializeTaskMessage(Message) + "\n");
}

TaskScopedMessage TaskScopedMessageStore::ResolveForTask(
    const std::string& TaskId,
    int TaskRevision,
    const std::string& MessageId) const {
  const auto OpenMessages = ReadOpenForTask(TaskId, TaskRevision);
  const auto It = std::find_if(OpenMessages.begin(), OpenMessages.end(),
                               [&](const TaskScopedMessage& Message) {
                                 return Message.MessageId == MessageId;
                               });
  if (It == OpenMessages.end()) {
    throw WorkflowError("Message is not open in the requested TaskId/TaskRevision scope");
  }
  TaskScopedMessage Closed = *It;
  Closed.Status = "Closed";
  Append(Closed);
  return Closed;
}

std::vector<TaskScopedMessage> TaskScopedMessageStore::ReadOpenForTask(
    const std::string& TaskId,
    int TaskRevision,
    std::size_t MaximumMessages) const {
  if (!IsTaskId(TaskId) || TaskRevision <= 0) {
    throw WorkflowError("Message query requires a valid TaskId and TaskRevision");
  }
  std::ifstream Input(MessagePath, std::ios::binary);
  if (!Input) {
    if (!std::filesystem::exists(MessagePath)) {
      return {};
    }
    throw WorkflowError("Unable to read coordination message log: " + MessagePath.string());
  }
  std::map<std::string, TaskScopedMessage> LatestByMessageId;
  std::string Line;
  std::size_t Scanned = 0;
  constexpr std::size_t MaximumScannedMessages = 100000;
  while (std::getline(Input, Line)) {
    if (++Scanned > MaximumScannedMessages) {
      throw WorkflowError("Coordination message log exceeds the bounded scan limit");
    }
    if (Line.empty()) {
      continue;
    }
    YAML::Node Node;
    try {
      Node = YAML::Load(Line);
    } catch (const YAML::Exception& Error) {
      throw WorkflowError("Invalid coordination JSONL record: " + std::string(Error.what()));
    }
    if (OptionalEventString(Node, "TaskId", "task_id") != TaskId ||
        EventInteger(Node, "TaskRevision", "task_revision") != TaskRevision) {
      continue;
    }
    TaskScopedMessage Message;
    Message.MessageId = OptionalEventString(Node, "MessageId", "message_id");
    Message.TaskId = TaskId;
    Message.TaskRevision = TaskRevision;
    Message.SenderAgentId = OptionalEventString(Node, "SenderAgentId", "sender_agent_id");
    Message.RecipientAgentId = OptionalEventString(Node, "RecipientAgentId", "recipient_agent_id");
    Message.MessageType = OptionalEventString(Node, "MessageType", "message_type");
    Message.Status = OptionalEventString(Node, "Status", "status");
    Message.Subject = OptionalEventString(Node, "Subject", "subject");
    Message.Summary = OptionalEventString(Node, "Summary", "summary");
    const YAML::Node Evidence = Node["EvidenceRefs"] ? Node["EvidenceRefs"] : Node["evidence_refs"];
    if (Evidence && Evidence.IsSequence()) {
      for (const YAML::Node& Reference : Evidence) {
        if (Reference.IsScalar()) {
          Message.EvidenceRefs.push_back(Reference.as<std::string>());
        }
      }
    }
    LatestByMessageId[Message.MessageId] = std::move(Message);
  }
  std::vector<TaskScopedMessage> Results;
  for (const auto& [MessageId, Message] : LatestByMessageId) {
    (void)MessageId;
    if (Message.Status == "Open") {
      Results.push_back(Message);
    }
  }
  if (Results.size() > MaximumMessages) {
    Results.erase(Results.begin(), Results.begin() +
                  static_cast<std::ptrdiff_t>(Results.size() - MaximumMessages));
  }
  return Results;
}

void WriteLocalReviewBundle(const std::filesystem::path& RepositoryRoot,
                            const TaskSpec& Task,
                            const ReviewBundleData& Bundle,
                            const std::filesystem::path& OutputPath,
                            const EventStore& Events) {
  if (!IsCommitId(Bundle.BaseCommit) || !IsCommitId(Bundle.HeadCommit)) {
    throw WorkflowError("ReviewBundle requires fixed BaseCommit and HeadCommit");
  }
  if (Bundle.ChangedPaths.empty()) {
    throw WorkflowError("ReviewBundle requires at least one changed path");
  }
  if (!Bundle.ChangeKinds.empty() &&
      Bundle.ChangeKinds.size() != Bundle.ChangedPaths.size()) {
    throw WorkflowError("ReviewBundle change status count does not match changed paths");
  }
  if (Bundle.AuthorAgentId.empty() || Bundle.ReviewerAgentId.empty() ||
      Bundle.AuthorAgentId == Bundle.ReviewerAgentId) {
    throw WorkflowError("ReviewBundle requires an independent Reviewer");
  }
  if (Bundle.RemotePrCreated) {
    throw WorkflowError("This bootstrap controller only emits an explicitly local ReviewBundle");
  }
  ValidateWriteSetPaths(Task, Bundle.ChangedPaths);
  const auto Root = CanonicalExistingDirectory(RepositoryRoot);
  if (!OutputPath.is_absolute()) {
    throw WorkflowError("Local ReviewBundle output path must be absolute");
  }
  for (const auto& Part : OutputPath) {
    if (Part == "..") {
      throw WorkflowError("ReviewBundle output path cannot contain parent traversal");
    }
  }
  const auto CanonicalOutputPath = CanonicalizeFuturePath(OutputPath);
  const auto CanonicalBundleRoot = CanonicalizeFuturePath(Root.parent_path() / "ReviewBundles");
  if (IsWithinPath(CanonicalOutputPath, Root) ||
      !IsWithinPath(CanonicalOutputPath, CanonicalBundleRoot)) {
    throw WorkflowError("Local ReviewBundle output must remain under the sibling ReviewBundles root");
  }
  if (std::filesystem::exists(CanonicalOutputPath)) {
    throw WorkflowError("Local ReviewBundle output already exists; refusing to overwrite evidence");
  }
  for (std::size_t Index = 0; Index < Bundle.ChangedPaths.size(); ++Index) {
    WorkflowEvent FileEvent;
    FileEvent.EventType = "FileChangeObserved";
    FileEvent.ReleaseId = Task.ReleaseId;
    FileEvent.PhaseId = Task.PhaseId;
    FileEvent.TaskId = Task.TaskId;
    FileEvent.TaskRevision = Task.TaskRevision;
    FileEvent.AgentId = Bundle.AuthorAgentId;
    FileEvent.Branch = Task.Branch;
    FileEvent.Result = Index < Bundle.ChangeKinds.size()
                           ? Bundle.ChangeKinds[Index]
                           : "Changed";
    FileEvent.EvidenceRefs = {Bundle.ChangedPaths[Index], Bundle.BaseCommit,
                              Bundle.HeadCommit};
    Events.Append(FileEvent);
  }
  ReviewBundleData CompleteBundle = Bundle;
  CompleteBundle.Events = Events.ReadTaskEvents(Task.TaskId);
  WriteExclusive(OutputPath, SerializeReviewBundle(Task, CompleteBundle) + "\n");

  WorkflowEvent BundleEvent;
  BundleEvent.EventType = "ReviewBundleCreated";
  BundleEvent.ReleaseId = Task.ReleaseId;
  BundleEvent.PhaseId = Task.PhaseId;
  BundleEvent.TaskId = Task.TaskId;
  BundleEvent.TaskRevision = Task.TaskRevision;
  BundleEvent.AgentId = Bundle.AuthorAgentId;
  BundleEvent.Branch = Task.Branch;
  BundleEvent.Result = "LocalReviewBundle";
  BundleEvent.EvidenceRefs = {OutputPath.string(), Bundle.BaseCommit, Bundle.HeadCommit};
  Events.Append(BundleEvent);
}

[[noreturn]] void RejectChangeApproval() {
  throw WorkflowError("ChangeRequest approval is owned by the Coordinator/Owner; the controller cannot approve scope changes");
}

[[noreturn]] void RejectMerge() {
  throw WorkflowError("Merge is an IntegrationOwner action; the controller never merges a task branch");
}

[[noreturn]] void RejectUnscopedHistoryRead() {
  throw WorkflowError("Unscoped task history queries are forbidden; provide one TaskId");
}

}  // namespace EmulativeDialog::WorkflowController
