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

struct ReadSetSelection {
  std::string Path;
  std::string RelativeEntry;
  std::string OutputEntry;
  std::string SelectorKey;
  std::string SelectorValue;
  bool HasSelector = false;
};

ReadSetSelection ParseReadSetSelection(const std::string& Entry) {
  const std::size_t Separator = Entry.find('#');
  const std::string Path = NormalizeRelativePath(
      Separator == std::string::npos ? Entry : Entry.substr(0, Separator));
  ReadSetSelection Selection;
  Selection.Path = Path;
  Selection.RelativeEntry = LowerAscii(Path);
  Selection.OutputEntry = Path;
  if (Separator == std::string::npos) {
    return Selection;
  }
  if (Entry.find('#', Separator + 1) != std::string::npos) {
    throw WorkflowError("ReadSet YAML selector must contain one fragment: " + Entry);
  }
  const std::string Fragment = Entry.substr(Separator + 1);
  const std::size_t Equals = Fragment.find('=');
  if (Equals == std::string::npos || Equals == 0 || Equals + 1 == Fragment.size() ||
      Fragment.find('=', Equals + 1) != std::string::npos) {
    throw WorkflowError("ReadSet YAML selector must use Key=Value syntax: " + Entry);
  }
  Selection.SelectorKey = Fragment.substr(0, Equals);
  Selection.SelectorValue = Fragment.substr(Equals + 1);
  const auto IsSelectorKeyChar = [](unsigned char C) {
    return std::isalnum(C) != 0 || C == '_' || C == '-';
  };
  const auto IsSelectorValueChar = [](unsigned char C) {
    return std::isalnum(C) != 0 || C == '_' || C == '-' || C == '.' || C == ':';
  };
  if (!std::all_of(Selection.SelectorKey.begin(), Selection.SelectorKey.end(),
                   IsSelectorKeyChar) ||
      !std::all_of(Selection.SelectorValue.begin(), Selection.SelectorValue.end(),
                   IsSelectorValueChar)) {
    throw WorkflowError("ReadSet YAML selector contains an unsupported character: " + Entry);
  }
  Selection.HasSelector = true;
  Selection.RelativeEntry += "#" + Selection.SelectorKey + "=" + Selection.SelectorValue;
  Selection.OutputEntry += "#" + Selection.SelectorKey + "=" + Selection.SelectorValue;
  return Selection;
}

void CollectYamlSelectorMatches(const YAML::Node& Node,
                                const std::string& Key,
                                const std::string& Value,
                                std::vector<YAML::Node>& Matches,
                                std::size_t& Visited,
                                std::size_t Depth) {
  if (++Visited > 100000 || Depth > 64) {
    throw WorkflowError("ReadSet YAML selector exceeded its bounded traversal limit");
  }
  if (Node.IsMap()) {
    const YAML::Node SelectedValue = Node[Key];
    if (SelectedValue && SelectedValue.IsScalar() && SelectedValue.as<std::string>() == Value) {
      Matches.push_back(Node);
      return;
    }
    if (SelectedValue && SelectedValue.IsSequence()) {
      for (const YAML::Node& Item : SelectedValue) {
        if (Item.IsScalar() && Item.as<std::string>() == Value) {
          Matches.push_back(Node);
          return;
        }
      }
    }
    if (Key.ends_with("Id")) {
      const YAML::Node PluralValue = Node[Key + "s"];
      if (PluralValue && PluralValue.IsSequence()) {
        for (const YAML::Node& Item : PluralValue) {
          if (Item.IsScalar() && Item.as<std::string>() == Value) {
            Matches.push_back(Node);
            return;
          }
        }
      }
    }
    for (const auto& Entry : Node) {
      CollectYamlSelectorMatches(Entry.second, Key, Value, Matches, Visited, Depth + 1);
    }
  } else if (Node.IsSequence()) {
    for (const YAML::Node& Item : Node) {
      CollectYamlSelectorMatches(Item, Key, Value, Matches, Visited, Depth + 1);
    }
  }
}

std::string ExtractYamlSelector(const std::string& Content,
                                const ReadSetSelection& Selection) {
  if (!Selection.HasSelector) {
    return Content;
  }
  YAML::Node Root;
  try {
    Root = YAML::Load(Content);
  } catch (const YAML::Exception& Error) {
    throw WorkflowError("Selected ReadSet YAML is invalid: " + std::string(Error.what()));
  }
  std::vector<YAML::Node> Matches;
  std::size_t Visited = 0;
  CollectYamlSelectorMatches(Root, Selection.SelectorKey, Selection.SelectorValue,
                             Matches, Visited, 0);
  if (Matches.size() != 1) {
    throw WorkflowError("ReadSet YAML selector must match exactly one entry: " +
                        Selection.RelativeEntry + " (matches=" +
                        std::to_string(Matches.size()) + ")");
  }
  return YAML::Dump(Matches.front()) + "\n";
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

bool IsSafePathSegment(const std::string& Value) {
  return !Value.empty() && Value != "." && Value != ".." &&
         std::all_of(Value.begin(), Value.end(), [](unsigned char C) {
           return std::isalnum(C) != 0 || C == '-' || C == '_';
         });
}

void RejectReparseComponents(const std::filesystem::path& Root,
                             const std::string& RelativePath) {
  std::filesystem::path Current = Root;
  for (const auto& Component : std::filesystem::path(RelativePath)) {
    Current /= Component;
    const DWORD Attributes = GetFileAttributesW(Current.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD Error = GetLastError();
      if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND) {
        break;
      }
      throw WorkflowError("Unable to inspect task-owned path component: " + Current.string());
    }
    if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw WorkflowError("Task-owned paths cannot traverse reparse points: " + RelativePath);
    }
  }
}

std::filesystem::path ResolveTaskOwnedPath(
    const std::filesystem::path& RepositoryRoot,
    const std::string& RelativePath,
    const std::string& AllowedRoot) {
  const std::string Normalized = NormalizeRelativePath(RelativePath);
  const std::string NormalizedAllowedRoot = NormalizeRelativePath(AllowedRoot);
  const std::string LowerPath = LowerAscii(Normalized);
  const std::string LowerAllowed = LowerAscii(NormalizedAllowedRoot);
  if (!LowerPath.starts_with(LowerAllowed + "/")) {
    throw WorkflowError("Task-owned path is outside its registered directory: " + RelativePath);
  }
  const auto Root = CanonicalExistingDirectory(RepositoryRoot);
  RejectReparseComponents(Root, Normalized);
  const auto Candidate = CanonicalizeFuturePath(Root / Normalized);
  const auto AllowedDirectory = CanonicalizeFuturePath(Root / NormalizedAllowedRoot);
  if (!IsWithinPath(Candidate, AllowedDirectory) || Candidate == AllowedDirectory ||
      !IsWithinPath(Candidate, Root)) {
    throw WorkflowError("Task-owned path escapes its registered repository directory: " +
                        RelativePath);
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
  Task.ReviewerAgentId = OptionalString(Node, "ReviewerAgentId");
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
    const YAML::Node StaticChecks = Validation["StaticChecks"];
    if (StaticChecks && !StaticChecks.IsNull()) {
      if (!StaticChecks.IsSequence()) {
        throw WorkflowError("StaticChecks must be a sequence");
      }
      for (const YAML::Node& Check : StaticChecks) {
        if (Check.IsScalar()) {
          const std::string Value = Check.as<std::string>();
          if (Value.starts_with("git diff --check ")) {
            Task.StaticChecks.push_back(Value);
          }
        } else if (!Check.IsMap() || Check.size() != 1) {
          throw WorkflowError("StaticChecks entries must be scalar rules or descriptive key/value items");
        }
      }
    }
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
  if (!Event.RawRecord.empty()) {
    return Event.RawRecord;
  }
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
         << "\"MaxReferenceFiles\":" << Event.MaxReferenceFiles << ','
         << "\"TaskSpecPath\":" << JsonEscape(RedactSensitiveText(Event.TaskSpecPath)) << ','
         << "\"TaskSpecCommit\":" << JsonEscape(Event.TaskSpecCommit) << ','
         << "\"TaskSpecBlobId\":" << JsonEscape(Event.TaskSpecBlobId) << ','
         << "\"AssignmentEventCommit\":" << JsonEscape(Event.AssignmentEventCommit) << ','
         << "\"AssignmentEventId\":" << JsonEscape(Event.AssignmentEventId) << ','
         << "\"CodeBaseCommit\":" << JsonEscape(Event.CodeBaseCommit) << ','
         << "\"HeadCommit\":" << JsonEscape(Event.HeadCommit) << ','
         << "\"BudgetCorrectionEventCommit\":"
         << JsonEscape(Event.BudgetCorrectionEventCommit) << ','
         << "\"BudgetCorrectionEventId\":" << JsonEscape(Event.BudgetCorrectionEventId) << ','
         << "\"CorrectionOfEventId\":" << JsonEscape(Event.CorrectionOfEventId) << ','
         << "\"CorrectedField\":" << JsonEscape(Event.CorrectedField) << ','
         << "\"OriginalValue\":" << JsonEscape(RedactSensitiveText(Event.OriginalApprovalReason)) << ','
         << "\"CorrectedValue\":" << JsonEscape(RedactSensitiveText(Event.CorrectedApprovalReason)) << ','
         << "\"ChangeRequestId\":" << JsonEscape(Event.ChangeRequestId) << ','
         << "\"ContextBudgetApprovalEventId\":"
         << JsonEscape(Event.ContextBudgetApprovalEventId) << ','
         << "\"OwnerAgentId\":" << JsonEscape(RedactSensitiveText(Event.OwnerAgentId)) << ','
         << "\"ReviewerAgentId\":" << JsonEscape(RedactSensitiveText(Event.ReviewerAgentId)) << ','
         << "\"Worktree\":" << JsonEscape(RedactSensitiveText(Event.Worktree)) << ','
         << "\"OutputSummary\":" << JsonEscape(RedactSensitiveText(Event.OutputSummary)) << ','
         << "\"ReadSetPaths\":" << JsonStringArray(Event.ReadSetPaths) << ','
         << "\"TargetEventId\":" << JsonEscape(Event.TargetEventId) << ','
         << "\"TargetReleaseId\":" << JsonEscape(Event.TargetReleaseId) << ','
         << "\"TargetPhaseId\":" << JsonEscape(Event.TargetPhaseId) << ','
         << "\"TargetTaskId\":" << JsonEscape(Event.TargetTaskId) << ','
         << "\"TargetTaskRevision\":" << Event.TargetTaskRevision << ','
         << "\"ExpectedVariantCount\":" << Event.ExpectedVariantCount << ','
         << "\"ExpectedOccurrenceCount\":" << Event.ExpectedOccurrenceCount << ','
         << "\"CanonicalOccurrence\":" << Event.CanonicalOccurrence
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

bool AppendLocked(const std::filesystem::path& Path,
                  const std::string& Line,
                  const std::function<bool()>* FlushOperation = nullptr,
                  const std::string& UniqueKey = {},
                  const std::string& UniqueValue = {},
                  const std::function<bool(const std::vector<std::string>&, bool)>&
                      BeforeAppend = {}) {
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
    std::vector<std::string> ExistingRecords;
    bool CompleteTail = true;
    if (!UniqueKey.empty() || BeforeAppend) {
      LARGE_INTEGER Beginning{};
      if (!SetFilePointerEx(Handle, Beginning, nullptr, FILE_BEGIN)) {
        throw WorkflowError("Unable to seek append-only log for uniqueness check: " +
                            Path.string());
      }
      std::array<char, 64U * 1024U> Buffer{};
      std::string PendingLine;
      std::size_t Scanned = 0;
      std::unordered_map<std::string, std::string> ExistingById;
      const auto CheckRecord = [&](const std::string& Record) {
        if (Record.empty()) {
          return;
        }
        if (++Scanned > 100000) {
          throw WorkflowError("Event log exceeds the bounded EventId check limit");
        }
        ExistingRecords.push_back(Record);
        if (UniqueKey.empty()) {
          return;
        }
        YAML::Node Node;
        try {
          Node = YAML::Load(Record);
        } catch (const YAML::Exception& Error) {
          throw WorkflowError("Invalid JSONL event record during uniqueness check: " +
                              std::string(Error.what()));
        }
        const std::string ExistingId = OptionalEventString(Node, UniqueKey, "event_id");
        if (ExistingId.empty()) {
          throw WorkflowError("Event log record is missing its unique identifier");
        }
        const auto [Existing, Inserted] = ExistingById.emplace(ExistingId, Record);
        if (!Inserted && Existing->second != Record) {
          throw WorkflowError("Conflicting records reuse EventId: " + ExistingId);
        }
        if (ExistingId == UniqueValue) {
          throw WorkflowError("Duplicate EventId rejected: " + UniqueValue);
        }
      };
      bool SawBytes = false;
      bool LastByteWasNewline = true;
      for (;;) {
        DWORD BytesRead = 0;
        if (!ReadFile(Handle, Buffer.data(), static_cast<DWORD>(Buffer.size()),
                      &BytesRead, nullptr)) {
          throw WorkflowError("Unable to read append-only log for uniqueness check: " +
                              Path.string());
        }
        if (BytesRead == 0) {
          break;
        }
        SawBytes = true;
        LastByteWasNewline = Buffer[BytesRead - 1] == '\n';
        PendingLine.append(Buffer.data(), BytesRead);
        if (PendingLine.size() > 4U * 1024U * 1024U &&
            PendingLine.find('\n') == std::string::npos) {
          throw WorkflowError("Event log record exceeds the uniqueness-check byte limit");
        }
        std::size_t Newline = PendingLine.find('\n');
        while (Newline != std::string::npos) {
          CheckRecord(PendingLine.substr(0, Newline));
          PendingLine.erase(0, Newline + 1);
          Newline = PendingLine.find('\n');
        }
      }
      if (!PendingLine.empty()) {
        CompleteTail = false;
        CheckRecord(PendingLine);
      } else if (SawBytes) {
        CompleteTail = LastByteWasNewline;
      }
    }
    if (BeforeAppend && !BeforeAppend(ExistingRecords, CompleteTail)) {
      const bool Flushed = FlushOperation != nullptr
                               ? (*FlushOperation)()
                               : FlushFileBuffers(Handle) != 0;
      if (!Flushed) {
        const DWORD Error = GetLastError();
        throw WorkflowError("Unable to confirm existing append durability (Windows error " +
                            std::to_string(Error) + "): " + Path.string());
      }
      UnlockFileEx(Handle, 0, MAXDWORD, MAXDWORD, &Overlap);
      CloseHandle(Handle);
      return false;
    }
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
    const bool Flushed = FlushOperation != nullptr
                             ? (*FlushOperation)()
                             : FlushFileBuffers(Handle) != 0;
    if (!Flushed) {
      const DWORD Error = GetLastError();
      throw WorkflowError("Unable to flush append-only log (Windows error " +
                          std::to_string(Error) + "): " + Path.string());
    }
    UnlockFileEx(Handle, 0, MAXDWORD, MAXDWORD, &Overlap);
    CloseHandle(Handle);
    return true;
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
         << "\"branch\":" << JsonEscape(RedactSensitiveText(Task.Branch)) << ','
         << "\"worktree\":" << JsonEscape(RedactSensitiveText(Task.Worktree)) << ','
         << "\"title\":" << JsonEscape(RedactSensitiveText(Task.Title)) << ','
         << "\"goal\":" << JsonEscape(RedactSensitiveText(Task.Goal)) << ','
         << "\"author_agent_id\":" << JsonEscape(RedactSensitiveText(Bundle.AuthorAgentId)) << ','
         << "\"reviewer_agent_id\":" << JsonEscape(RedactSensitiveText(Bundle.ReviewerAgentId)) << ','
         << "\"task_spec_path\":" << JsonEscape(Bundle.TaskSpecPath) << ','
         << "\"task_spec_commit\":" << JsonEscape(Bundle.TaskSpecCommit) << ','
         << "\"task_spec_blob\":" << JsonEscape(Bundle.TaskSpecBlobId) << ','
         << "\"task_spec_blob_id\":" << JsonEscape(Bundle.TaskSpecBlobId) << ','
         << "\"assignment_event_commit\":" << JsonEscape(Bundle.AssignmentEventCommit) << ','
         << "\"assignment_event_id\":" << JsonEscape(Bundle.AssignmentEventId) << ','
         << "\"code_base_commit\":" << JsonEscape(Bundle.CodeBaseCommit) << ','
         << "\"budget_correction_event_commit\":"
         << JsonEscape(Bundle.BudgetCorrectionEventCommit) << ','
         << "\"budget_correction_event_id\":"
         << JsonEscape(Bundle.BudgetCorrectionEventId) << ','
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
           << "\"event_id\":" << JsonEscape(Validation.EventId) << ','
           << "\"command\":" << JsonEscape(RedactSensitiveText(Validation.Command)) << ','
           << "\"working_directory\":" << JsonEscape(Validation.WorkingDirectory.string()) << ','
           << "\"exit_code\":" << Validation.ExitCode << ','
           << "\"evidence_path\":" << JsonEscape(Validation.EvidencePath.string()) << ','
           << "\"head_commit\":" << JsonEscape(Validation.HeadCommit)
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

std::filesystem::path ResolveTaskEventPath(
    const std::filesystem::path& RepositoryRoot,
    const TaskSpec& Task) {
  const std::string EventShard = NormalizeRelativePath(Task.EventShard);
  const std::string LowerShard = LowerAscii(EventShard);
  if (!IsTaskId(Task.TaskId) || !LowerShard.starts_with("workflow/events/") ||
      EventShard.find(':') != std::string::npos ||
      std::filesystem::path(EventShard).extension() != ".jsonl") {
    throw WorkflowError("TaskSpec EventShard must be a .jsonl path under Workflow/Events");
  }
  return ResolveTaskOwnedPath(RepositoryRoot, EventShard, "Workflow/Events");
}

std::filesystem::path ResolveTaskCheckpointPath(
    const std::filesystem::path& RepositoryRoot,
    const TaskSpec& Task) {
  if (!IsTaskId(Task.TaskId) || Task.TaskRevision <= 0 ||
      !IsSafePathSegment(Task.ReleaseId) || !IsSafePathSegment(Task.PhaseId)) {
    throw WorkflowError("Checkpoint route has an invalid ReleaseId, PhaseId, TaskId, or revision");
  }
  const std::string RelativePath =
      "Workflow/Checkpoints/" + Task.ReleaseId + "/" + Task.PhaseId + "/" +
      Task.TaskId + "-r" + std::to_string(Task.TaskRevision) + ".yaml";
  return ResolveTaskOwnedPath(RepositoryRoot, RelativePath, "Workflow/Checkpoints");
}

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

TaskSpec LoadTaskSpecFromPinnedSnapshot(
    const std::filesystem::path& RepositoryRoot,
    const std::string& PhaseId,
    const std::string& TaskId,
    const std::string& TaskSpecCommit,
    const std::string& TaskSpecBlobId,
    const std::string& AssignmentEventCommit,
    const std::string& AssignmentEventId,
    const std::string& BudgetCorrectionEventCommit,
    const std::string& BudgetCorrectionEventId,
    ProcessRunner& Runner) {
  if (!IsTaskId(TaskId) || !IsCommitId(TaskSpecCommit) ||
      !IsCommitId(TaskSpecBlobId) || !IsCommitId(AssignmentEventCommit) ||
      AssignmentEventId.empty() || !IsCommitId(BudgetCorrectionEventCommit) ||
      BudgetCorrectionEventId.empty()) {
    throw WorkflowError("Pinned TaskSpec request contains an invalid ID or object pin");
  }
  const auto Root = CanonicalExistingDirectory(RepositoryRoot);
  const ProcessResult TaskIndexResult = RunGit(
      Runner, Root, {"show", TaskSpecCommit + ":Workflow/TaskIndex.yaml"});
  YAML::Node TaskIndex;
  try {
    TaskIndex = YAML::Load(TaskIndexResult.Output);
  } catch (const YAML::Exception& Error) {
    throw WorkflowError("Pinned TaskIndex YAML is invalid: " + std::string(Error.what()));
  }
  const YAML::Node Plans = TaskIndex["PhasePlans"];
  if (!Plans || !Plans.IsSequence()) {
    throw WorkflowError("Pinned TaskIndex has no PhasePlans sequence");
  }
  std::string TaskPlanPath;
  for (const YAML::Node& Plan : Plans) {
    if (OptionalString(Plan, "PhaseId") == PhaseId) {
      TaskPlanPath = NormalizeRelativePath(RequiredString(Plan, "Path"));
      break;
    }
  }
  if (TaskPlanPath.empty() || !TaskPlanPath.starts_with("Workflow/Plans/")) {
    throw WorkflowError("Pinned TaskIndex does not route the requested phase to a task plan");
  }
  const std::string PinnedBlob = TrimCommandOutput(RunGit(
      Runner, Root, {"rev-parse", TaskSpecCommit + ":" + TaskPlanPath}).Output);
  if (PinnedBlob != TaskSpecBlobId) {
    throw WorkflowError("TaskSpecBlobId does not match the task plan at TaskSpecCommit");
  }
  const ProcessResult PlanResult = RunGit(
      Runner, Root, {"show", TaskSpecCommit + ":" + TaskPlanPath});
  YAML::Node PhasePlan;
  try {
    PhasePlan = YAML::Load(PlanResult.Output);
  } catch (const YAML::Exception& Error) {
    throw WorkflowError("Pinned phase plan YAML is invalid: " + std::string(Error.what()));
  }
  TaskSpec Task = ParseTaskSpec(FindTaskNode(PhasePlan, TaskId), PhaseId);
  Task.TaskPlanPath = TaskPlanPath;
  Task.ReleaseId = OptionalString(TaskIndex, "CurrentReleaseId", "Bootstrap");
  const YAML::Node CommunicationStore = TaskIndex["CommunicationStore"];
  const std::string CommunicationRoot = CommunicationStore
                                            ? OptionalString(CommunicationStore, "Root")
                                            : std::string{};
  if (!CommunicationRoot.empty()) {
    Task.CommunicationShard = NormalizeRelativePath(
        CommunicationRoot + "/" + Task.ReleaseId + "/" + PhaseId + ".jsonl");
  }
  if (Task.TaskId != TaskId || !Task.RegisteredBudgetOverride ||
      Task.RegisteredBudgetOverride->TaskRevision != Task.TaskRevision ||
      !IsCommitId(Task.BaselineCommit) ||
      !IsAllowedBranch(Task.TaskId, Task.Branch) || Task.OwnerAgentId.empty() ||
      Task.ReviewerAgentId.empty() || Task.OwnerAgentId == Task.ReviewerAgentId ||
      !std::filesystem::path(Task.Worktree).is_absolute()) {
    throw WorkflowError("Pinned TaskSpec identity or registered budget revision is inconsistent");
  }
  if (Task.EventShard.empty() ||
      !NormalizeRelativePath(Task.EventShard).starts_with("Workflow/Events/")) {
    throw WorkflowError("Pinned TaskSpec EventShard must remain under Workflow/Events");
  }

  const ProcessResult BaseAncestor = RunGit(
      Runner, Root, {"merge-base", "--is-ancestor", Task.BaselineCommit, TaskSpecCommit});
  (void)BaseAncestor;
  (void)RunGit(Runner, Root,
               {"merge-base", "--is-ancestor", TaskSpecCommit, AssignmentEventCommit});
  (void)RunGit(Runner, Root,
               {"merge-base", "--is-ancestor", AssignmentEventCommit,
                BudgetCorrectionEventCommit});
  const ProcessResult AssignmentLog = RunGit(
      Runner, Root, {"show", AssignmentEventCommit + ":" + Task.EventShard});
  YAML::Node Assignment;
  std::istringstream Lines(AssignmentLog.Output);
  std::string Line;
  while (std::getline(Lines, Line)) {
    if (Line.empty()) {
      continue;
    }
    YAML::Node Candidate;
    try {
      Candidate = YAML::Load(Line);
    } catch (const YAML::Exception& Error) {
      throw WorkflowError("Pinned assignment event log contains invalid JSONL: " +
                          std::string(Error.what()));
    }
    if (OptionalEventString(Candidate, "EventId", "event_id") == AssignmentEventId) {
      Assignment = Candidate;
      break;
    }
  }
  if (!Assignment || OptionalEventString(Assignment, "EventType", "event_type") !=
                         "TaskReworkStarted" ||
      OptionalEventString(Assignment, "TaskId", "task_id") != Task.TaskId ||
      EventInteger(Assignment, "TaskRevision", "task_revision") != Task.TaskRevision ||
      OptionalEventString(Assignment, "TaskSpecCommit", "task_spec_commit") != TaskSpecCommit ||
      OptionalEventString(Assignment, "TaskSpecBlobId", "task_spec_blob_id") != TaskSpecBlobId ||
      OptionalEventString(Assignment, "TaskSpecPath", "task_spec_path") != Task.TaskPlanPath ||
      OptionalEventString(Assignment, "CodeBaseCommit", "code_base_commit") != Task.BaselineCommit ||
      OptionalEventString(Assignment, "BaseCommit", "base_commit") != Task.BaselineCommit ||
      OptionalEventString(Assignment, "ContextBudgetApprovalEventId",
                          "context_budget_approval_event_id") !=
          Task.RegisteredBudgetOverride->ApprovalEventId ||
      OptionalEventString(Assignment, "OwnerAgentId", "owner_agent_id") != Task.OwnerAgentId ||
      OptionalEventString(Assignment, "ReviewerAgentId", "reviewer_agent_id") !=
          Task.ReviewerAgentId ||
      OptionalEventString(Assignment, "Branch", "branch") != Task.Branch ||
      OptionalEventString(Assignment, "Worktree", "worktree") != Task.Worktree) {
    throw WorkflowError("AssignmentEvent does not bind the frozen TaskSpec and CodeBase");
  }

  const ProcessResult CorrectionLog = RunGit(
      Runner, Root, {"show", BudgetCorrectionEventCommit + ":" + Task.EventShard});
  YAML::Node Correction;
  YAML::Node OriginalApproval;
  std::istringstream CorrectionLines(CorrectionLog.Output);
  while (std::getline(CorrectionLines, Line)) {
    if (Line.empty()) {
      continue;
    }
    YAML::Node Candidate;
    try {
      Candidate = YAML::Load(Line);
    } catch (const YAML::Exception& Error) {
      throw WorkflowError("Pinned budget correction log contains invalid JSONL: " +
                          std::string(Error.what()));
    }
    const std::string CandidateId = OptionalEventString(Candidate, "EventId", "event_id");
    if (CandidateId == BudgetCorrectionEventId) {
      Correction = Candidate;
    } else if (CandidateId == Task.RegisteredBudgetOverride->ApprovalEventId) {
      OriginalApproval = Candidate;
    }
  }
  std::vector<std::string> CorrectedReadSetPaths;
  if (Correction && Correction["ReadSetPaths"] && Correction["ReadSetPaths"].IsSequence()) {
    for (const YAML::Node& Path : Correction["ReadSetPaths"]) {
      if (Path.IsScalar()) {
        CorrectedReadSetPaths.push_back(Path.as<std::string>());
      }
    }
  }
  if (!Correction ||
      OptionalEventString(Correction, "EventType", "event_type") !=
          "ContextBudgetExceptionCorrection" ||
      OptionalEventString(Correction, "CorrectionOfEventId", "correction_of_event_id") !=
          Task.RegisteredBudgetOverride->ApprovalEventId ||
      OptionalEventString(Correction, "TaskId", "task_id") != Task.TaskId ||
      EventInteger(Correction, "TaskRevision", "task_revision") != Task.TaskRevision ||
      OptionalEventString(Correction, "AgentRole", "agent_role") != "Coordinator" ||
      OptionalEventString(Correction, "Result", "result").empty() ||
      OptionalEventString(Correction, "CorrectedField", "corrected_field") != "ApprovalReason" ||
      OptionalEventString(Correction, "OriginalValue", "original_value") !=
          OptionalEventString(OriginalApproval, "ApprovalReason", "approval_reason") ||
      OptionalEventString(Correction, "TaskSpecCommit", "task_spec_commit") != TaskSpecCommit ||
      OptionalEventString(Correction, "TaskSpecBlobId", "task_spec_blob_id") != TaskSpecBlobId ||
      OptionalEventString(Correction, "TaskSpecPath", "task_spec_path") != Task.TaskPlanPath ||
      OptionalEventString(Correction, "AssignmentEventCommit", "assignment_event_commit") !=
          AssignmentEventCommit ||
      OptionalEventString(Correction, "AssignmentEventId", "assignment_event_id") !=
          AssignmentEventId ||
      OptionalEventString(Correction, "CodeBaseCommit", "code_base_commit") != Task.BaselineCommit ||
      OptionalEventString(Correction, "ChangeRequestId", "change_request_id") !=
          "CR-ED-0008-003" ||
      EventSize(Correction, "MaxReferenceFiles", "max_reference_files") !=
          Task.RegisteredBudgetOverride->MaxReferenceFiles ||
      CorrectedReadSetPaths != Task.ReadSet ||
      OptionalEventString(Correction, "CorrectedValue", "corrected_value") !=
          Task.RegisteredBudgetOverride->Reason ||
      OptionalEventString(OriginalApproval, "EventType", "event_type") !=
          "ContextBudgetExceptionApproved" ||
      OptionalEventString(OriginalApproval, "TaskId", "task_id") != Task.TaskId ||
      EventInteger(OriginalApproval, "TaskRevision", "task_revision") != Task.TaskRevision ||
      OptionalEventString(OriginalApproval, "AgentRole", "agent_role") != "Coordinator" ||
      OptionalEventString(OriginalApproval, "Result", "result") != "Approved" ||
      EventSize(OriginalApproval, "MaxReferenceFiles", "max_reference_files") !=
          Task.RegisteredBudgetOverride->MaxReferenceFiles) {
    throw WorkflowError("Budget correction event does not bind the frozen Revision4 TaskSpec and approval");
  }
  const YAML::Node OriginalReadSet = OriginalApproval["ReadSetPaths"]
                                         ? OriginalApproval["ReadSetPaths"]
                                         : OriginalApproval["read_set_paths"];
  if (!OriginalReadSet || !OriginalReadSet.IsSequence() ||
      StringSequence(OriginalApproval, "ReadSetPaths") != Task.ReadSet) {
    throw WorkflowError("Original Coordinator budget approval has a different ReadSet");
  }

  Task.TaskSpecCommit = TaskSpecCommit;
  Task.TaskSpecBlobId = TaskSpecBlobId;
  Task.AssignmentEventCommit = AssignmentEventCommit;
  Task.AssignmentEventId = AssignmentEventId;
  Task.CodeBaseCommit = Task.BaselineCommit;
  Task.BudgetCorrectionEventCommit = BudgetCorrectionEventCommit;
  Task.BudgetCorrectionEventId = BudgetCorrectionEventId;
  return Task;
}


std::vector<EventIdConflictAuthorization>
LoadEventIdConflictAuthorizationsFromPinnedSnapshot(
    const std::filesystem::path& RepositoryRoot,
    const std::string& PhaseId,
    const std::string& TaskId,
    int TaskRevision,
    const std::string& TaskSpecCommit,
    const std::string& TaskSpecBlobId,
    ProcessRunner& Runner) {
  if (PhaseId != "P1" || TaskId != "ED-0014" || TaskRevision != 9 ||
      !IsCommitId(TaskSpecCommit) || !IsCommitId(TaskSpecBlobId)) {
    throw WorkflowError("EventId recovery requires the pinned ED-0014 revision 8 TaskSpec");
  }
  const auto Root = CanonicalExistingDirectory(RepositoryRoot);
  const ProcessResult IndexResult = RunGit(
      Runner, Root, {"show", TaskSpecCommit + ":Workflow/TaskIndex.yaml"});
  YAML::Node Index;
  try {
    Index = YAML::Load(IndexResult.Output);
  } catch (const YAML::Exception& Error) {
    throw WorkflowError("Pinned TaskIndex YAML is invalid: " + std::string(Error.what()));
  }
  const YAML::Node Plans = Index["PhasePlans"];
  if (!Plans || !Plans.IsSequence()) {
    throw WorkflowError("Pinned TaskIndex has no PhasePlans sequence");
  }
  std::string PlanPath;
  for (const YAML::Node& Plan : Plans) {
    if (OptionalString(Plan, "PhaseId") == PhaseId) {
      PlanPath = NormalizeRelativePath(RequiredString(Plan, "Path"));
      break;
    }
  }
  if (PlanPath.empty() || !PlanPath.starts_with("Workflow/Plans/")) {
    throw WorkflowError("Pinned TaskIndex does not route P1 to a task plan");
  }
  const std::string ActualBlob = TrimCommandOutput(
      RunGit(Runner, Root, {"rev-parse", TaskSpecCommit + ":" + PlanPath}).Output);
  if (ActualBlob != TaskSpecBlobId) {
    throw WorkflowError("Pinned TaskSpec blob does not match TaskSpec commit");
  }
  const std::string TaskPattern = "  - TaskId: " + TaskId;
  const ProcessResult TaskMatches = RunGit(
      Runner, Root, {"grep", "-n", "-F", TaskPattern, TaskSpecCommit, "--", PlanPath});
  std::istringstream MatchLines(TaskMatches.Output);
  std::string MatchLine;
  std::size_t MatchCount = 0;
  while (std::getline(MatchLines, MatchLine)) {
    if (!MatchLine.empty()) {
      ++MatchCount;
    }
  }
  if (MatchCount != 1) {
    throw WorkflowError("Pinned task plan must contain exactly one current TaskId record");
  }
  const ProcessResult TaskEntryResult = RunGit(
      Runner, Root, {"grep", "-n", "-A", "500", "-F", TaskPattern,
                     TaskSpecCommit, "--", PlanPath});
  const std::string MatchPrefix = TaskSpecCommit + ":" + PlanPath + ":";
  const std::string ContextPrefix = TaskSpecCommit + ":" + PlanPath + "-";
  std::istringstream EntryLines(TaskEntryResult.Output);
  std::string EntryLine;
  std::string TaskYaml;
  bool FoundTask = false;
  bool ReachedNextTask = false;
  while (std::getline(EntryLines, EntryLine)) {
    if (!EntryLine.empty() && EntryLine.back() == '\r') {
      EntryLine.pop_back();
    }
    std::string SourceLine;
    if (EntryLine.starts_with(MatchPrefix)) {
      const std::string NumberAndText = EntryLine.substr(MatchPrefix.size());
      const std::size_t Separator = NumberAndText.find(':');
      if (Separator == std::string::npos) {
        throw WorkflowError("Pinned task selection output is malformed");
      }
      SourceLine = NumberAndText.substr(Separator + 1);
    } else if (EntryLine.starts_with(ContextPrefix)) {
      const std::string NumberAndText = EntryLine.substr(ContextPrefix.size());
      const std::size_t Separator = NumberAndText.find('-');
      if (Separator == std::string::npos) {
        throw WorkflowError("Pinned task context output is malformed");
      }
      SourceLine = NumberAndText.substr(Separator + 1);
    } else {
      continue;
    }
    if (!FoundTask) {
      if (SourceLine == TaskPattern) {
        FoundTask = true;
        TaskYaml = "Tasks:\n" + SourceLine + "\n";
      }
      continue;
    }
    if (SourceLine.starts_with("  - TaskId: ")) {
      if (SourceLine == TaskPattern) {
        throw WorkflowError("Pinned task plan contains a duplicate current TaskId record");
      }
      ReachedNextTask = true;
      break;
    }
    TaskYaml += SourceLine + "\n";
  }
  if (!FoundTask || !ReachedNextTask) {
    throw WorkflowError("Pinned task selection did not capture one complete TaskId record");
  }
  YAML::Node Plan;
  try {
    Plan = YAML::Load(TaskYaml);
  } catch (const YAML::Exception& Error) {
    throw WorkflowError("Pinned TaskSpec task record YAML is invalid: " +
                        std::string(Error.what()));
  }
  const YAML::Node Task = FindTaskNode(Plan, TaskId);
  if (OptionalString(Task, "PhaseId", PhaseId) != PhaseId ||
      Task["TaskRevision"].as<int>(0) != TaskRevision ||
      OptionalString(Task, "State") != "Ready") {
    throw WorkflowError("Pinned ED-0014 TaskSpec identity or state is not authorized");
  }
  const std::vector<std::pair<std::string, std::string>> ApprovedChanges{
      {"ChangeRequest", "CR-ED-0014-001"},
      {"RevisionChangeRequest", "CR-ED-0014-002"},
      {"AdditionalChangeRequest", "CR-ED-0014-003"},
      {"FinalChangeRequest", "CR-ED-0014-004"},
      {"ValidationChangeRequest", "CR-ED-0014-005"}};
  for (const auto& [Field, ExpectedId] : ApprovedChanges) {
    const YAML::Node Change = Task[Field];
    if (!Change || OptionalString(Change, "ChangeRequestId") != ExpectedId ||
        OptionalString(Change, "Decision") != "Accepted" ||
        OptionalString(Change, "ApprovedByRole") != "Coordinator" ||
        OptionalString(Change, "ApprovedByAgentId") != "/root") {
      throw WorkflowError("Pinned ED-0014 TaskSpec has an unapproved recovery change request");
    }
  }
  const YAML::Node Rules = Task["Rules"];
  const YAML::Node Overrides = Rules ? Rules["ApprovedOverrides"] : YAML::Node{};
  if (!Overrides || !Overrides.IsSequence() || Overrides.size() != 5) {
    throw WorkflowError("Pinned ED-0014 TaskSpec must contain exactly five conflict approvals");
  }
  struct ExpectedTarget {
    const char* Id;
    int Revision;
  };
  static constexpr ExpectedTarget ExpectedTargets[] = {
      {"6202e631-52ce-4145-96af-ce6934df7749", 1},
      {"5d773818-7efa-4055-b2ea-600d0dcaa69d", 1},
      {"72cc74ea-b68c-4588-95ca-58e0f6c22685", 1},
      {"4a9507a0-c338-40f0-8871-f15aff82be1c", 1},
      {"378e2cbe-98e9-4419-8418-287ef16b7c74", 2}};
  const std::string ExpectedShard = "Workflow/Events/Bootstrap/P1.jsonl";
  std::vector<EventIdConflictAuthorization> Authorizations;
  Authorizations.reserve(5);
  std::unordered_map<std::string, bool> Seen;
  const std::string ReleaseId = OptionalString(Index, "CurrentReleaseId", "Bootstrap");
  const std::string Branch = OptionalString(Task, "Branch");
  for (const YAML::Node& Override : Overrides) {
    EventIdConflictAuthorization Authorization;
    Authorization.EventShard =
        NormalizeRelativePath(RequiredString(Override, "EventShard"));
    Authorization.TargetEventId = RequiredString(Override, "TargetEventId");
    const YAML::Node Scope = Override["Scope"];
    if (!Scope || !Scope.IsMap()) {
      throw WorkflowError("Conflict approval requires a full target scope");
    }
    Authorization.TargetReleaseId = RequiredString(Scope, "ReleaseId");
    Authorization.TargetPhaseId = RequiredString(Scope, "PhaseId");
    Authorization.TargetTaskId = RequiredString(Scope, "TaskId");
    Authorization.TargetTaskRevision = Scope["TaskRevision"].as<int>(0);
    Authorization.ExpectedVariantCount =
        Override["ExpectedVariantCount"].as<std::size_t>(0);
    Authorization.ExpectedOccurrenceCount =
        Override["ExpectedOccurrenceCount"].as<std::size_t>(0);
    Authorization.CanonicalOccurrence =
        Override["CanonicalOccurrence"].as<std::size_t>(0);
    Authorization.ApprovalReason = RequiredString(Override, "ApprovalReason");
    Authorization.ChangeRequestId = RequiredString(Override, "ChangeRequestId");
    Authorization.ApprovedByRole = RequiredString(Override, "ApprovedByRole");
    Authorization.ApprovedByAgentId = RequiredString(Override, "ApprovedByAgentId");
    if (RequiredString(Override, "RuleRef") != "EventIdConflictResolution" ||
        Authorization.ChangeRequestId != "CR-ED-0014-001" ||
        RequiredString(Override, "ApprovedByRole") != "Coordinator" ||
        Authorization.ApprovedByAgentId != "/root" ||
        Authorization.EventShard != ExpectedShard ||
        Authorization.TargetReleaseId != "Bootstrap" ||
        Authorization.TargetPhaseId != "P1" ||
        Authorization.TargetTaskId != "ED-0013" ||
        (Authorization.ExpectedVariantCount != 2) ||
        (Authorization.ExpectedOccurrenceCount != 2) ||
        Authorization.CanonicalOccurrence != 2 ||
        Authorization.ApprovalReason.empty() ||
        !Seen.emplace(Authorization.TargetEventId, true).second) {
      throw WorkflowError("Pinned EventId conflict approval has unexpected scope or values");
    }
    const auto Expected = std::find_if(
        std::begin(ExpectedTargets), std::end(ExpectedTargets),
        [&](const ExpectedTarget& Target) {
          return Authorization.TargetEventId == Target.Id;
        });
    if (Expected == std::end(ExpectedTargets) ||
        Authorization.TargetTaskRevision != Expected->Revision) {
      throw WorkflowError("Pinned TaskSpec contains an unauthorized EventId target");
    }
    Authorization.TaskSpecCommit = TaskSpecCommit;
    Authorization.TaskSpecBlobId = TaskSpecBlobId;
    Authorization.TaskPlanPath = PlanPath;
    Authorization.ResolutionReleaseId = ReleaseId;
    Authorization.ResolutionPhaseId = PhaseId;
    Authorization.ResolutionTaskId = TaskId;
    Authorization.ResolutionTaskRevision = TaskRevision;
    Authorization.ResolutionBranch = Branch;
    Authorizations.push_back(std::move(Authorization));
  }
  if (Seen.size() != std::size(ExpectedTargets)) {
    throw WorkflowError("Pinned TaskSpec is missing one or more required EventId approvals");
  }
  return Authorizations;
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
  const std::string Candidate = ParseReadSetSelection(RelativePath).RelativeEntry;
  const bool Authorized = std::any_of(
      Task.ReadSet.begin(), Task.ReadSet.end(), [&](const std::string& Allowed) {
        return ParseReadSetSelection(Allowed).RelativeEntry == Candidate;
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
    const std::optional<VerifiedContextBudgetOverride>& BudgetOverride,
    ProcessRunner* PinnedSnapshotRunner) {
  std::size_t AllowedReferences = Task.MaxReferenceFiles;
  if (AllowedReferences > 12) {
    if (!BudgetOverride || BudgetOverride->Value().TaskId != Task.TaskId ||
        BudgetOverride->Value().TaskRevision != Task.TaskRevision ||
        BudgetOverride->Value().MaxReferenceFiles < AllowedReferences ||
        BudgetOverride->Value().ApprovedByRole != "Coordinator" ||
        BudgetOverride->Value().Reason.empty() ||
        !BudgetOverride->Value().ApprovalEventRef.starts_with("Workflow/Events/") ||
        BudgetOverride->Value().ApprovalEventRef.find("..") != std::string::npos ||
        BudgetOverride->TaskSpecCommit() != Task.TaskSpecCommit ||
        BudgetOverride->TaskSpecBlobId() != Task.TaskSpecBlobId ||
        BudgetOverride->AssignmentEventCommit() != Task.AssignmentEventCommit ||
        BudgetOverride->AssignmentEventId() != Task.AssignmentEventId ||
        BudgetOverride->BudgetCorrectionEventCommit() != Task.BudgetCorrectionEventCommit ||
        BudgetOverride->BudgetCorrectionEventId() != Task.BudgetCorrectionEventId) {
      throw WorkflowError("ReadSet budget above 12 requires a matching Coordinator exception event");
    }
    AllowedReferences = BudgetOverride->Value().MaxReferenceFiles;
  } else if (BudgetOverride) {
    if (BudgetOverride->Value().TaskId != Task.TaskId ||
        BudgetOverride->Value().TaskRevision != Task.TaskRevision ||
        BudgetOverride->Value().MaxReferenceFiles < Task.ReadSet.size() ||
        BudgetOverride->Value().ApprovedByRole != "Coordinator" ||
        BudgetOverride->Value().Reason.empty() ||
        !BudgetOverride->Value().ApprovalEventRef.starts_with("Workflow/Events/") ||
        BudgetOverride->Value().ApprovalEventRef.find("..") != std::string::npos ||
        BudgetOverride->TaskSpecCommit() != Task.TaskSpecCommit ||
        BudgetOverride->TaskSpecBlobId() != Task.TaskSpecBlobId ||
        BudgetOverride->AssignmentEventCommit() != Task.AssignmentEventCommit ||
        BudgetOverride->AssignmentEventId() != Task.AssignmentEventId ||
        BudgetOverride->BudgetCorrectionEventCommit() != Task.BudgetCorrectionEventCommit ||
        BudgetOverride->BudgetCorrectionEventId() != Task.BudgetCorrectionEventId) {
      throw WorkflowError("Invalid Coordinator ReadSet budget exception");
    }
    AllowedReferences = BudgetOverride->Value().MaxReferenceFiles;
  }
  if (Task.ReadSet.size() > AllowedReferences || AllowedReferences > 64) {
    throw WorkflowError("ReadSet exceeds the registered reference-file budget");
  }

  std::vector<ContextFile> Context;
  Context.reserve(Task.ReadSet.size());
  std::size_t TotalBytes = 0;
  const bool PinnedSnapshot = !Task.TaskSpecCommit.empty();
  if (PinnedSnapshot && (!PinnedSnapshotRunner || !IsCommitId(Task.CodeBaseCommit))) {
    throw WorkflowError("Pinned ReadSet assembly requires its CodeBaseCommit and Git ProcessRunner");
  }
  const auto CanonicalRoot = PinnedSnapshot
                                 ? std::filesystem::path{}
                                 : CanonicalExistingDirectory(RepositoryRoot);
  for (const std::string& RelativePath : Task.ReadSet) {
    const ReadSetSelection Selection = ParseReadSetSelection(RelativePath);
    ValidateReadSetPath(Task, RelativePath);
    std::string Content;
    constexpr std::size_t MaximumSelectorSourceBytes = 32U * 1024U * 1024U;
    if (PinnedSnapshot) {
      const std::string TreeEntry = TrimCommandOutput(RunGit(
          *PinnedSnapshotRunner, RepositoryRoot,
          {"ls-tree", Task.CodeBaseCommit, "--", Selection.Path}).Output);
      const std::size_t FirstSpace = TreeEntry.find(' ');
      const std::size_t SecondSpace = TreeEntry.find(' ', FirstSpace == std::string::npos
                                                            ? FirstSpace
                                                            : FirstSpace + 1);
      const std::size_t Tab = TreeEntry.find('\t');
      const std::string Mode = FirstSpace == std::string::npos
                                   ? std::string{}
                                   : TreeEntry.substr(0, FirstSpace);
      const std::string ObjectType =
          FirstSpace == std::string::npos || SecondSpace == std::string::npos
              ? std::string{}
              : TreeEntry.substr(FirstSpace + 1, SecondSpace - FirstSpace - 1);
      if (TreeEntry.empty() || FirstSpace == std::string::npos ||
          SecondSpace == std::string::npos || Tab == std::string::npos ||
          (Mode != "100644" && Mode != "100755") || ObjectType != "blob" ||
          TreeEntry.substr(Tab + 1) != Selection.Path) {
        throw WorkflowError("Pinned ReadSet entry is missing, a symlink, or not a regular Git blob: " +
                            RelativePath);
      }
      Content = RunGit(*PinnedSnapshotRunner, RepositoryRoot,
                       {"show", Task.CodeBaseCommit + ":" + Selection.Path}).Output;
      if (Selection.HasSelector && Content.size() > MaximumSelectorSourceBytes) {
        throw WorkflowError("Pinned selector source exceeds its bounded YAML parse size: " +
                            RelativePath);
      }
    } else {
      const auto FullPath = ResolveRepoPath(RepositoryRoot, Selection.Path);
      const std::string CanonicalRelativePath =
          FullPath.lexically_relative(CanonicalRoot).generic_string();
      const std::string CanonicalReadSetEntry =
          CanonicalRelativePath + (Selection.HasSelector
                                       ? "#" + Selection.SelectorKey + "=" + Selection.SelectorValue
                                       : std::string{});
      ValidateReadSetPath(Task, CanonicalReadSetEntry);
      std::error_code Error;
      if (!std::filesystem::is_regular_file(FullPath, Error) || Error) {
        throw WorkflowError("ReadSet entries must resolve to existing files: " + RelativePath);
      }
      const auto Size = std::filesystem::file_size(FullPath, Error);
      if (Error || (Selection.HasSelector && Size > MaximumSelectorSourceBytes) ||
          (!Selection.HasSelector && Size > MaximumFileBytes)) {
        throw WorkflowError("ReadSet file or bundle exceeds the configured context byte limit: " +
                            RelativePath);
      }
      std::ifstream Input(FullPath, std::ios::binary);
      if (!Input) {
        throw WorkflowError("Unable to read ReadSet file: " + RelativePath);
      }
      Content.assign(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
    }
    Content = ExtractYamlSelector(Content, Selection);
    const std::size_t Size = Content.size();
    if (Size > MaximumFileBytes || TotalBytes + Size > MaximumTotalBytes) {
      throw WorkflowError("ReadSet file or bundle exceeds the configured context byte limit: " +
                          RelativePath);
    }
    TotalBytes += Content.size();
    Context.push_back({Selection.OutputEntry, RedactSensitiveText(Content)});
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

WorktreeResult VerifyExistingWorktreeForReview(const WorktreeRequest& Request,
                                               const std::string& TaskState,
                                               ProcessRunner& Runner) {
  if (TaskState != "Review" && TaskState != "Rework") {
    throw WorkflowError("Read-only ReviewBundle verification requires Review or Rework state");
  }
  const auto RepositoryRoot = CanonicalExistingDirectory(Request.RepositoryRoot);
  const auto RequestedWorktree = Request.Assignment.Worktree;
  if (!IsTaskId(Request.Assignment.TaskId) ||
      !IsAllowedBranch(Request.Assignment.TaskId, Request.Assignment.Branch) ||
      !IsCommitId(Request.Assignment.BaseCommit) || !RequestedWorktree.is_absolute()) {
    throw WorkflowError("Review worktree request lacks a valid TaskId, branch, path, or BaseCommit");
  }
  const ProcessResult SourceTopLevel = RunGit(
      Runner, RepositoryRoot, {"rev-parse", "--show-toplevel"});
  const auto CanonicalSource = CanonicalExistingDirectory(
      std::filesystem::path(TrimCommandOutput(SourceTopLevel.Output)));
  if (!PathsEqual(CanonicalSource, RepositoryRoot)) {
    throw WorkflowError("RepositoryRoot must be the source worktree top-level directory");
  }
  const auto CanonicalWorktree = CanonicalExistingDirectory(RequestedWorktree);
  if (IsWithinPath(CanonicalWorktree, CanonicalSource)) {
    throw WorkflowError("Assigned worktree must be outside the original source worktree");
  }
  const std::string SourceBranchBefore = TrimCommandOutput(
      RunGit(Runner, CanonicalSource, {"branch", "--show-current"}).Output);
  const std::string SourceHeadBefore = TrimCommandOutput(
      RunGit(Runner, CanonicalSource, {"rev-parse", "HEAD"}).Output);
  const std::string SourceStatusBefore = RunGit(
      Runner, CanonicalSource, {"status", "--porcelain", "--untracked-files=all"}).Output;

  const auto WorktreeTopLevel = RunGit(
      Runner, CanonicalWorktree, {"rev-parse", "--show-toplevel"});
  const auto CanonicalRegisteredTopLevel = CanonicalExistingDirectory(
      std::filesystem::path(TrimCommandOutput(WorktreeTopLevel.Output)));
  if (!PathsEqual(CanonicalRegisteredTopLevel, CanonicalWorktree)) {
    throw WorkflowError("Review path is not the registered worktree top-level directory");
  }
  const std::string ActualBranch = TrimCommandOutput(
      RunGit(Runner, CanonicalWorktree, {"branch", "--show-current"}).Output);
  if (ActualBranch != Request.Assignment.Branch ||
      !IsRegisteredWorktree(Runner, CanonicalSource, CanonicalWorktree, ActualBranch)) {
    throw WorkflowError("Review worktree registration or branch does not match the assignment pin");
  }
  const std::string WorktreeStatus = RunGit(
      Runner, CanonicalWorktree, {"status", "--porcelain", "--untracked-files=all"}).Output;
  if (!TrimCommandOutput(WorktreeStatus).empty()) {
    throw WorkflowError("ReviewBundle requires a clean worktree at the fixed HeadCommit");
  }
  const std::string HeadCommit = TrimCommandOutput(
      RunGit(Runner, CanonicalWorktree, {"rev-parse", "HEAD"}).Output);
  if (!IsCommitId(HeadCommit)) {
    throw WorkflowError("Review worktree HEAD is not a valid commit id");
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
    throw WorkflowError("Read-only ReviewBundle verification changed the source worktree");
  }
  return {CanonicalWorktree, ActualBranch, Request.Assignment.BaseCommit, HeadCommit, false};
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
  if (!IsCommitId(Task.FixedHeadCommit)) {
    throw WorkflowError("Validation must be bound to the fixed assigned HeadCommit");
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
  if (!Task.TaskSpecCommit.empty()) {
    const std::string HeadAfter = TrimCommandOutput(Runner.Run(
        {"git", "-C", WorkingDirectory.string(), "rev-parse", "HEAD"},
        WorkingDirectory).Output);
    const std::string StatusAfter = Runner.Run(
        {"git", "-C", WorkingDirectory.string(), "status", "--porcelain",
         "--untracked-files=all"}, WorkingDirectory).Output;
    if (HeadAfter != Task.FixedHeadCommit || !TrimCommandOutput(StatusAfter).empty()) {
      Process.Output += "\nController rejected validation evidence because the assigned worktree changed during execution.\n";
      if (Process.ExitCode == 0) {
        Process.ExitCode = 1;
      }
    }
  }
  WriteExclusive(EvidencePath, RedactSensitiveText(Process.Output));

  WorkflowEvent Event;
  Event.EventId = NewRecordId();
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
  Event.TaskSpecPath = Task.TaskPlanPath;
  Event.TaskSpecCommit = Task.TaskSpecCommit;
  Event.TaskSpecBlobId = Task.TaskSpecBlobId;
  Event.AssignmentEventCommit = Task.AssignmentEventCommit;
  Event.AssignmentEventId = Task.AssignmentEventId;
  Event.CodeBaseCommit = Task.CodeBaseCommit;
  Event.HeadCommit = Task.FixedHeadCommit;
  Event.BudgetCorrectionEventCommit = Task.BudgetCorrectionEventCommit;
  Event.BudgetCorrectionEventId = Task.BudgetCorrectionEventId;
  Events.Append(Event);

  return {ExactCommand, WorkingDirectory, Process.ExitCode, EvidencePath, Event.EventId,
          Task.FixedHeadCommit};
}

ValidationResult RunTypedGitDiffCheck(const TaskSpec& Task,
                                      const std::string& HeadCommit,
                                      const std::filesystem::path& WorkingDirectory,
                                      const std::filesystem::path& EvidencePath,
                                      ProcessRunner& Runner,
                                      const EventStore& Events,
                                      const std::string& AgentId,
                                      const std::string& CorrelationId) {
  static const std::string FrozenRule = "git diff --check <CodeBaseCommit> <HeadCommit>";
  if (std::find(Task.StaticChecks.begin(), Task.StaticChecks.end(), FrozenRule) ==
      Task.StaticChecks.end()) {
    throw WorkflowError("Typed GitDiffCheck is not listed in the frozen TaskSpec");
  }
  if (!IsCommitId(Task.CodeBaseCommit) || Task.CodeBaseCommit != Task.BaselineCommit ||
      !IsCommitId(HeadCommit) || Task.FixedHeadCommit != HeadCommit) {
    throw WorkflowError("Typed GitDiffCheck requires the pinned CodeBaseCommit and fixed HeadCommit");
  }
  if (!WorkingDirectory.is_absolute() || !std::filesystem::is_directory(WorkingDirectory) ||
      !EvidencePath.is_absolute() || std::filesystem::exists(EvidencePath)) {
    throw WorkflowError("Typed GitDiffCheck requires an existing worktree and new absolute evidence path");
  }
  const auto CanonicalWorkingDirectory = CanonicalExistingDirectory(WorkingDirectory);
  const auto CanonicalEvidencePath = CanonicalizeFuturePath(EvidencePath);
  if (IsWithinPath(CanonicalEvidencePath, CanonicalWorkingDirectory)) {
    throw WorkflowError("Typed GitDiffCheck evidence must remain outside the source worktree");
  }
  const auto BuildSibling = CanonicalWorkingDirectory.parent_path() /
                            (CanonicalWorkingDirectory.filename().string() + ".build");
  const auto CanonicalBuildSibling = CanonicalizeFuturePath(BuildSibling);
  if (!IsWithinPath(CanonicalEvidencePath, CanonicalBuildSibling)) {
    throw WorkflowError("Typed GitDiffCheck evidence must remain under the worktree sibling build directory");
  }

  const std::string ExactCommand =
      "git diff --check " + Task.CodeBaseCommit + " " + HeadCommit;
  const ProcessResult Process = Runner.Run(
      {"git", "-C", CanonicalWorkingDirectory.string(), "diff", "--check",
       Task.CodeBaseCommit, HeadCommit}, CanonicalWorkingDirectory);
  std::ostringstream Evidence;
  Evidence << "Command=" << ExactCommand << "\nExitCode=" << Process.ExitCode << "\n"
           << RedactSensitiveText(Process.Output);
  WriteExclusive(EvidencePath, Evidence.str());

  WorkflowEvent Event;
  Event.EventId = NewRecordId();
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
  Event.WorkingDirectory = CanonicalWorkingDirectory.string();
  Event.ExitCode = static_cast<long>(Process.ExitCode);
  Event.EvidencePath = EvidencePath.string();
  Event.EvidenceRefs = {EvidencePath.string(), Task.CodeBaseCommit, HeadCommit};
  Event.TaskSpecPath = Task.TaskPlanPath;
  Event.TaskSpecCommit = Task.TaskSpecCommit;
  Event.TaskSpecBlobId = Task.TaskSpecBlobId;
  Event.AssignmentEventCommit = Task.AssignmentEventCommit;
  Event.AssignmentEventId = Task.AssignmentEventId;
  Event.CodeBaseCommit = Task.CodeBaseCommit;
  Event.HeadCommit = HeadCommit;
  Event.BudgetCorrectionEventCommit = Task.BudgetCorrectionEventCommit;
  Event.BudgetCorrectionEventId = Task.BudgetCorrectionEventId;
  Events.Append(Event);
  return {ExactCommand, CanonicalWorkingDirectory, Process.ExitCode, EvidencePath,
          Event.EventId, HeadCommit};
}

std::string RedactSensitiveText(const std::string& Text) {
  std::string Result = Text;
  static const std::regex AuthorizationBearer(
      R"((authorization\s*[:=]\s*Bearer\s+)(?:"[^"]*"|'[^']*'|[^\s,;&]+))",
      std::regex::icase);
  static const std::regex KeyValue(
      R"(((?:api[_-]?(?:key|token)|(?:access|refresh)[_-]?token|token|password|passwd|secret|authorization)\s*[:=]\s*)(?:"[^"]*"|'[^']*'|[^\s,;&]+))",
      std::regex::icase);
  static const std::regex Bearer(
      R"((Bearer\s+)(?:"[^"]*"|'[^']*'|[A-Za-z0-9._~+/=-]+))",
                                 std::regex::icase);
  static const std::regex FlagValue(
      R"(((?:--api-key|--access-token|--token|--password|--secret)\s+)(?:"[^"]*"|'[^']*'|[^\s,;&]+))",
      std::regex::icase);
  Result = std::regex_replace(Result, AuthorizationBearer, "$1[REDACTED]");
  Result = std::regex_replace(Result, Bearer, "$1[REDACTED]");
  Result = std::regex_replace(Result, KeyValue, "$1[REDACTED]");
  Result = std::regex_replace(Result, FlagValue, "$1[REDACTED]");
  return Result;
}


namespace {

struct EventLogRow {
  WorkflowEvent Event;
  std::string RawRecord;
  std::size_t PhysicalLine = 0;
};

struct EventLogSnapshot {
  std::vector<std::string> Records;
  bool CompleteTail = true;
};

EventLogSnapshot ReadEventLogSnapshot(
    const std::filesystem::path& Path,
    const std::function<bool()>* FlushOperation) {
  HANDLE Handle = CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (Handle == INVALID_HANDLE_VALUE) {
    const DWORD Error = GetLastError();
    if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND ||
        !std::filesystem::exists(Path)) {
      return {};
    }
    throw WorkflowError("Unable to open event log for durable read: " + Path.string());
  }
  OVERLAPPED Overlap{};
  if (!LockFileEx(Handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &Overlap)) {
    CloseHandle(Handle);
    throw WorkflowError("Unable to lock event log for durable read: " + Path.string());
  }
  std::string Bytes;
  try {
    const bool Flushed = FlushOperation != nullptr
                             ? (*FlushOperation)()
                             : FlushFileBuffers(Handle) != 0;
    if (!Flushed) {
      const DWORD Error = GetLastError();
      throw WorkflowError("Unable to confirm event log durability (Windows error " +
                          std::to_string(Error) + "): " + Path.string());
    }
    LARGE_INTEGER Beginning{};
    if (!SetFilePointerEx(Handle, Beginning, nullptr, FILE_BEGIN)) {
      throw WorkflowError("Unable to seek event log for durable read: " + Path.string());
    }
    std::array<char, 64U * 1024U> Buffer{};
    for (;;) {
      DWORD BytesRead = 0;
      if (!ReadFile(Handle, Buffer.data(), static_cast<DWORD>(Buffer.size()),
                    &BytesRead, nullptr)) {
        throw WorkflowError("Unable to read event log: " + Path.string());
      }
      if (BytesRead == 0) {
        break;
      }
      Bytes.append(Buffer.data(), BytesRead);
      if (Bytes.size() > 64U * 1024U * 1024U) {
        throw WorkflowError("Event log exceeds the bounded scan size");
      }
    }
    UnlockFileEx(Handle, 0, MAXDWORD, MAXDWORD, &Overlap);
    CloseHandle(Handle);
  } catch (...) {
    UnlockFileEx(Handle, 0, MAXDWORD, MAXDWORD, &Overlap);
    CloseHandle(Handle);
    throw;
  }

  EventLogSnapshot Snapshot;
  Snapshot.CompleteTail = Bytes.empty() || Bytes.back() == '\n';
  std::size_t Start = 0;
  for (;;) {
    const std::size_t Newline = Bytes.find('\n', Start);
    if (Newline == std::string::npos) {
      if (Start < Bytes.size()) {
        Snapshot.Records.push_back(Bytes.substr(Start));
      }
      break;
    }
    Snapshot.Records.push_back(Bytes.substr(Start, Newline - Start));
    Start = Newline + 1;
    if (Start == Bytes.size()) {
      break;
    }
  }
  return Snapshot;
}

WorkflowEvent ParseWorkflowEventRecord(const std::string& SourceRecord) {
  std::string Record = SourceRecord;
  if (!Record.empty() && Record.back() == '\r') {
    Record.pop_back();
  }
  YAML::Node Node;
  try {
    Node = YAML::Load(Record);
  } catch (const YAML::Exception& Error) {
    throw WorkflowError("Invalid JSONL event record: " + std::string(Error.what()));
  }
  if (!Node.IsMap()) {
    throw WorkflowError("Event record must be a JSON object");
  }
  WorkflowEvent Event;
  Event.RawRecord = Record;
  Event.EventId = OptionalEventString(Node, "EventId", "event_id");
  if (Event.EventId.empty()) {
    throw WorkflowError("Structured event is missing its EventId");
  }
  Event.EventType = OptionalEventString(Node, "EventType", "event_type");
  Event.OccurredAt = OptionalEventString(Node, "OccurredAt", "occurred_at");
  Event.ReleaseId = OptionalEventString(Node, "ReleaseId", "release_id");
  Event.PhaseId = OptionalEventString(Node, "PhaseId", "phase_id");
  Event.TaskId = OptionalEventString(Node, "TaskId", "task_id");
  Event.TaskRevision = EventInteger(Node, "TaskRevision", "task_revision");
  Event.AgentId = OptionalEventString(Node, "AgentId", "agent_id");
  Event.AgentRole = OptionalEventString(Node, "AgentRole", "agent_role");
  Event.Branch = OptionalEventString(Node, "Branch", "branch");
  Event.CorrelationId = OptionalEventString(Node, "CorrelationId", "correlation_id");
  Event.Result = OptionalEventString(Node, "Result", "result");
  const YAML::Node Evidence =
      Node["EvidenceRefs"] ? Node["EvidenceRefs"] : Node["evidence_refs"];
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
  Event.TaskSpecPath = OptionalEventString(Node, "TaskSpecPath", "task_spec_path");
  Event.TaskSpecCommit = OptionalEventString(Node, "TaskSpecCommit", "task_spec_commit");
  Event.TaskSpecBlobId = OptionalEventString(Node, "TaskSpecBlobId", "task_spec_blob_id");
  Event.AssignmentEventCommit =
      OptionalEventString(Node, "AssignmentEventCommit", "assignment_event_commit");
  Event.AssignmentEventId = OptionalEventString(Node, "AssignmentEventId", "assignment_event_id");
  Event.CodeBaseCommit = OptionalEventString(Node, "CodeBaseCommit", "code_base_commit");
  Event.HeadCommit = OptionalEventString(Node, "HeadCommit", "head_commit");
  Event.BudgetCorrectionEventCommit =
      OptionalEventString(Node, "BudgetCorrectionEventCommit", "budget_correction_event_commit");
  Event.BudgetCorrectionEventId =
      OptionalEventString(Node, "BudgetCorrectionEventId", "budget_correction_event_id");
  Event.CorrectionOfEventId = OptionalEventString(Node, "CorrectionOfEventId", "correction_of_event_id");
  Event.CorrectedField = OptionalEventString(Node, "CorrectedField", "corrected_field");
  Event.OriginalApprovalReason = OptionalEventString(Node, "OriginalValue", "original_value");
  Event.CorrectedApprovalReason = OptionalEventString(Node, "CorrectedValue", "corrected_value");
  Event.ChangeRequestId = OptionalEventString(Node, "ChangeRequestId", "change_request_id");
  Event.ContextBudgetApprovalEventId =
      OptionalEventString(Node, "ContextBudgetApprovalEventId", "context_budget_approval_event_id");
  Event.OwnerAgentId = OptionalEventString(Node, "OwnerAgentId", "owner_agent_id");
  Event.ReviewerAgentId = OptionalEventString(Node, "ReviewerAgentId", "reviewer_agent_id");
  Event.Worktree = OptionalEventString(Node, "Worktree", "worktree");
  Event.OutputSummary = OptionalEventString(Node, "OutputSummary", "output_summary");
  const YAML::Node ReadSetPaths =
      Node["ReadSetPaths"] ? Node["ReadSetPaths"] : Node["read_set_paths"];
  if (ReadSetPaths && ReadSetPaths.IsSequence()) {
    for (const YAML::Node& PathValue : ReadSetPaths) {
      if (PathValue.IsScalar()) {
        Event.ReadSetPaths.push_back(PathValue.as<std::string>());
      }
    }
  }
  Event.TargetEventId = OptionalEventString(Node, "TargetEventId", "target_event_id");
  Event.TargetReleaseId = OptionalEventString(Node, "TargetReleaseId", "target_release_id");
  Event.TargetPhaseId = OptionalEventString(Node, "TargetPhaseId", "target_phase_id");
  Event.TargetTaskId = OptionalEventString(Node, "TargetTaskId", "target_task_id");
  Event.TargetTaskRevision = EventInteger(Node, "TargetTaskRevision", "target_task_revision");
  Event.ExpectedVariantCount = EventSize(Node, "ExpectedVariantCount", "expected_variant_count");
  Event.ExpectedOccurrenceCount =
      EventSize(Node, "ExpectedOccurrenceCount", "expected_occurrence_count");
  Event.CanonicalOccurrence = EventSize(Node, "CanonicalOccurrence", "canonical_occurrence");
  return Event;
}

std::vector<EventLogRow> ParseEventLogRows(const std::vector<std::string>& Records) {
  if (Records.size() > 100000) {
    throw WorkflowError("Event log exceeds the bounded scan limit");
  }
  std::vector<EventLogRow> Rows;
  Rows.reserve(Records.size());
  std::size_t LineNumber = 0;
  for (const std::string& Raw : Records) {
    ++LineNumber;
    if (Raw.empty() || Raw == "\r") {
      continue;
    }
    if (Raw.size() > 4U * 1024U * 1024U) {
      throw WorkflowError("Event log record exceeds the bounded record size");
    }
    EventLogRow Row;
    Row.Event = ParseWorkflowEventRecord(Raw);
    Row.RawRecord = Row.Event.RawRecord;
    Row.PhysicalLine = LineNumber;
    Rows.push_back(std::move(Row));
  }
  return Rows;
}

void ValidateResolutionEnvelope(const EventLogRow& Row) {
  const WorkflowEvent& Event = Row.Event;
  if (Event.EventType != "EventIdConflictResolution") {
    return;
  }
  if (Event.TaskId != "ED-0014" || Event.TaskRevision != 9 ||
      Event.AgentId != "/root" || Event.AgentRole != "Coordinator" ||
      Event.EventId.empty() || Event.ReleaseId != "Bootstrap" ||
      Event.PhaseId != "P1" || Event.Branch.empty() ||
      Event.CorrelationId != "ED-0014-R9-" + Event.TargetEventId ||
      Event.Result != "ResolvedOccurrenceSelected" ||
      Event.TargetEventId.empty() || Event.TargetReleaseId != "Bootstrap" ||
      Event.TargetPhaseId != "P1" || Event.TargetTaskId != "ED-0013" ||
      Event.TargetTaskRevision <= 0 || Event.ExpectedVariantCount != 2 ||
      Event.ExpectedOccurrenceCount != 2 || Event.CanonicalOccurrence != 2 ||
      !IsCommitId(Event.TaskSpecCommit) || !IsCommitId(Event.TaskSpecBlobId) ||
      Event.ChangeRequestId != "CR-ED-0014-001" || Event.ApprovalReason.empty() ||
      Event.TaskSpecPath != "Workflow/Plans/P1.yaml") {
    throw WorkflowError("EventId conflict resolution envelope or approval pins are invalid");
  }
  static const std::unordered_map<std::string, int> TargetRevisions{
      {"6202e631-52ce-4145-96af-ce6934df7749", 1},
      {"5d773818-7efa-4055-b2ea-600d0dcaa69d", 1},
      {"72cc74ea-b68c-4588-95ca-58e0f6c22685", 1},
      {"4a9507a0-c338-40f0-8871-f15aff82be1c", 1},
      {"378e2cbe-98e9-4419-8418-287ef16b7c74", 2}};
  const auto Expected = TargetRevisions.find(Event.TargetEventId);
  if (Expected == TargetRevisions.end() ||
      Expected->second != Event.TargetTaskRevision) {
    throw WorkflowError("EventId conflict resolution names an unapproved target scope");
  }
  YAML::Node Node = YAML::Load(Row.RawRecord);
  static const std::vector<std::string> AllowedResolutionFields{
      "EventId", "EventType", "OccurredAt", "ReleaseId", "PhaseId", "TaskId",
      "TaskRevision", "AgentId", "AgentRole", "Branch", "CorrelationId", "Result",
      "EvidenceRefs", "Command", "WorkingDirectory", "ExitCode", "EvidencePath",
      "ApprovalReason", "MaxReferenceFiles", "TaskSpecPath", "TaskSpecCommit",
      "TaskSpecBlobId", "AssignmentEventCommit", "AssignmentEventId",
      "CodeBaseCommit", "HeadCommit", "BudgetCorrectionEventCommit",
      "BudgetCorrectionEventId", "CorrectionOfEventId", "CorrectedField",
      "OriginalValue", "CorrectedValue", "ChangeRequestId",
      "ContextBudgetApprovalEventId", "OwnerAgentId", "ReviewerAgentId",
      "Worktree", "OutputSummary", "ReadSetPaths", "TargetEventId",
      "TargetReleaseId", "TargetPhaseId", "TargetTaskId", "TargetTaskRevision",
      "ExpectedVariantCount", "ExpectedOccurrenceCount", "CanonicalOccurrence"};
  std::unordered_map<std::string, bool> SeenFields;
  for (const auto& Entry : Node) {
    if (!Entry.first.IsScalar()) {
      throw WorkflowError("Resolution records cannot contain non-scalar field names");
    }
    const std::string Key = Entry.first.as<std::string>();
    if (std::find(AllowedResolutionFields.begin(), AllowedResolutionFields.end(), Key) ==
        AllowedResolutionFields.end()) {
      throw WorkflowError("Resolution record contains an unknown or prohibited field: " + Key);
    }
    if (!SeenFields.emplace(Key, true).second) {
      throw WorkflowError("Resolution record contains a duplicate field: " + Key);
    }
  }
  if (!Event.Command.empty() || !Event.WorkingDirectory.empty() ||
      !Event.EvidencePath.empty() || !Event.OutputSummary.empty() ||
      !Event.ReadSetPaths.empty() || Event.EvidenceRefs.size() != 5 ||
      std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(),
                Event.TaskSpecCommit) == Event.EvidenceRefs.end() ||
      std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(),
                Event.TaskSpecBlobId) == Event.EvidenceRefs.end() ||
      std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(),
                Event.ChangeRequestId) == Event.EvidenceRefs.end() ||
      std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(),
                Event.TargetEventId) == Event.EvidenceRefs.end() ||
      std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(),
                Event.TaskSpecPath + "#TaskId=ED-0014") == Event.EvidenceRefs.end()) {
    throw WorkflowError("Resolution record must contain only the pinned safe evidence references");
  }
}

bool SameTargetScope(const WorkflowEvent& Left, const WorkflowEvent& Right) {
  return Left.ReleaseId == Right.ReleaseId && Left.PhaseId == Right.PhaseId &&
         Left.TaskId == Right.TaskId && Left.TaskRevision == Right.TaskRevision;
}

struct ResolvedEventProjection {
  std::vector<EventLogRow> SelectedRows;
};

ResolvedEventProjection ProjectEventLog(const std::vector<std::string>& Records,
                                       bool AllowUnresolved) {
  const std::vector<EventLogRow> Rows = ParseEventLogRows(Records);
  std::unordered_map<std::string, std::vector<std::size_t>> RowsById;
  for (std::size_t Index = 0; Index < Rows.size(); ++Index) {
    RowsById[Rows[Index].Event.EventId].push_back(Index);
  }
  std::unordered_map<std::string, std::size_t> ResolutionByTarget;
  std::unordered_map<std::string, std::size_t> UniqueResolutionById;
  for (std::size_t Index = 0; Index < Rows.size(); ++Index) {
    if (Rows[Index].Event.EventType != "EventIdConflictResolution") {
      continue;
    }
    ValidateResolutionEnvelope(Rows[Index]);
    const auto IdGroup = RowsById.find(Rows[Index].Event.EventId);
    if (IdGroup == RowsById.end()) {
      throw WorkflowError("Resolution record disappeared from the EventId scan");
    }
    const std::size_t First = IdGroup->second.front();
    for (const std::size_t DuplicateIndex : IdGroup->second) {
      if (Rows[DuplicateIndex].RawRecord != Rows[First].RawRecord) {
        throw WorkflowError("Conflicting records reuse a resolution EventId");
      }
    }
    if (!UniqueResolutionById.emplace(Rows[Index].Event.EventId, Index).second) {
      continue;
    }
    if (!ResolutionByTarget.emplace(Rows[Index].Event.TargetEventId, Index).second) {
      throw WorkflowError("More than one resolution targets the same EventId");
    }
  }

  ResolvedEventProjection Projection;
  std::unordered_map<std::string, bool> HandledTargets;
  std::unordered_map<std::string, bool> ProcessedIds;
  for (const auto& [EventId, Group] : RowsById) {
    if (!ProcessedIds.emplace(EventId, true).second) {
      continue;
    }
    std::vector<std::string> DistinctVariants;
    for (const std::size_t Index : Group) {
      if (std::find(DistinctVariants.begin(), DistinctVariants.end(),
                    Rows[Index].RawRecord) == DistinctVariants.end()) {
        DistinctVariants.push_back(Rows[Index].RawRecord);
      }
    }
    if (DistinctVariants.size() > 1 &&
        Rows[Group.front()].Event.EventType == "EventIdConflictResolution") {
      throw WorkflowError("Conflicting EventId resolution records cannot be resolved recursively");
    }
    const auto Resolution = ResolutionByTarget.find(EventId);
    if (DistinctVariants.size() == 1) {
      if (Resolution != ResolutionByTarget.end()) {
        throw WorkflowError("EventId resolution is stale because its target is no longer conflicted");
      }
      Projection.SelectedRows.push_back(Rows[Group.front()]);
      continue;
    }
    if (Resolution == ResolutionByTarget.end()) {
      if (AllowUnresolved) {
        continue;
      }
      throw WorkflowError("Conflicting records reuse EventId: " + EventId);
    }
    const WorkflowEvent& ResolutionEvent = Rows[Resolution->second].Event;
    std::size_t LastTargetIndex = 0;
    bool FirstTarget = true;
    for (const std::size_t Index : Group) {
      if (FirstTarget || Rows[Index].PhysicalLine > Rows[LastTargetIndex].PhysicalLine) {
        LastTargetIndex = Index;
        FirstTarget = false;
      }
      if (!SameTargetScope(Rows[Group.front()].Event, Rows[Index].Event)) {
        throw WorkflowError("Conflicting EventId variants have different scopes");
      }
    }
    if (ResolutionEvent.ExpectedVariantCount != DistinctVariants.size() ||
        ResolutionEvent.ExpectedOccurrenceCount != Group.size() ||
        ResolutionEvent.CanonicalOccurrence == 0 ||
        ResolutionEvent.CanonicalOccurrence > Group.size() ||
        Rows[Resolution->second].PhysicalLine <= Rows[LastTargetIndex].PhysicalLine) {
      throw WorkflowError("EventId resolution is stale or precedes a target occurrence");
    }
    const EventLogRow& Canonical =
        Rows[Group[ResolutionEvent.CanonicalOccurrence - 1]];
    if (Canonical.Event.ReleaseId != ResolutionEvent.TargetReleaseId ||
        Canonical.Event.PhaseId != ResolutionEvent.TargetPhaseId ||
        Canonical.Event.TaskId != ResolutionEvent.TargetTaskId ||
        Canonical.Event.TaskRevision != ResolutionEvent.TargetTaskRevision) {
      throw WorkflowError("Canonical EventId occurrence does not match the approved target scope");
    }
    Projection.SelectedRows.push_back(Canonical);
    HandledTargets.emplace(EventId, true);
  }
  for (const auto& [TargetId, ResolutionIndex] : ResolutionByTarget) {
    (void)ResolutionIndex;
    if (HandledTargets.find(TargetId) == HandledTargets.end()) {
      throw WorkflowError("EventId resolution references a missing or unconflicted target");
    }
  }
  std::sort(Projection.SelectedRows.begin(), Projection.SelectedRows.end(),
            [](const EventLogRow& Left, const EventLogRow& Right) {
              return Left.PhysicalLine < Right.PhysicalLine;
            });
  return Projection;
}

bool HasUnsafeResolutionId(const std::vector<EventLogRow>& Rows,
                           const std::string& EventId) {
  return std::any_of(Rows.begin(), Rows.end(), [&](const EventLogRow& Row) {
    return Row.Event.EventId == EventId;
  });
}

bool SameResolutionPayload(const WorkflowEvent& Existing,
                           const WorkflowEvent& Expected) {
  return Existing.EventId == Expected.EventId &&
         Existing.EventType == Expected.EventType &&
         Existing.ReleaseId == Expected.ReleaseId &&
         Existing.PhaseId == Expected.PhaseId &&
         Existing.TaskId == Expected.TaskId &&
         Existing.TaskRevision == Expected.TaskRevision &&
         Existing.AgentId == Expected.AgentId &&
         Existing.AgentRole == Expected.AgentRole &&
         Existing.Branch == Expected.Branch &&
         Existing.CorrelationId == Expected.CorrelationId &&
         Existing.Result == Expected.Result &&
         Existing.EvidenceRefs == Expected.EvidenceRefs &&
         Existing.TaskSpecPath == Expected.TaskSpecPath &&
         Existing.TaskSpecCommit == Expected.TaskSpecCommit &&
         Existing.TaskSpecBlobId == Expected.TaskSpecBlobId &&
         Existing.ChangeRequestId == Expected.ChangeRequestId &&
         Existing.ApprovalReason == Expected.ApprovalReason &&
         Existing.TargetEventId == Expected.TargetEventId &&
         Existing.TargetReleaseId == Expected.TargetReleaseId &&
         Existing.TargetPhaseId == Expected.TargetPhaseId &&
         Existing.TargetTaskId == Expected.TargetTaskId &&
         Existing.TargetTaskRevision == Expected.TargetTaskRevision &&
         Existing.ExpectedVariantCount == Expected.ExpectedVariantCount &&
         Existing.ExpectedOccurrenceCount == Expected.ExpectedOccurrenceCount &&
         Existing.CanonicalOccurrence == Expected.CanonicalOccurrence;
}

}

EventStore::EventStore(std::filesystem::path EventPath,
                       std::function<bool()> FlushOperation)
    : EventPath(std::move(EventPath)), FlushOperation(std::move(FlushOperation)) {}

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
  const std::string Line = SerializeEvent(Copy) + "\n";
  const auto* Flush = FlushOperation ? &FlushOperation : nullptr;
  AppendLocked(EventPath, Line, Flush, {}, {},
               [&](const std::vector<std::string>& Existing, bool CompleteTail) {
                 if (!CompleteTail) {
                   throw WorkflowError("Event log has an incomplete tail; append refused");
                 }
                 const ResolvedEventProjection Projection =
                     ProjectEventLog(Existing, false);
                 (void)Projection;
                 const std::vector<EventLogRow> Rows = ParseEventLogRows(Existing);
                 if (HasUnsafeResolutionId(Rows, Copy.EventId)) {
                   throw WorkflowError("Duplicate EventId rejected: " + Copy.EventId);
                 }
                 return true;
               });
}

void EventStore::ResolveEventIdConflict(
    const EventIdConflictAuthorization& Authorization) const {
  if (Authorization.EventShard.empty() || Authorization.TargetEventId.empty() ||
      Authorization.TargetReleaseId.empty() || Authorization.TargetPhaseId.empty() ||
      !IsTaskId(Authorization.TargetTaskId) || Authorization.TargetTaskRevision <= 0 ||
      Authorization.ExpectedVariantCount < 2 ||
      Authorization.ExpectedOccurrenceCount < 2 ||
      Authorization.CanonicalOccurrence == 0 ||
      Authorization.CanonicalOccurrence > Authorization.ExpectedOccurrenceCount ||
      Authorization.ApprovalReason.empty() ||
      Authorization.ChangeRequestId != "CR-ED-0014-001" ||
      Authorization.ApprovedByRole != "Coordinator" ||
      Authorization.ApprovedByAgentId != "/root" ||
      !IsCommitId(Authorization.TaskSpecCommit) ||
      !IsCommitId(Authorization.TaskSpecBlobId) ||
      Authorization.TaskPlanPath != "Workflow/Plans/P1.yaml" ||
      Authorization.ResolutionReleaseId != "Bootstrap" ||
      Authorization.ResolutionPhaseId != "P1" ||
      Authorization.ResolutionTaskId != "ED-0014" ||
      Authorization.ResolutionTaskRevision != 9 ||
      Authorization.ResolutionBranch.empty()) {
    throw WorkflowError("EventId resolution authorization is incomplete or invalid");
  }
  WorkflowEvent Event;
  Event.EventId = "eventid-conflict-resolution-" + Authorization.TargetEventId;
  Event.EventType = "EventIdConflictResolution";
  Event.OccurredAt = UtcNow();
  Event.ReleaseId = Authorization.ResolutionReleaseId;
  Event.PhaseId = Authorization.ResolutionPhaseId;
  Event.TaskId = Authorization.ResolutionTaskId;
  Event.TaskRevision = Authorization.ResolutionTaskRevision;
  Event.AgentId = Authorization.ApprovedByAgentId;
  Event.AgentRole = Authorization.ApprovedByRole;
  Event.Branch = Authorization.ResolutionBranch;
  Event.CorrelationId = "ED-0014-R9-" + Authorization.TargetEventId;
  Event.Result = "ResolvedOccurrenceSelected";
  Event.EvidenceRefs = {
      Authorization.TaskPlanPath + "#TaskId=ED-0014",
      Authorization.TaskSpecCommit,
      Authorization.TaskSpecBlobId,
      Authorization.ChangeRequestId,
      Authorization.TargetEventId};
  Event.TaskSpecPath = Authorization.TaskPlanPath;
  Event.TaskSpecCommit = Authorization.TaskSpecCommit;
  Event.TaskSpecBlobId = Authorization.TaskSpecBlobId;
  Event.ChangeRequestId = Authorization.ChangeRequestId;
  Event.ApprovalReason = Authorization.ApprovalReason;
  Event.TargetEventId = Authorization.TargetEventId;
  Event.TargetReleaseId = Authorization.TargetReleaseId;
  Event.TargetPhaseId = Authorization.TargetPhaseId;
  Event.TargetTaskId = Authorization.TargetTaskId;
  Event.TargetTaskRevision = Authorization.TargetTaskRevision;
  Event.ExpectedVariantCount = Authorization.ExpectedVariantCount;
  Event.ExpectedOccurrenceCount = Authorization.ExpectedOccurrenceCount;
  Event.CanonicalOccurrence = Authorization.CanonicalOccurrence;

  const std::string Line = SerializeEvent(Event) + "\n";
  const auto* Flush = FlushOperation ? &FlushOperation : nullptr;
  AppendLocked(EventPath, Line, Flush, {}, {},
               [&](const std::vector<std::string>& Existing, bool CompleteTail) {
                 if (!CompleteTail) {
                   throw WorkflowError("Event log has a malformed or torn tail; resolution refused");
                 }
                 const ResolvedEventProjection Projection =
                     ProjectEventLog(Existing, true);
                 (void)Projection;
                 const std::vector<EventLogRow> Rows = ParseEventLogRows(Existing);
                 std::vector<const EventLogRow*> TargetRows;
                 std::vector<const EventLogRow*> ExistingResolutions;
                 for (const EventLogRow& Row : Rows) {
                   if (Row.Event.EventId == Authorization.TargetEventId) {
                     TargetRows.push_back(&Row);
                   }
                   if (Row.Event.EventType == "EventIdConflictResolution" &&
                       Row.Event.TargetEventId == Authorization.TargetEventId) {
                     ExistingResolutions.push_back(&Row);
                   }
                 }
                 if (ExistingResolutions.size() > 1) {
                   throw WorkflowError("Multiple resolutions already target this EventId");
                 }
                 if (TargetRows.empty()) {
                   throw WorkflowError("Authorized EventId is missing from the event shard");
                 }
                 std::vector<std::string> DistinctVariants;
                 for (const EventLogRow* Row : TargetRows) {
                   if (!SameTargetScope(TargetRows.front()->Event, Row->Event) ||
                       Row->Event.ReleaseId != Authorization.TargetReleaseId ||
                       Row->Event.PhaseId != Authorization.TargetPhaseId ||
                       Row->Event.TaskId != Authorization.TargetTaskId ||
                       Row->Event.TaskRevision != Authorization.TargetTaskRevision) {
                     throw WorkflowError("EventId variants do not match the approved target scope");
                   }
                   if (std::find(DistinctVariants.begin(), DistinctVariants.end(),
                                 Row->RawRecord) == DistinctVariants.end()) {
                     DistinctVariants.push_back(Row->RawRecord);
                   }
                 }
                 if (DistinctVariants.size() != Authorization.ExpectedVariantCount ||
                     TargetRows.size() != Authorization.ExpectedOccurrenceCount ||
                     Authorization.CanonicalOccurrence > TargetRows.size()) {
                   throw WorkflowError("EventId conflict counts changed after Coordinator approval");
                 }
                 if (!ExistingResolutions.empty()) {
                   if (SameResolutionPayload(ExistingResolutions.front()->Event, Event)) {
                     return false;
                   }
                   throw WorkflowError("Existing EventId resolution conflicts with the pinned approval");
                 }
                 if (HasUnsafeResolutionId(Rows, Event.EventId)) {
                   throw WorkflowError("Resolution EventId is already used by another event");
                 }
                 return true;
               });
}

std::vector<WorkflowEvent> EventStore::ReadTaskEvents(const std::string& TaskId,
                                                      std::size_t MaximumEvents) const {
  if (!IsTaskId(TaskId)) {
    throw WorkflowError("Task-scoped event query requires a valid TaskId");
  }
  const auto* Flush = FlushOperation ? &FlushOperation : nullptr;
  const EventLogSnapshot Snapshot = ReadEventLogSnapshot(EventPath, Flush);
  if (!Snapshot.CompleteTail) {
    throw WorkflowError("Event log has a malformed or torn tail");
  }
  const ResolvedEventProjection Projection =
      ProjectEventLog(Snapshot.Records, false);
  std::vector<WorkflowEvent> Results;
  for (const EventLogRow& Row : Projection.SelectedRows) {
    if (Row.Event.TaskId != TaskId) {
      continue;
    }
    Results.push_back(Row.Event);
  }
  if (Results.size() > MaximumEvents) {
    Results.erase(Results.begin(),
                  Results.begin() + static_cast<std::ptrdiff_t>(Results.size() - MaximumEvents));
  }
  return Results;
}

std::vector<ValidationResult> CollectTaskRevisionValidations(
    const TaskSpec& Task,
    const std::vector<WorkflowEvent>& Events) {
  if (!IsTaskId(Task.TaskId) || Task.TaskRevision <= 0 ||
      !IsCommitId(Task.FixedHeadCommit)) {
    throw WorkflowError("ReviewBundle validation collection requires TaskId, TaskRevision, and fixed HeadCommit");
  }
  std::vector<ValidationResult> Results;
  std::unordered_map<std::string, std::string> PayloadByEventId;
  for (const WorkflowEvent& Event : Events) {
    if (Event.EventType != "CommandExecuted" || Event.TaskId != Task.TaskId ||
        Event.TaskRevision != Task.TaskRevision) {
      continue;
    }
    if (Event.TaskSpecCommit != Task.TaskSpecCommit ||
        Event.TaskSpecBlobId != Task.TaskSpecBlobId ||
        Event.AssignmentEventCommit != Task.AssignmentEventCommit ||
        Event.AssignmentEventId != Task.AssignmentEventId ||
        Event.CodeBaseCommit != Task.CodeBaseCommit ||
        Event.BudgetCorrectionEventCommit != Task.BudgetCorrectionEventCommit ||
        Event.BudgetCorrectionEventId != Task.BudgetCorrectionEventId ||
        Event.HeadCommit != Task.FixedHeadCommit) {
      continue;
    }
    if (Event.EventId.empty() || Event.Command.empty() || Event.ExitCode < 0 ||
        Event.WorkingDirectory.empty() ||
        !std::filesystem::path(Event.WorkingDirectory).is_absolute() ||
        !std::filesystem::is_directory(Event.WorkingDirectory) || Event.EvidencePath.empty() ||
        !std::filesystem::path(Event.EvidencePath).is_absolute() ||
        !std::filesystem::is_regular_file(Event.EvidencePath)) {
      throw WorkflowError("Current-revision validation lacks an EventId or accessible EvidencePath");
    }
    std::ostringstream Payload;
    Payload << Event.Command << '\n' << Event.WorkingDirectory << '\n' << Event.ExitCode << '\n'
            << Event.EvidencePath;
    const auto [Existing, Inserted] = PayloadByEventId.emplace(Event.EventId, Payload.str());
    if (!Inserted) {
      if (Existing->second != Payload.str()) {
        throw WorkflowError("Conflicting validation records reuse EventId: " + Event.EventId);
      }
      continue;
    }
    Results.push_back({Event.Command, Event.WorkingDirectory,
                       static_cast<unsigned long>(Event.ExitCode), Event.EvidencePath,
                       Event.EventId, Event.HeadCommit});
  }
  return Results;
}

std::optional<VerifiedContextBudgetOverride> EventStore::FindContextBudgetOverride(
    const TaskSpec& Task) const {
  if (!Task.RegisteredBudgetOverride || !IsCommitId(Task.TaskSpecCommit) ||
      !IsCommitId(Task.TaskSpecBlobId) || !IsCommitId(Task.AssignmentEventCommit) ||
      Task.AssignmentEventId.empty() || !IsCommitId(Task.BudgetCorrectionEventCommit) ||
      Task.BudgetCorrectionEventId.empty() || Task.CodeBaseCommit != Task.BaselineCommit) {
    return std::nullopt;
  }
  const ContextBudgetOverride& Registered = *Task.RegisteredBudgetOverride;
  if (Task.EventShard.empty() || Task.TaskPlanPath.empty()) {
    return std::nullopt;
  }
  const std::string NormalizedEventShard = NormalizeRelativePath(Task.EventShard);
  if (!LowerAscii(NormalizedEventShard).starts_with("workflow/events/") ||
      NormalizedEventShard.find(':') != std::string::npos) {
    return std::nullopt;
  }
  const std::vector<WorkflowEvent> TaskEvents = ReadTaskEvents(Task.TaskId);
  const auto Assignment = std::find_if(
      TaskEvents.begin(), TaskEvents.end(), [&](const WorkflowEvent& Event) {
        return Event.EventId == Task.AssignmentEventId &&
               Event.EventType == "TaskReworkStarted" &&
               Event.TaskRevision == Task.TaskRevision &&
               Event.TaskSpecCommit == Task.TaskSpecCommit &&
               Event.TaskSpecBlobId == Task.TaskSpecBlobId &&
               Event.TaskSpecPath == Task.TaskPlanPath &&
               Event.CodeBaseCommit == Task.CodeBaseCommit &&
               Event.ContextBudgetApprovalEventId == Registered.ApprovalEventId &&
               Event.OwnerAgentId == Task.OwnerAgentId &&
               Event.ReviewerAgentId == Task.ReviewerAgentId &&
               Event.Branch == Task.Branch && Event.Worktree == Task.Worktree;
      });
  if (Assignment == TaskEvents.end()) {
    return std::nullopt;
  }
  const std::string TaskEvidenceRef =
      Task.TaskPlanPath + "#" + Task.TaskId + ".ContextBudgetOverride";
  const std::string ReadSetEvidenceRef =
      Task.TaskPlanPath + "#" + Task.TaskId + ".ReadSet";
  const auto OriginalApproval = std::find_if(
      TaskEvents.begin(), TaskEvents.end(), [&](const WorkflowEvent& Event) {
        return Event.EventId == Registered.ApprovalEventId &&
               Event.EventType == "ContextBudgetExceptionApproved" &&
               Event.TaskId == Task.TaskId && Event.TaskRevision == Task.TaskRevision &&
               Event.AgentRole == "Coordinator" && Event.Result == "Approved" &&
               !Event.ApprovalReason.empty() && Event.MaxReferenceFiles ==
                   Registered.MaxReferenceFiles && Event.ReadSetPaths == Task.ReadSet &&
               std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(), TaskEvidenceRef) !=
                   Event.EvidenceRefs.end() &&
               std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(), ReadSetEvidenceRef) !=
                   Event.EvidenceRefs.end();
      });
  if (OriginalApproval == TaskEvents.end()) {
    return std::nullopt;
  }
  const auto Correction = std::find_if(
      TaskEvents.begin(), TaskEvents.end(), [&](const WorkflowEvent& Event) {
        return Event.EventId == Task.BudgetCorrectionEventId &&
               Event.EventType == "ContextBudgetExceptionCorrection" &&
               Event.TaskId == Task.TaskId && Event.TaskRevision == Task.TaskRevision &&
               Event.AgentRole == "Coordinator" &&
               Event.CorrectionOfEventId == Registered.ApprovalEventId &&
               Event.CorrectedField == "ApprovalReason" &&
               Event.OriginalApprovalReason == OriginalApproval->ApprovalReason &&
               Event.CorrectedApprovalReason == Registered.Reason &&
               Event.TaskSpecCommit == Task.TaskSpecCommit &&
               Event.TaskSpecBlobId == Task.TaskSpecBlobId &&
               Event.TaskSpecPath == Task.TaskPlanPath &&
               Event.AssignmentEventCommit == Task.AssignmentEventCommit &&
               Event.AssignmentEventId == Task.AssignmentEventId &&
               Event.CodeBaseCommit == Task.CodeBaseCommit &&
               Event.ChangeRequestId == "CR-ED-0008-003" &&
               Event.MaxReferenceFiles == Registered.MaxReferenceFiles &&
               Event.ReadSetPaths == Task.ReadSet &&
               !Event.Result.empty() &&
               std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(), TaskEvidenceRef) !=
                   Event.EvidenceRefs.end() &&
               std::find(Event.EvidenceRefs.begin(), Event.EvidenceRefs.end(),
                         Registered.ApprovalEventId) != Event.EvidenceRefs.end();
      });
  if (Correction == TaskEvents.end() || Registered.TaskId != Task.TaskId ||
      Registered.TaskRevision != Task.TaskRevision ||
      Registered.ApprovedByRole != "Coordinator" || Registered.Reason.empty() ||
      Registered.MaxReferenceFiles < Task.ReadSet.size()) {
    return std::nullopt;
  }
  ContextBudgetOverride Override = Registered;
  Override.ApprovalEventRef = Task.EventShard + "#" + OriginalApproval->EventId;
  return VerifiedContextBudgetOverride(Override, Task.TaskSpecCommit,
                                       Task.TaskSpecBlobId, Task.AssignmentEventCommit,
                                       Task.AssignmentEventId,
                                       Task.BudgetCorrectionEventCommit,
                                       Task.BudgetCorrectionEventId);
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
  if (!IsCommitId(Bundle.BaseCommit) || !IsCommitId(Bundle.HeadCommit) ||
      !IsCommitId(Bundle.TaskSpecCommit) || !IsCommitId(Bundle.TaskSpecBlobId) ||
      !IsCommitId(Bundle.AssignmentEventCommit) || Bundle.AssignmentEventId.empty() ||
      (Task.ReadSet.size() > 12 &&
       (!IsCommitId(Bundle.BudgetCorrectionEventCommit) ||
        Bundle.BudgetCorrectionEventId.empty())) ||
      !IsCommitId(Bundle.CodeBaseCommit) || Bundle.BaseCommit != Bundle.CodeBaseCommit ||
      !IsCommitId(Task.FixedHeadCommit) || Task.FixedHeadCommit != Bundle.HeadCommit ||
      Bundle.TaskSpecPath != Task.TaskPlanPath ||
      Bundle.BaseCommit != Task.BaselineCommit || Bundle.CodeBaseCommit != Task.CodeBaseCommit ||
      Bundle.TaskSpecCommit != Task.TaskSpecCommit ||
      Bundle.TaskSpecBlobId != Task.TaskSpecBlobId ||
      Bundle.AssignmentEventCommit != Task.AssignmentEventCommit ||
      Bundle.AssignmentEventId != Task.AssignmentEventId ||
      Bundle.BudgetCorrectionEventCommit != Task.BudgetCorrectionEventCommit ||
      Bundle.BudgetCorrectionEventId != Task.BudgetCorrectionEventId) {
    throw WorkflowError("ReviewBundle requires matching TaskSpec, assignment, CodeBase, and Head pins");
  }
  if (Bundle.ChangedPaths.empty()) {
    throw WorkflowError("ReviewBundle requires at least one changed path");
  }
  if (!Bundle.ChangeKinds.empty() &&
      Bundle.ChangeKinds.size() != Bundle.ChangedPaths.size()) {
    throw WorkflowError("ReviewBundle change status count does not match changed paths");
  }
  if (Bundle.AuthorAgentId.empty() || Bundle.ReviewerAgentId.empty() ||
      Bundle.AuthorAgentId == Bundle.ReviewerAgentId ||
      Bundle.AuthorAgentId != Task.OwnerAgentId ||
      Bundle.ReviewerAgentId != Task.ReviewerAgentId) {
    throw WorkflowError("ReviewBundle requires an independent Reviewer");
  }
  if (Bundle.RemotePrCreated) {
    throw WorkflowError("This bootstrap controller only emits an explicitly local ReviewBundle");
  }
  const std::vector<WorkflowEvent> TaskEvents = Events.ReadTaskEvents(Task.TaskId);
  const std::vector<WorkflowEvent>& BundleEvents = Bundle.Events;
  const auto AssignmentEvent = std::find_if(
      TaskEvents.begin(), TaskEvents.end(), [&](const WorkflowEvent& Event) {
        return Event.EventId == Task.AssignmentEventId &&
               Event.EventType == "TaskReworkStarted" &&
               Event.TaskRevision == Task.TaskRevision &&
               Event.TaskSpecCommit == Task.TaskSpecCommit &&
               Event.TaskSpecBlobId == Task.TaskSpecBlobId &&
               Event.TaskSpecPath == Task.TaskPlanPath &&
               Event.CodeBaseCommit == Task.CodeBaseCommit &&
               Event.ContextBudgetApprovalEventId ==
                   (Task.RegisteredBudgetOverride
                        ? Task.RegisteredBudgetOverride->ApprovalEventId
                        : std::string{}) &&
               Event.OwnerAgentId == Task.OwnerAgentId &&
               Event.ReviewerAgentId == Task.ReviewerAgentId &&
               Event.Branch == Task.Branch && Event.Worktree == Task.Worktree;
      });
  if (AssignmentEvent == TaskEvents.end()) {
    throw WorkflowError("ReviewBundle is not bound to the pinned Coordinator assignment event");
  }
  if (std::none_of(BundleEvents.begin(), BundleEvents.end(), [&](const WorkflowEvent& Event) {
        return Event.EventId == Task.AssignmentEventId;
      })) {
    throw WorkflowError("ReviewBundle history omitted its pinned Coordinator assignment event");
  }
  if (Task.ReadSet.size() > 12 || !Task.BudgetCorrectionEventId.empty()) {
    const auto BudgetCorrection = std::find_if(
        TaskEvents.begin(), TaskEvents.end(), [&](const WorkflowEvent& Event) {
          return Event.EventId == Task.BudgetCorrectionEventId &&
                 Event.EventType == "ContextBudgetExceptionCorrection" &&
                 Event.TaskId == Task.TaskId && Event.TaskRevision == Task.TaskRevision &&
                 Event.CorrectionOfEventId ==
                     (Task.RegisteredBudgetOverride
                          ? Task.RegisteredBudgetOverride->ApprovalEventId
                          : std::string{});
        });
    if (BudgetCorrection == TaskEvents.end() ||
        std::none_of(BundleEvents.begin(), BundleEvents.end(), [&](const WorkflowEvent& Event) {
          return Event.EventId == Task.BudgetCorrectionEventId;
        })) {
      throw WorkflowError("ReviewBundle history omitted the pinned Coordinator budget correction event");
    }
  }
  if (Task.ReadSet.size() > 12) {
    const auto BudgetOverride = Events.FindContextBudgetOverride(Task);
    if (!BudgetOverride ||
        std::none_of(BundleEvents.begin(), BundleEvents.end(), [&](const WorkflowEvent& Event) {
          return Event.EventId == Task.RegisteredBudgetOverride->ApprovalEventId;
        })) {
      throw WorkflowError("ReviewBundle cannot use a ReadSet without its revision-specific Coordinator approval");
    }
  }
  ValidateWriteSetPaths(Task, Bundle.ChangedPaths);
  if (Bundle.Validations.empty()) {
    throw WorkflowError("ReviewBundle requires validation evidence for the frozen TaskSpec");
  }
  const auto CanonicalWorktree = CanonicalExistingDirectory(Task.Worktree);
  const auto BuildSibling = CanonicalWorktree.parent_path() /
                           (CanonicalWorktree.filename().string() + ".build");
  const auto CanonicalBuildSibling = CanonicalizeFuturePath(BuildSibling);
  const auto CurrentEvents = BundleEvents;
  std::unordered_map<std::string, const WorkflowEvent*> EventById;
  for (const WorkflowEvent& Event : CurrentEvents) {
    if (Event.EventId.empty()) {
      throw WorkflowError("ReviewBundle event history contains an Event without EventId");
    }
    const auto [It, Inserted] = EventById.emplace(Event.EventId, &Event);
    if (!Inserted && It->second->EventType != Event.EventType) {
      throw WorkflowError("ReviewBundle event history contains conflicting EventIds");
    }
    if (Event.TaskRevision == Task.TaskRevision && Event.EventType == "CommandExecuted" &&
        (Event.TaskSpecCommit != Task.TaskSpecCommit ||
         Event.TaskSpecBlobId != Task.TaskSpecBlobId ||
         Event.AssignmentEventCommit != Task.AssignmentEventCommit ||
         Event.AssignmentEventId != Task.AssignmentEventId ||
         Event.CodeBaseCommit != Task.CodeBaseCommit ||
         Event.BudgetCorrectionEventCommit != Task.BudgetCorrectionEventCommit ||
         Event.BudgetCorrectionEventId != Task.BudgetCorrectionEventId ||
         Event.HeadCommit != Bundle.HeadCommit)) {
      throw WorkflowError("ReviewBundle history contains an unbound current-revision validation event");
    }
  }
  std::unordered_map<std::string, const ValidationResult*> ValidationByEventId;
  std::unordered_map<std::string, bool> SuccessfulCommands;
  const std::string TypedGitDiffCommand =
      "git diff --check " + Bundle.CodeBaseCommit + " " + Bundle.HeadCommit;
  for (const std::string& Command : Task.ValidationCommands) {
    SuccessfulCommands.emplace(Command, false);
  }
  SuccessfulCommands.emplace(TypedGitDiffCommand, false);
  for (const ValidationResult& Validation : Bundle.Validations) {
    if (Validation.EventId.empty() || Validation.EvidencePath.empty() ||
        !Validation.EvidencePath.is_absolute()) {
      throw WorkflowError("Every ReviewBundle validation requires a unique EventId and absolute EvidencePath");
    }
    const auto [Previous, Inserted] =
        ValidationByEventId.emplace(Validation.EventId, &Validation);
    if (!Inserted) {
      if (Previous->second->Command != Validation.Command ||
          Previous->second->EvidencePath != Validation.EvidencePath ||
          Previous->second->ExitCode != Validation.ExitCode) {
        throw WorkflowError("Conflicting validations reuse EventId: " + Validation.EventId);
      }
      throw WorkflowError("Duplicate validation EventId must be deduplicated before bundling: " +
                          Validation.EventId);
    }
    const auto EventIt = EventById.find(Validation.EventId);
    if (EventIt == EventById.end()) {
      throw WorkflowError("ReviewBundle validation is not bound to a recorded EventId");
    }
    const WorkflowEvent& Event = *EventIt->second;
    if (Event.EventType != "CommandExecuted" || Event.TaskId != Task.TaskId ||
        Event.TaskRevision != Task.TaskRevision ||
        Event.TaskSpecCommit != Task.TaskSpecCommit ||
        Event.TaskSpecBlobId != Task.TaskSpecBlobId ||
        Event.AssignmentEventCommit != Task.AssignmentEventCommit ||
        Event.AssignmentEventId != Task.AssignmentEventId ||
        Event.CodeBaseCommit != Task.CodeBaseCommit ||
        Event.BudgetCorrectionEventCommit != Task.BudgetCorrectionEventCommit ||
        Event.BudgetCorrectionEventId != Task.BudgetCorrectionEventId ||
        Event.HeadCommit != Bundle.HeadCommit ||
        Event.Command != Validation.Command ||
        Event.WorkingDirectory != Validation.WorkingDirectory.string() ||
        Event.EvidencePath != Validation.EvidencePath.string() ||
        Event.ExitCode != static_cast<long>(Validation.ExitCode) ||
        Validation.HeadCommit != Bundle.HeadCommit) {
      throw WorkflowError("ReviewBundle validation does not match its pinned command event");
    }
    if (!PathsEqual(CanonicalExistingDirectory(Validation.WorkingDirectory),
                    CanonicalWorktree)) {
      throw WorkflowError("ReviewBundle validation working directory does not match the assigned worktree");
    }
    std::error_code EvidenceError;
    if (!std::filesystem::is_regular_file(Validation.EvidencePath, EvidenceError) || EvidenceError) {
      throw WorkflowError("ReviewBundle validation EvidencePath is not an accessible file");
    }
    const auto CanonicalEvidencePath = FinalPathForExistingPath(Validation.EvidencePath);
    if (!IsWithinPath(CanonicalEvidencePath, CanonicalBuildSibling) ||
        IsWithinPath(CanonicalEvidencePath, CanonicalWorktree)) {
      throw WorkflowError("ReviewBundle EvidencePath must resolve under the external worktree build directory");
    }
    const auto Success = SuccessfulCommands.find(Validation.Command);
    if (Success != SuccessfulCommands.end() && Validation.ExitCode == 0) {
      Success->second = true;
    }
  }
  for (const auto& [Command, Succeeded] : SuccessfulCommands) {
    if (!Succeeded) {
      throw WorkflowError("ReviewBundle lacks a successful frozen validation: " + Command);
    }
  }
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
  ReviewBundleData CompleteBundle = Bundle;
  for (std::size_t Index = 0; Index < Bundle.ChangedPaths.size(); ++Index) {
    WorkflowEvent FileEvent;
    FileEvent.EventId = NewRecordId();
    FileEvent.EventType = "FileChangeObserved";
    FileEvent.ReleaseId = Task.ReleaseId;
    FileEvent.PhaseId = Task.PhaseId;
    FileEvent.TaskId = Task.TaskId;
    FileEvent.TaskRevision = Task.TaskRevision;
    FileEvent.TaskSpecPath = Task.TaskPlanPath;
    FileEvent.TaskSpecCommit = Task.TaskSpecCommit;
    FileEvent.TaskSpecBlobId = Task.TaskSpecBlobId;
    FileEvent.AssignmentEventCommit = Task.AssignmentEventCommit;
    FileEvent.AssignmentEventId = Task.AssignmentEventId;
    FileEvent.CodeBaseCommit = Task.CodeBaseCommit;
    FileEvent.HeadCommit = Bundle.HeadCommit;
    FileEvent.BudgetCorrectionEventCommit = Task.BudgetCorrectionEventCommit;
    FileEvent.BudgetCorrectionEventId = Task.BudgetCorrectionEventId;
    FileEvent.AgentId = Bundle.AuthorAgentId;
    FileEvent.Branch = Task.Branch;
    FileEvent.Result = Index < Bundle.ChangeKinds.size()
                           ? Bundle.ChangeKinds[Index]
                           : "Changed";
    FileEvent.EvidenceRefs = {Bundle.ChangedPaths[Index], Bundle.BaseCommit,
                              Bundle.HeadCommit};
    Events.Append(FileEvent);
    CompleteBundle.Events.push_back(std::move(FileEvent));
  }
  WriteExclusive(OutputPath, SerializeReviewBundle(Task, CompleteBundle) + "\n");

  WorkflowEvent BundleEvent;
  BundleEvent.EventType = "ReviewBundleCreated";
  BundleEvent.ReleaseId = Task.ReleaseId;
  BundleEvent.PhaseId = Task.PhaseId;
  BundleEvent.TaskId = Task.TaskId;
  BundleEvent.TaskRevision = Task.TaskRevision;
  BundleEvent.TaskSpecPath = Task.TaskPlanPath;
  BundleEvent.TaskSpecCommit = Task.TaskSpecCommit;
  BundleEvent.TaskSpecBlobId = Task.TaskSpecBlobId;
  BundleEvent.AssignmentEventCommit = Task.AssignmentEventCommit;
  BundleEvent.AssignmentEventId = Task.AssignmentEventId;
  BundleEvent.CodeBaseCommit = Task.CodeBaseCommit;
  BundleEvent.HeadCommit = Bundle.HeadCommit;
  BundleEvent.BudgetCorrectionEventCommit = Task.BudgetCorrectionEventCommit;
  BundleEvent.BudgetCorrectionEventId = Task.BudgetCorrectionEventId;
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
