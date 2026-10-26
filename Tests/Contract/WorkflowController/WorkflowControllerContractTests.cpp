#include "WorkflowController.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace EmulativeDialog::WorkflowController;

void Check(bool Condition, const std::string& Message) {
  if (!Condition) {
    throw std::runtime_error(Message);
  }
}

template <typename Function>
void ExpectWorkflowError(Function&& Action, const std::string& Message) {
  bool Rejected = false;
  try {
    Action();
  } catch (const WorkflowError&) {
    Rejected = true;
  }
  Check(Rejected, Message);
}

std::string UniqueSuffix() {
  return std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64());
}

class OwnedTempDirectory final {
 public:
  OwnedTempDirectory() {
    Path = std::filesystem::temp_directory_path() /
           ("EmulativeDialog-WorkflowController-" + UniqueSuffix());
    std::filesystem::create_directories(Path);
    Marker = Path / "OwnedByWorkflowControllerContractTests.txt";
    std::ofstream Output(Marker, std::ios::binary);
    Output << "WorkflowControllerContractTests:" << Path.filename().string();
    Output.close();
    Check(static_cast<bool>(Output), "Unable to create temporary ownership marker");
  }

  ~OwnedTempDirectory() {
    std::ifstream Input(Marker, std::ios::binary);
    std::string Value((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
    const std::string Expected =
        "WorkflowControllerContractTests:" + Path.filename().string();
    if (Value == Expected && Path.parent_path() == std::filesystem::temp_directory_path()) {
      std::error_code Error;
      std::filesystem::remove_all(Path, Error);
    }
  }

  std::filesystem::path Path;
  std::filesystem::path Marker;
};

void WriteText(const std::filesystem::path& Path, const std::string& Value) {
  std::filesystem::create_directories(Path.parent_path());
  std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
  Output << Value;
  Check(static_cast<bool>(Output), "Unable to write fixture: " + Path.string());
}

std::string ReadText(const std::filesystem::path& Path) {
  std::ifstream Input(Path, std::ios::binary);
  Check(static_cast<bool>(Input), "Unable to read fixture: " + Path.string());
  return std::string((std::istreambuf_iterator<char>(Input)),
                     std::istreambuf_iterator<char>());
}

std::string TrimText(std::string Value) {
  while (!Value.empty() && (Value.back() == '\r' || Value.back() == '\n' ||
                            Value.back() == ' ' || Value.back() == '\t')) {
    Value.pop_back();
  }
  return Value;
}

struct RepositoryFixture {
  explicit RepositoryFixture(const std::filesystem::path& Root) : Root(Root) {
    WriteText(Root / "Workflow/TaskIndex.yaml",
              "SchemaVersion: 2\n"
              "PhasePlans:\n"
              "  - PhaseId: P1\n"
              "    Path: Workflow/Plans/P1.yaml\n"
              "  - PhaseId: P2\n"
              "    Path: \"\"\n");
    WriteText(Root / "Workflow/TaskTemplate.yaml",
              "TaskTemplateFixture: true\nAuthorization: Bearer secret-value\n");
    WriteText(Root / "Workflow/Plans/P1.yaml",
              "SchemaVersion: 1\n"
              "PhaseId: P1\n"
              "Tasks:\n"
              "  - TaskId: ED-0001\n"
              "    TaskRevision: 1\n"
              "    State: Merged\n"
              "    DependsOn: []\n"
              "  - TaskId: ED-0002\n"
              "    TaskRevision: 2\n"
              "    Title: Controller fixture\n"
              "    Goal: Check a bounded controller path\n"
              "    State: Ready\n"
              "    OwnerAgentId: \"\"\n"
              "    Branch: \"\"\n"
              "    Worktree: \"\"\n"
              "    BaselineCommit: \"\"\n"
              "    DependsOn: [ED-0001]\n"
              "    ReadSet: [Workflow/TaskTemplate.yaml]\n"
              "    WriteSet: [Tools/WorkflowController/, CMakeLists.txt]\n"
              "    ForbiddenSet: [Source/**, Architecture/Contracts/**]\n"
              "    SharedLocks: [RootCMakeAndPresets]\n"
              "    ContextBudgetOverride:\n"
              "      TaskId: ED-0002\n"
              "      TaskRevision: 2\n"
              "      MaxReferenceFiles: 14\n"
              "      ApprovedByRole: Coordinator\n"
              "      ApprovalEventId: approval-event-1\n"
              "      Reason: Coordinator approves a test budget\n"
              "    Validation:\n"
              "      TargetedBuild: [cmake --preset windows-mingw, \"cmake --build --preset windows-mingw --target BuildProbe WorkflowController WorkflowControllerContractTests\"]\n"
              "      ContractFixtures: [ctest --preset windows-mingw --output-on-failure]\n"
              "      StaticChecks: [\"git diff --check <CodeBaseCommit> <HeadCommit>\"]\n"
              "    Deliverable:\n"
              "      EventShard: Workflow/Events/Bootstrap/P1.jsonl\n");
    WriteText(Root / "Secrets/Private.txt", "OPENAI_API_KEY=sk-private-fixture\n");
  }

  std::filesystem::path Root;
};

class FakeProcessRunner final : public ProcessRunner {
 public:
  ProcessResult Run(const std::vector<std::string>& Arguments,
                    const std::filesystem::path& WorkingDirectory) override {
    LastArguments = Arguments;
    LastWorkingDirectory = WorkingDirectory;
    if (Arguments.size() >= 5 && Arguments[0] == "git" && Arguments[1] == "-C" &&
        Arguments[3] == "rev-parse" && Arguments[4] == "HEAD") {
      return {GitExitCode, GitHeadCommit + "\n"};
    }
    if (Arguments.size() >= 5 && Arguments[0] == "git" && Arguments[1] == "-C" &&
        Arguments[3] == "status") {
      return {GitExitCode, GitStatus};
    }
    ++Calls;
    return {ExitCode, "Build complete\nAuthorization: Bearer secret-value\nAPI_TOKEN=fixture-secret\n"};
  }

  unsigned long ExitCode = 0;
  unsigned long GitExitCode = 0;
  int Calls = 0;
  std::string GitHeadCommit;
  std::string GitStatus;
  std::vector<std::string> LastArguments;
  std::filesystem::path LastWorkingDirectory;
};

class PinnedSnapshotProcessRunner final : public ProcessRunner {
 public:
  PinnedSnapshotProcessRunner(std::filesystem::path RepositoryRoot,
                               std::string TaskSpecCommit,
                               std::string TaskSpecBlobId,
                               std::string AssignmentEventCommit,
                               std::string TaskIndexText,
                               std::string TaskPlanText,
                               std::string AssignmentLog)
      : RepositoryRoot(std::move(RepositoryRoot)),
        TaskSpecCommit(std::move(TaskSpecCommit)),
        TaskSpecBlobId(std::move(TaskSpecBlobId)),
        AssignmentEventCommit(std::move(AssignmentEventCommit)),
        TaskIndexText(std::move(TaskIndexText)),
        TaskPlanText(std::move(TaskPlanText)),
        AssignmentLog(std::move(AssignmentLog)) {}

  ProcessResult Run(const std::vector<std::string>& Arguments,
                    const std::filesystem::path&) override {
    ++Calls;
    Check(Arguments.size() >= 4 && Arguments[0] == "git" && Arguments[1] == "-C" &&
              std::filesystem::path(Arguments[2]) == RepositoryRoot,
          "Pinned snapshot loader issued an unexpected process command");
    if (Arguments[3] == "show" && Arguments.size() == 5 &&
        Arguments[4] == TaskSpecCommit + ":Workflow/TaskIndex.yaml") {
      return {0, TaskIndexText};
    }
    if (Arguments[3] == "rev-parse" && Arguments.size() == 5 &&
        Arguments[4] == TaskSpecCommit + ":Workflow/Plans/P1.yaml") {
      return {0, TaskSpecBlobId + "\n"};
    }
    if (Arguments[3] == "show" && Arguments.size() == 5 &&
        Arguments[4] == TaskSpecCommit + ":Workflow/Plans/P1.yaml") {
      return {0, TaskPlanText};
    }
    if (Arguments[3] == "merge-base" && Arguments.size() == 7 &&
        Arguments[4] == "--is-ancestor") {
      return {0, {}};
    }
    if (Arguments[3] == "show" && Arguments.size() == 5 &&
        Arguments[4] == AssignmentEventCommit + ":Workflow/Events/Bootstrap/P1.jsonl") {
      return {0, AssignmentLog};
    }
    throw std::runtime_error("Pinned snapshot loader issued an unregistered Git command");
  }

  int Calls = 0;

 private:
  std::filesystem::path RepositoryRoot;
  std::string TaskSpecCommit;
  std::string TaskSpecBlobId;
  std::string AssignmentEventCommit;
  std::string TaskIndexText;
  std::string TaskPlanText;
  std::string AssignmentLog;
};

class ReadSetSnapshotProcessRunner final : public ProcessRunner {
 public:
  explicit ReadSetSnapshotProcessRunner(std::filesystem::path RepositoryRoot)
      : RepositoryRoot(std::move(RepositoryRoot)) {}

  ProcessResult Run(const std::vector<std::string>& Arguments,
                    const std::filesystem::path&) override {
    Check(Arguments.size() >= 5 && Arguments[0] == "git" && Arguments[1] == "-C" &&
              std::filesystem::path(Arguments[2]) == RepositoryRoot,
          "Pinned ReadSet fixture received an unexpected Git command");
    if (Arguments[3] == "ls-tree" && Arguments.size() == 7 && Arguments[5] == "--") {
      return {0, "100644 blob " + std::string(40, 'a') + "\t" + Arguments[6] + "\n"};
    }
    if (Arguments[3] == "show" && Arguments.size() == 5) {
      return {0, "approved context fixture\n"};
    }
    throw std::runtime_error("Pinned ReadSet fixture received an unregistered Git command");
  }

 private:
  std::filesystem::path RepositoryRoot;
};

void TestTaskRoutingAndAssignment(const std::filesystem::path& Root) {
  const RepositoryFixture Fixture(Root);
  const TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Check(Task.TaskId == "ED-0002" && Task.TaskRevision == 2,
        "Task routing did not return the requested revision");
  Check(Task.ValidationCommands.size() == 3,
        "Validation commands were not read from the selected TaskSpec");
  Check(LoadTaskState(Fixture.Root, "Workflow/TaskIndex.yaml", "ED-0001") == "Merged",
        "Dependency status lookup did not route through TaskIndex");

  AssignmentRequest Request{"ED-0002", 2, "/root/test-agent",
                            "Task/ED-0002-ControllerFixture",
                            std::filesystem::absolute(Root / "AssignedWorktree"),
                            std::string(40, 'a')};
  ValidateAssignment(Task, Request, {{"ED-0001", "Merged"}});
  ExpectWorkflowError(
      [&] { ValidateAssignment(Task, AssignmentRequest{
                                         "ED-0002", 1, Request.OwnerAgentId, Request.Branch,
                                         Request.Worktree, Request.BaseCommit},
                                 {{"ED-0001", "Merged"}}); },
      "Stale TaskRevision was accepted");
  ExpectWorkflowError(
      [&] { ValidateAssignment(Task, Request, {{"ED-0001", "Proposed"}}); },
      "Unsatisfied dependency was accepted");
  TaskSpec InvalidState = Task;
  InvalidState.State = "Proposed";
  ExpectWorkflowError(
      [&] { ValidateAssignment(InvalidState, Request, {{"ED-0001", "Merged"}}); },
      "Invalid task state was accepted for assignment");
  ExpectWorkflowError(
      [&] { (void)LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0099"); },
      "Missing TaskId was accepted");
  Check(IsAllowedStateTransition("Ready", "Assigned"),
        "Ready -> Assigned transition should be valid");
  Check(!IsAllowedStateTransition("Active", "Verified"),
        "Active -> Verified must be rejected");
}

void TestPinnedTaskSpecAssignment(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  const RepositoryFixture Fixture(Temp.Path / "Repository");
  const std::string TaskSpecCommit(40, 'a');
  const std::string TaskSpecBlobId(40, 'b');
  const std::string AssignmentEventCommit(40, 'c');
  const std::string CodeBaseCommit(40, 'd');
  const auto Worktree = Temp.Path / "AssignedWorktree";
  std::ostringstream TaskIndex;
  TaskIndex << "CurrentReleaseId: Bootstrap\n"
            << "PhasePlans:\n"
            << "  - PhaseId: P1\n"
            << "    Path: Workflow/Plans/P1.yaml\n";
  std::ostringstream TaskPlan;
  TaskPlan << "SchemaVersion: 1\n"
           << "PhaseId: P1\n"
           << "Tasks:\n"
           << "  - TaskId: ED-0002\n"
           << "    TaskRevision: 4\n"
           << "    Title: pinned task fixture\n"
           << "    Goal: check an immutable task snapshot\n"
           << "    State: Rework\n"
           << "    OwnerAgentId: /root/test-agent\n"
           << "    ReviewerAgentId: /root/independent-reviewer\n"
           << "    Branch: Task/ED-0002-ControllerFixture\n"
           << "    Worktree: " << Worktree.generic_string() << "\n"
           << "    BaselineCommit: " << CodeBaseCommit << "\n"
           << "    ReadSet: [Workflow/TaskTemplate.yaml]\n"
           << "    WriteSet: [Tools/WorkflowController/]\n"
           << "    ForbiddenSet: [Source/**, Architecture/Contracts/**]\n"
           << "    SharedLocks: []\n"
           << "    ContextBudgetOverride:\n"
           << "      TaskId: ED-0002\n"
           << "      TaskRevision: 4\n"
           << "      MaxReferenceFiles: 1\n"
           << "      ApprovedByRole: Coordinator\n"
           << "      ApprovalEventId: approval-r4\n"
           << "      Reason: test a pinned budget event\n"
           << "    Validation:\n"
           << "      TargetedBuild: [cmake --preset windows-mingw]\n"
           << "      ContractFixtures: [ctest --preset windows-mingw --output-on-failure]\n"
           << "      StaticChecks: [\"git diff --check <CodeBaseCommit> <HeadCommit>\"]\n"
           << "    Deliverable:\n"
           << "      EventShard: Workflow/Events/Bootstrap/P1.jsonl\n";
  std::ostringstream AssignmentEvent;
  AssignmentEvent << "{\"EventId\":\"assignment-r4\","
                  << "\"EventType\":\"TaskReworkStarted\","
                  << "\"ReleaseId\":\"Bootstrap\",\"PhaseId\":\"P1\","
                  << "\"TaskId\":\"ED-0002\",\"TaskRevision\":4,"
                  << "\"TaskSpecCommit\":\"" << TaskSpecCommit << "\","
                  << "\"TaskSpecBlobId\":\"" << TaskSpecBlobId << "\","
                  << "\"TaskSpecPath\":\"Workflow/Plans/P1.yaml\","
                  << "\"CodeBaseCommit\":\"" << CodeBaseCommit << "\","
                  << "\"BaseCommit\":\"" << CodeBaseCommit << "\","
                  << "\"ContextBudgetApprovalEventId\":\"approval-r4\","
                  << "\"OwnerAgentId\":\"/root/test-agent\","
                  << "\"ReviewerAgentId\":\"/root/independent-reviewer\","
                  << "\"Branch\":\"Task/ED-0002-ControllerFixture\","
                  << "\"Worktree\":\"" << Worktree.generic_string() << "\"}\n";
  PinnedSnapshotProcessRunner Runner(Fixture.Root, TaskSpecCommit, TaskSpecBlobId,
                                     AssignmentEventCommit, TaskIndex.str(),
                                     TaskPlan.str(), AssignmentEvent.str());
  const TaskSpec Task = LoadTaskSpecFromPinnedSnapshot(
      Fixture.Root, "P1", "ED-0002", TaskSpecCommit, TaskSpecBlobId,
      AssignmentEventCommit, "assignment-r4", Runner);
  Check(Task.TaskRevision == 4 && Task.State == "Rework" &&
            Task.TaskSpecCommit == TaskSpecCommit &&
            Task.TaskSpecBlobId == TaskSpecBlobId &&
            Task.AssignmentEventCommit == AssignmentEventCommit &&
            Task.CodeBaseCommit == CodeBaseCommit &&
            Task.ReviewerAgentId == "/root/independent-reviewer" &&
            Task.ValidationCommands.size() == 2,
        "Pinned TaskSpec/assignment fields did not load from their immutable commits");

  PinnedSnapshotProcessRunner WrongBlobRunner(Fixture.Root, TaskSpecCommit, TaskSpecBlobId,
                                             AssignmentEventCommit, TaskIndex.str(),
                                             TaskPlan.str(), AssignmentEvent.str());
  ExpectWorkflowError(
      [&] { (void)LoadTaskSpecFromPinnedSnapshot(
                Fixture.Root, "P1", "ED-0002", TaskSpecCommit, std::string(40, 'e'),
                AssignmentEventCommit, "assignment-r4", WrongBlobRunner); },
      "Pinned TaskSpec loader accepted a mismatched blob ID");
  ExpectWorkflowError(
      [&] { (void)LoadTaskSpecFromPinnedSnapshot(
                Fixture.Root, "P1", "ED-0002", TaskSpecCommit, TaskSpecBlobId,
                AssignmentEventCommit, "wrong-assignment-id", Runner); },
      "Pinned TaskSpec loader accepted an unbound assignment event ID");
}

void TestReadSetAndWriteSetBoundaries(const std::filesystem::path& Root) {
  const RepositoryFixture Fixture(Root);
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  const auto Context = AssembleReadSetContext(Fixture.Root, Task);
  Check(Context.size() == 1 && Context.front().RelativePath == "Workflow/TaskTemplate.yaml",
        "Context bundle did not contain exactly the frozen ReadSet");
  Check(Context.front().Content.find("TaskTemplateFixture") != std::string::npos,
        "Authorized ReadSet content was not assembled");
  Check(Context.front().Content.find("secret-value") == std::string::npos,
        "ReadSet context leaked the suffix of an Authorization Bearer value");
  EventStore ExpansionEvents(Fixture.Root / Task.EventShard);
  ExpectWorkflowError(
      [&] { RequestAdditionalRead(Task, "Secrets/Private.txt", "Need a scoped fixture",
                                  ExpansionEvents, "/root/test-agent", "read-expansion"); },
      "Path expansion outside ReadSet was accepted");
  const auto ExpansionRecords = ExpansionEvents.ReadTaskEvents(Task.TaskId);
  Check(ExpansionRecords.size() == 1 &&
            ExpansionRecords.front().EventType == "ContextExpanded" &&
            ExpansionRecords.front().Result.find("RejectedOutsideFrozenReadSet") !=
                std::string::npos,
        "Rejected ReadSet expansion was not recorded with its reason");

  ValidateWriteSetPaths(Task, {"Tools/WorkflowController/Example.cpp", "CMakeLists.txt"});
  ExpectWorkflowError(
      [&] { ValidateWriteSetPaths(Task, {"Architecture/Contracts/CoreAgentContract.md"}); },
      "Write outside WriteSet was accepted");
  ExpectWorkflowError(
      [&] { ValidateWriteSetPaths(Task, {"CMakePresets.json"}); },
      "Root shared path without its WriteSet entry was accepted");

  TaskSpec Other = Task;
  Other.TaskId = "ED-0003";
  Other.WriteSet = {"Tools/WorkflowController/"};
  Check(!FindWriteSetConflicts(Task, Other).empty(),
        "Overlapping WriteSets were not detected");
  Other.WriteSet = {"Source/Core/"};
  Check(!FindWriteSetConflicts(Task, Other).empty(),
        "Overlapping SharedLocks were not detected");

  TaskSpec Large = Task;
  for (int Index = 0; Index < 13; ++Index) {
    const std::string Relative = "Context/File" + std::to_string(Index) + ".txt";
    WriteText(Fixture.Root / Relative, "bounded fixture\n");
    Large.ReadSet.push_back(Relative);
  }
  ExpectWorkflowError(
      [&] { (void)AssembleReadSetContext(Fixture.Root, Large); },
      "ReadSet over the default 12-file budget was accepted without an exception");
  static_assert(!std::is_default_constructible_v<VerifiedContextBudgetOverride>);
  static_assert(!std::is_constructible_v<VerifiedContextBudgetOverride,
                                         ContextBudgetOverride>);
  ExpectWorkflowError(
      [&] { ValidateReadSetPath(Large, "Secrets/Private.txt"); },
      "Budget exception expanded the approved ReadSet paths");

  const auto LinkPath = Fixture.Root / "Workflow/Reads/Allowed.yaml";
  const auto SecretPath = Fixture.Root / "Secrets/Private.txt";
  std::filesystem::create_directories(LinkPath.parent_path());
  TaskSpec SymlinkTask = Task;
  SymlinkTask.ReadSet = {"Workflow/Reads/Allowed.yaml"};
  const DWORD FileSymlinkFlags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (!CreateSymbolicLinkW(LinkPath.c_str(), SecretPath.c_str(), FileSymlinkFlags)) {
    throw std::runtime_error("Windows test environment cannot create the in-repository ReadSet symlink; error " +
                             std::to_string(GetLastError()));
  }
  ExpectWorkflowError(
      [&] { (void)AssembleReadSetContext(Fixture.Root, SymlinkTask); },
      "ReadSet symlink to an in-repository but unauthorized target was accepted");
}

void TestTaskOwnedPathResolution(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  const RepositoryFixture Fixture(Temp.Path / "Repository");
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Task.EventShard = "../../Outside/events.jsonl";
  ExpectWorkflowError(
      [&] { (void)ResolveTaskEventPath(Fixture.Root, Task); },
      "EventShard parent traversal was accepted");
  Task.EventShard = "C:/Temp/events.jsonl";
  ExpectWorkflowError(
      [&] { (void)ResolveTaskEventPath(Fixture.Root, Task); },
      "Absolute EventShard path was accepted");

  const auto EventRoot = Fixture.Root / "Workflow/Events";
  const auto OutsideEvents = Fixture.Root.parent_path() / "OutsideEvents";
  std::filesystem::create_directories(EventRoot);
  std::filesystem::create_directories(OutsideEvents);
  const auto EventEscape = EventRoot / "Bootstrap";
  const DWORD DirectorySymlinkFlags = SYMBOLIC_LINK_FLAG_DIRECTORY |
                                       SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (!CreateSymbolicLinkW(EventEscape.c_str(), OutsideEvents.c_str(), DirectorySymlinkFlags)) {
    throw std::runtime_error("Windows test environment cannot create the EventShard reparse fixture; error " +
                             std::to_string(GetLastError()));
  }
  Task.EventShard = "Workflow/Events/Bootstrap/P1.jsonl";
  ExpectWorkflowError(
      [&] { (void)ResolveTaskEventPath(Fixture.Root, Task); },
      "EventShard reparse escape was accepted");

  Task.ReleaseId = "..";
  ExpectWorkflowError(
      [&] { (void)ResolveTaskCheckpointPath(Fixture.Root, Task); },
      "Checkpoint ReleaseId traversal was accepted");
  Task.ReleaseId = "Bootstrap";
  Task.PhaseId = "C:/Temp";
  ExpectWorkflowError(
      [&] { (void)ResolveTaskCheckpointPath(Fixture.Root, Task); },
      "Checkpoint PhaseId path injection was accepted");
  Task.PhaseId = "P1";

  const auto CheckpointRoot = Fixture.Root / "Workflow/Checkpoints";
  const auto OutsideCheckpoints = Fixture.Root.parent_path() / "OutsideCheckpoints";
  std::filesystem::create_directories(CheckpointRoot);
  std::filesystem::create_directories(OutsideCheckpoints);
  const auto CheckpointEscape = CheckpointRoot / "Bootstrap";
  if (!CreateSymbolicLinkW(CheckpointEscape.c_str(), OutsideCheckpoints.c_str(),
                           DirectorySymlinkFlags)) {
    throw std::runtime_error("Windows test environment cannot create the checkpoint reparse fixture; error " +
                             std::to_string(GetLastError()));
  }
  ExpectWorkflowError(
      [&] { (void)ResolveTaskCheckpointPath(Fixture.Root, Task); },
      "Checkpoint reparse escape was accepted");
}

void TestReadSetYamlFragmentSelectors(const std::filesystem::path& Root) {
  const RepositoryFixture Fixture(Root);
  const std::string CapabilityMap =
      "Capabilities:\n"
      "  - CapabilityId: ED-0101\n"
      "    Name: CanonicalStreamedAgentEvents\n"
      "    Decision: Keep\n"
      "  - CapabilityId: ED-0115\n"
      "    Name: WeChatDeferredAdapter\n"
      "    Decision: Defer\n";
  const std::string ModuleMap =
      "Modules:\n"
      "  - ModuleId: AgentRuntime\n"
      "    Owner: AgentRuntimeOwner\n"
      "    Path: Source/Agent/AgentRuntime/\n"
      "  - ModuleId: WeChatChannel\n"
      "    Owner: ChannelOwner\n"
      "    Path: Source/Channels/WeChat/\n";
  WriteText(Fixture.Root / "Workflow/CodexCapabilityMap/AgentCore.yaml", CapabilityMap);
  WriteText(Fixture.Root / "Architecture/ModuleMap.yaml", ModuleMap);
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Task.ReadSet = {
      "Workflow/CodexCapabilityMap/AgentCore.yaml#CapabilityId=ED-0101",
      "Architecture/ModuleMap.yaml#ModuleId=AgentRuntime"};
  const auto Context = AssembleReadSetContext(Fixture.Root, Task);
  Check(Context.size() == 2 &&
            Context[0].RelativePath == Task.ReadSet[0] &&
            Context[1].RelativePath == Task.ReadSet[1],
        "ReadSet selector context did not preserve each exact frozen fragment selector");
  const std::string SelectedText = Context[0].Content + Context[1].Content;
  Check(SelectedText.find("CanonicalStreamedAgentEvents") != std::string::npos &&
            SelectedText.find("AgentRuntimeOwner") != std::string::npos &&
            SelectedText.find("ED-0115") == std::string::npos &&
            SelectedText.find("WeChatDeferredAdapter") == std::string::npos &&
            SelectedText.find("WeChatChannel") == std::string::npos,
        "ReadSet selector exposed another capability or module outside its selected YAML entries");

  Task.ReadSet = {"Workflow/CodexCapabilityMap/AgentCore.yaml#CapabilityId=ED-0999"};
  ExpectWorkflowError(
      [&] { (void)AssembleReadSetContext(Fixture.Root, Task); },
      "Missing YAML fragment selector was silently accepted");
  Task.ReadSet = {"Workflow/CodexCapabilityMap/AgentCore.yaml#CapabilityId=ED-0101"};
  WriteText(Fixture.Root / "Workflow/CodexCapabilityMap/DuplicateCases.yaml",
            "Capabilities:\n"
            "  - CapabilityId: ED-0101\n"
            "    Name: First\n"
            "  - CapabilityId: ED-0101\n"
            "    Name: Duplicate\n");
  Task.ReadSet = {"Workflow/CodexCapabilityMap/DuplicateCases.yaml#CapabilityId=ED-0101"};
  ExpectWorkflowError(
      [&] { (void)AssembleReadSetContext(Fixture.Root, Task); },
      "Duplicate YAML fragment selector was silently accepted");
}

void RunGit(WindowsProcessRunner& Runner,
            const std::filesystem::path& Directory,
            const std::vector<std::string>& Arguments) {
  const ProcessResult Result = Runner.Run(Arguments, Directory);
  Check(Result.ExitCode == 0, "Git fixture setup failed: " + Result.Output);
}

void TestIsolatedWorktree(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  const auto Repository = Temp.Path / "Repository";
  const auto Worktree = Temp.Path / "LinkedWorktree";
  std::filesystem::create_directories(Repository);
  WindowsProcessRunner Runner;
  RunGit(Runner, Repository, {"git", "init", "--initial-branch", "main"});
  RunGit(Runner, Repository, {"git", "config", "user.name", "Controller Fixture"});
  RunGit(Runner, Repository, {"git", "config", "user.email", "controller-fixture@example.invalid"});
  WriteText(Repository / "README.md", "isolated worktree fixture\n");
  RunGit(Runner, Repository, {"git", "add", "README.md"});
  RunGit(Runner, Repository, {"git", "commit", "-m", "fixture baseline"});
  const std::string BaseCommit = TrimText(Runner.Run(
      {"git", "-C", Repository.string(), "rev-parse", "HEAD"}, Repository).Output);
  const std::string OriginalBranch = TrimText(Runner.Run(
      {"git", "-C", Repository.string(), "branch", "--show-current"}, Repository).Output);
  const std::string OriginalStatus = Runner.Run(
      {"git", "-C", Repository.string(), "status", "--porcelain", "--untracked-files=all"},
      Repository).Output;

  AssignmentRequest Assignment{"ED-0002", 2, "/root/test-agent",
                               "Task/ED-0002-ControllerFixture", Worktree, BaseCommit};
  const WorktreeResult Created = CreateOrVerifyWorktree({Repository, Assignment}, Runner);
  Check(Created.Created && Created.HeadCommit == BaseCommit && Created.Branch == Assignment.Branch,
        "Assigned worktree was not created at its BaseCommit");
  Check(TrimText(Runner.Run({"git", "-C", Repository.string(), "branch", "--show-current"},
                        Repository).Output) == OriginalBranch,
        "Worktree creation switched the original branch");
  Check(Runner.Run({"git", "-C", Repository.string(), "status", "--porcelain",
                    "--untracked-files=all"}, Repository).Output == OriginalStatus,
        "Worktree creation changed original worktree files");
  const WorktreeResult Verified = CreateOrVerifyWorktree({Repository, Assignment}, Runner);
  Check(!Verified.Created && Verified.HeadCommit == BaseCommit,
        "Existing assigned worktree was not verified idempotently");
  const WorktreeResult ReviewVerified = VerifyExistingWorktreeForReview(
      {Repository, Assignment}, "Review", Runner);
  Check(!ReviewVerified.Created && ReviewVerified.HeadCommit == BaseCommit &&
            std::filesystem::exists(Worktree / "README.md"),
        "Review state did not verify the existing worktree without creating or changing it");
  ExpectWorkflowError(
      [&] { (void)VerifyExistingWorktreeForReview({Repository, Assignment}, "Ready", Runner); },
      "Read-only ReviewBundle verification accepted an unreviewable state");
  AssignmentRequest MissingReview = Assignment;
  MissingReview.Worktree = Temp.Path / "MissingReviewWorktree";
  ExpectWorkflowError(
      [&] { (void)VerifyExistingWorktreeForReview({Repository, MissingReview}, "Review", Runner); },
      "Read-only ReviewBundle verification created a missing worktree");
  Check(!std::filesystem::exists(MissingReview.Worktree),
        "ReviewBundle verification created a worktree as a side effect");
  Check(TrimText(Runner.Run({"git", "-C", Repository.string(), "branch", "--show-current"},
                            Repository).Output) == OriginalBranch &&
            Runner.Run({"git", "-C", Repository.string(), "status", "--porcelain",
                        "--untracked-files=all"}, Repository).Output == OriginalStatus,
        "Failed ReviewBundle verification switched or changed the original worktree");

  Assignment.Worktree = Repository / "nested-worktree";
  ExpectWorkflowError(
      [&] { (void)CreateOrVerifyWorktree({Repository, Assignment}, Runner); },
      "Worktree inside original worktree was accepted");
}

void TestEventsCheckpointsAndTaskScopedMessages(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  const auto EventPath = Temp.Path / "Workflow/Events/P1.jsonl";
  EventStore Events(EventPath);
  WorkflowEvent Event;
  Event.EventId = "event-1";
  Event.EventType = "CommandExecuted";
  Event.PhaseId = "P1";
  Event.TaskId = "ED-0002";
  Event.TaskRevision = 2;
  Event.AgentId = "/root/test-agent";
  Event.Command = "cmake --preset windows-mingw --token fixture-secret Authorization: Bearer secret-value";
  Event.WorkingDirectory = Temp.Path.string();
  Event.ExitCode = 0;
  Event.EvidencePath = "evidence.log";
  Event.Result = "Succeeded";
  Events.Append(Event);
  WorkflowEvent Other = Event;
  Other.EventId = "event-other-task";
  Other.TaskId = "ED-0003";
  Other.Command = "cmake --preset other-task";
  Events.Append(Other);
  const auto ScopedEvents = Events.ReadTaskEvents("ED-0002");
  Check(ScopedEvents.size() == 1 && ScopedEvents.front().EventId == "event-1",
        "Event query returned unrelated task history");
  Check(ReadText(EventPath).find("fixture-secret") == std::string::npos,
        "Event log did not redact a command secret");
  Check(ReadText(EventPath).find("secret-value") == std::string::npos,
        "Event log leaked the suffix of an Authorization Bearer value");

  Checkpoint CheckpointValue;
  CheckpointValue.TaskId = "ED-0002";
  CheckpointValue.TaskRevision = 2;
  CheckpointValue.Branch = "Task/ED-0002-ControllerFixture";
  CheckpointValue.Worktree = (Temp.Path / "LinkedWorktree").string();
  CheckpointValue.BaseCommit = std::string(40, 'a');
  CheckpointValue.HeadCommit = std::string(40, 'b');
  CheckpointValue.NextAction = "Resume the bounded controller task";
  CheckpointValue.LastValidationResult =
      "api_token=fixture-secret; Authorization: Bearer secret-value was redacted";
  const auto CheckpointPath = Temp.Path / "Workflow/Checkpoints/ED-0002.yaml";
  CheckpointStore::Save(CheckpointPath, CheckpointValue);
  const Checkpoint Recovered = CheckpointStore::Recover(CheckpointPath, "ED-0002", 2);
  Check(Recovered.NextAction == CheckpointValue.NextAction &&
            Recovered.HeadCommit == CheckpointValue.HeadCommit,
        "Checkpoint did not resume after controller recreation");
  Check(ReadText(CheckpointPath).find("fixture-secret") == std::string::npos,
        "Checkpoint did not redact sensitive text");
  Check(ReadText(CheckpointPath).find("secret-value") == std::string::npos,
        "Checkpoint leaked the suffix of an Authorization Bearer value");
  ExpectWorkflowError(
      [&] { (void)CheckpointStore::Recover(CheckpointPath, "ED-0003", 2); },
      "Checkpoint was recovered for a different task");

  TaskScopedMessageStore Messages(Temp.Path / "Workflow/Communications/P1.jsonl");
  Messages.Append({"message-1", "ED-0002", 2, "/root/worker", "/root/coordinator",
                   "Question", "Open", "Budget", "Need a scoped fixture approval", {"test-ref"}});
  Messages.Append({"message-2", "ED-0003", 1, "/root/worker", "/root/coordinator",
                   "Question", "Open", "Other task", "Must not be loaded", {}});
  Messages.Append({"message-3", "ED-0002", 2, "/root/worker", "/root/coordinator",
                   "Acknowledgement", "Closed", "Resolved", "Closed message", {}});
  TaskScopedMessage Resolved = {"message-1", "ED-0002", 2, "/root/worker",
                                "/root/coordinator", "Acknowledgement", "Closed",
                                "Resolved", "Resolution for the open question", {}};
  Messages.Append(Resolved);
  Messages.Append({"message-4", "ED-0002", 2, "/root/coordinator", "/root/worker",
                   "DecisionLink", "Open", "Decision", "Scoped decision link", {}});
  const auto OpenMessages = Messages.ReadOpenForTask("ED-0002", 2);
  Check(OpenMessages.size() == 1 && OpenMessages.front().MessageId == "message-4",
        "Message store did not filter by TaskId and open status");

  const auto DuplicatePath = Temp.Path / "Workflow/Events/DuplicateIds.jsonl";
  EventStore DuplicateStore(DuplicatePath);
  WorkflowEvent UniqueEvent = Event;
  UniqueEvent.EventId = "duplicate-fixture-id";
  DuplicateStore.Append(UniqueEvent);
  ExpectWorkflowError([&] { DuplicateStore.Append(UniqueEvent); },
                      "EventStore appended the same EventId twice");
  const std::string OriginalLine = ReadText(DuplicatePath);
  {
    std::ofstream DuplicateOutput(DuplicatePath, std::ios::binary | std::ios::app);
    DuplicateOutput << OriginalLine;
  }
  Check(DuplicateStore.ReadTaskEvents("ED-0002").size() == 1,
        "Identical duplicate EventId was counted as two events");
  {
    std::ofstream ConflictOutput(DuplicatePath, std::ios::binary | std::ios::app);
    ConflictOutput << "{\"EventId\":\"duplicate-fixture-id\",\"EventType\":\"CommandExecuted\","
                      "\"ReleaseId\":\"Bootstrap\",\"PhaseId\":\"P1\","
                      "\"TaskId\":\"ED-0002\",\"TaskRevision\":2,"
                      "\"Command\":\"different command\"}\n";
  }
  ExpectWorkflowError([&] { (void)DuplicateStore.ReadTaskEvents("ED-0002"); },
                      "Conflicting records with the same EventId were accepted");

  const auto FlushFailurePath = Temp.Path / "Workflow/Events/FlushFailure.jsonl";
  EventStore FlushFailure(FlushFailurePath, [] {
    SetLastError(ERROR_WRITE_FAULT);
    return false;
  });
  WorkflowEvent Uncertain = Event;
  Uncertain.EventId = "flush-failure-id";
  ExpectWorkflowError([&] { FlushFailure.Append(Uncertain); },
                      "FlushFileBuffers failure was reported as a successful append");
  ExpectWorkflowError([&] { FlushFailure.Append(Uncertain); },
                      "Uncertain EventId append was blindly duplicated after flush failure");
}

void TestBudgetOverrideEvent(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  RepositoryFixture Fixture(Temp.Path / "Repository");
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Task.EventShard = "Workflow/Events/Bootstrap/P1.jsonl";
  Task.OwnerAgentId = "/root/test-agent";
  Task.ReviewerAgentId = "/root/independent-reviewer";
  Task.Branch = "Task/ED-0002-ControllerFixture";
  Task.Worktree = (Temp.Path / "AssignedWorktree").generic_string();
  Task.BaselineCommit = std::string(40, 'b');
  Task.CodeBaseCommit = Task.BaselineCommit;
  Task.TaskSpecCommit = std::string(40, 'c');
  Task.TaskSpecBlobId = std::string(40, 'd');
  Task.AssignmentEventCommit = std::string(40, 'e');
  Task.AssignmentEventId = "assignment-event-1";
  for (int Index = 0; Index < 13; ++Index) {
    const std::string Relative = "Context/Approved" + std::to_string(Index) + ".txt";
    WriteText(Fixture.Root / Relative, "approved context fixture\n");
    Task.ReadSet.push_back(Relative);
  }
  const auto EventPath = Fixture.Root / Task.EventShard;
  EventStore Events(EventPath);
  Check(!Events.FindContextBudgetOverride(Task).has_value(),
        "Budget override was accepted without its Coordinator event");
  WorkflowEvent Assignment;
  Assignment.EventId = Task.AssignmentEventId;
  Assignment.EventType = "TaskReworkStarted";
  Assignment.PhaseId = Task.PhaseId;
  Assignment.TaskId = Task.TaskId;
  Assignment.TaskRevision = Task.TaskRevision;
  Assignment.TaskSpecPath = Task.TaskPlanPath;
  Assignment.TaskSpecCommit = Task.TaskSpecCommit;
  Assignment.TaskSpecBlobId = Task.TaskSpecBlobId;
  Assignment.CodeBaseCommit = Task.CodeBaseCommit;
  Assignment.ContextBudgetApprovalEventId = "approval-event-1";
  Assignment.OwnerAgentId = Task.OwnerAgentId;
  Assignment.ReviewerAgentId = Task.ReviewerAgentId;
  Assignment.Branch = Task.Branch;
  Assignment.Worktree = Task.Worktree;
  Events.Append(Assignment);

  WorkflowEvent Approval;
  Approval.EventId = "approval-event-1";
  Approval.EventType = "ContextBudgetExceptionApproved";
  Approval.PhaseId = "P1";
  Approval.TaskId = Task.TaskId;
  Approval.TaskRevision = Task.TaskRevision;
  Approval.AgentId = "/root/coordinator";
  Approval.AgentRole = "Coordinator";
  Approval.Result = "Approved";
  Approval.ApprovalReason = "Coordinator approves a test budget";
  Approval.MaxReferenceFiles = 14;
  Approval.EvidenceRefs = {"Workflow/Plans/P1.yaml#ED-0002.ContextBudgetOverride"};
  Approval.ReadSetPaths = Task.ReadSet;
  Events.Append(Approval);

  const auto VerifiedOverride = Events.FindContextBudgetOverride(Task);
  Check(VerifiedOverride.has_value(),
        "Matching Coordinator budget exception event was not recognized");
  Check(VerifiedOverride->TaskSpecCommit() == Task.TaskSpecCommit &&
            VerifiedOverride->TaskSpecBlobId() == Task.TaskSpecBlobId &&
            VerifiedOverride->AssignmentEventCommit() == Task.AssignmentEventCommit &&
            VerifiedOverride->AssignmentEventId() == Task.AssignmentEventId,
        "Verified budget token was not bound to the pinned TaskSpec and assignment");
  ReadSetSnapshotProcessRunner SnapshotRunner(Fixture.Root);
  const auto Context = AssembleReadSetContext(Fixture.Root, Task, 1024 * 1024,
                                               8 * 1024 * 1024, VerifiedOverride,
                                               &SnapshotRunner);
  Check(Context.size() == 14,
        "Task budget override did not preserve the exact ReadSet boundary");
}

void TestValidationAndReviewBundle(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  const RepositoryFixture Fixture(Temp.Path / "Repository");
  WindowsProcessRunner GitRunner;
  RunGit(GitRunner, Fixture.Root, {"git", "init", "--initial-branch", "main"});
  RunGit(GitRunner, Fixture.Root, {"git", "config", "user.name", "Controller Fixture"});
  RunGit(GitRunner, Fixture.Root,
         {"git", "config", "user.email", "controller-fixture@example.invalid"});
  RunGit(GitRunner, Fixture.Root, {"git", "add", "-A"});
  RunGit(GitRunner, Fixture.Root, {"git", "commit", "-m", "review bundle base fixture"});
  const std::string BaseCommit = TrimText(GitRunner.Run(
      {"git", "-C", Fixture.Root.string(), "rev-parse", "HEAD"}, Fixture.Root).Output);

  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Task.State = "Review";
  Task.EventShard = "Workflow/Events/Bootstrap/P1.jsonl";
  Task.OwnerAgentId = "/root/test-agent";
  Task.ReviewerAgentId = "/root/independent-reviewer";
  Task.Branch = "Task/ED-0002-ReviewBundleFixture";
  const auto Worktree = Temp.Path / "ReviewWorktree";
  Task.Worktree = Worktree.generic_string();
  Task.BaselineCommit = BaseCommit;
  Task.CodeBaseCommit = Task.BaselineCommit;
  Task.TaskSpecCommit = std::string(40, 'c');
  Task.TaskSpecBlobId = std::string(40, 'd');
  Task.AssignmentEventCommit = std::string(40, 'e');
  Task.AssignmentEventId = "validation-assignment-event";
  Task.RegisteredBudgetOverride.reset();
  const WorktreeResult Created = CreateOrVerifyWorktree(
      {Fixture.Root, {Task.TaskId, Task.TaskRevision, Task.OwnerAgentId, Task.Branch,
                      Worktree, Task.BaselineCommit}}, GitRunner);
  Check(Created.Created && Created.BaseCommit == BaseCommit,
        "ReviewBundle fixture did not create its isolated assigned worktree");
  const auto ChangedFile = Worktree / "Tools/WorkflowController/WorkflowController.cpp";
  WriteText(ChangedFile, "// committed review fixture change\n");
  RunGit(GitRunner, Worktree, {"git", "add", "Tools/WorkflowController/WorkflowController.cpp"});
  RunGit(GitRunner, Worktree, {"git", "commit", "-m", "review bundle head fixture"});
  const std::string HeadCommit = TrimText(GitRunner.Run(
      {"git", "-C", Worktree.string(), "rev-parse", "HEAD"}, Worktree).Output);
  Task.FixedHeadCommit = HeadCommit;

  FakeProcessRunner Runner;
  Runner.GitHeadCommit = HeadCommit;
  EventStore Events(Fixture.Root / Task.EventShard);
  WorkflowEvent Assignment;
  Assignment.EventId = Task.AssignmentEventId;
  Assignment.EventType = "TaskReworkStarted";
  Assignment.PhaseId = Task.PhaseId;
  Assignment.TaskId = Task.TaskId;
  Assignment.TaskRevision = Task.TaskRevision;
  Assignment.TaskSpecPath = Task.TaskPlanPath;
  Assignment.TaskSpecCommit = Task.TaskSpecCommit;
  Assignment.TaskSpecBlobId = Task.TaskSpecBlobId;
  Assignment.CodeBaseCommit = Task.CodeBaseCommit;
  Assignment.OwnerAgentId = Task.OwnerAgentId;
  Assignment.ReviewerAgentId = Task.ReviewerAgentId;
  Assignment.Branch = Task.Branch;
  Assignment.Worktree = Task.Worktree;
  Events.Append(Assignment);
  const auto BuildSibling = Worktree.parent_path() /
                            (Worktree.filename().string() + ".build");
  std::vector<ValidationResult> ExecutedValidations;
  for (std::size_t Index = 0; Index < Task.ValidationCommands.size(); ++Index) {
    const auto EvidencePath = BuildSibling / "Validation" /
                              ("validation-" + std::to_string(Index) + ".log");
    ExecutedValidations.push_back(RunFrozenValidation(
        Task, Task.ValidationCommands[Index], Worktree, EvidencePath, Runner, Events,
        "/root/test-agent", "ED-0002-validation-" + std::to_string(Index)));
    const std::string EvidenceText = ReadText(EvidencePath);
    Check(EvidenceText.find("fixture-secret") == std::string::npos &&
              EvidenceText.find("secret-value") == std::string::npos,
          "Validation evidence leaked a token or Authorization Bearer suffix");
    Check(!ExecutedValidations.back().EventId.empty(),
          "Validation command did not return its recorded EventId");
  }
  Check(Runner.Calls == static_cast<int>(Task.ValidationCommands.size()) &&
            Events.ReadTaskEvents("ED-0002").size() == Task.ValidationCommands.size() + 1,
        "Frozen validations were not recorded as unique task-scoped events");
  ExpectWorkflowError(
      [&] { (void)RunFrozenValidation(Task, "cmake --build --preset windows-mingw",
                                      Fixture.Root, Temp.Path / "Evidence/unlisted.log",
                                      Runner, Events, "/root/test-agent", "unlisted"); },
      "Unlisted validation command was executed");
  const int CallsBeforeRejectedShell = Runner.Calls;
  TaskSpec ShellTask = Task;
  ShellTask.ValidationCommands.push_back("cmake --preset windows-mingw && whoami");
  ExpectWorkflowError(
      [&] { (void)RunFrozenValidation(ShellTask, ShellTask.ValidationCommands.back(), Fixture.Root,
                                      Temp.Path / "Evidence/shell.log", Runner, Events,
                                      "/root/test-agent", "shell"); },
      "Shell operator in a frozen validation command was accepted");
  Check(Runner.Calls == CallsBeforeRejectedShell,
        "Rejected validation command reached the process runner");

  const auto GitDiffEvidence = BuildSibling / "Validation/git-diff-check.log";
  const auto GitDiffValidation = RunTypedGitDiffCheck(
      Task, HeadCommit, Worktree, GitDiffEvidence, GitRunner, Events,
      "/root/test-agent", "ED-0002-git-diff-check");
  Check(GitDiffValidation.Command == "git diff --check " + Task.CodeBaseCommit + " " +
                                           HeadCommit &&
            GitDiffValidation.ExitCode == 0 && !GitDiffValidation.EventId.empty(),
        "Typed GitDiffCheck did not use the pinned CodeBase/Head or record its EventId");
  const WorktreeResult ReviewVerified = VerifyExistingWorktreeForReview(
      {Fixture.Root, {Task.TaskId, Task.TaskRevision, Task.OwnerAgentId, Task.Branch,
                      Worktree, Task.CodeBaseCommit}}, Task.State, GitRunner);
  Check(!ReviewVerified.Created && ReviewVerified.HeadCommit == HeadCommit,
        "Review state did not verify the existing fixed Head without creating a worktree");

  ReviewBundleData Bundle;
  Bundle.TaskSpecPath = Task.TaskPlanPath;
  Bundle.TaskSpecCommit = Task.TaskSpecCommit;
  Bundle.TaskSpecBlobId = Task.TaskSpecBlobId;
  Bundle.AssignmentEventCommit = Task.AssignmentEventCommit;
  Bundle.AssignmentEventId = Task.AssignmentEventId;
  Bundle.CodeBaseCommit = Task.CodeBaseCommit;
  Bundle.BaseCommit = Task.CodeBaseCommit;
  Bundle.HeadCommit = HeadCommit;
  Bundle.AuthorAgentId = "/root/test-agent";
  Bundle.ReviewerAgentId = "/root/independent-reviewer";
  Bundle.ChangedPaths = {"Tools/WorkflowController/WorkflowController.cpp"};
  Bundle.ChangeKinds = {"Modified"};
  Bundle.Events = Events.ReadTaskEvents("ED-0002");
  Bundle.Validations = CollectTaskRevisionValidations(Task, Bundle.Events);
  const auto BundleRoot = Fixture.Root.parent_path() / "ReviewBundles";
  const auto BundlePath = BundleRoot / "ED-0002/ReviewBundle.json";
  WriteLocalReviewBundle(Fixture.Root, Task, Bundle, BundlePath, Events);
  const std::string BundleText = ReadText(BundlePath);
  Check(BundleText.find("LocalReviewBundle") != std::string::npos &&
            BundleText.find("remote_pr_created") != std::string::npos &&
            BundleText.find("\"task_spec_path\":\"" + Task.TaskPlanPath + "\"") !=
                std::string::npos &&
            BundleText.find("\"task_spec_commit\":\"" + Task.TaskSpecCommit + "\"") !=
                std::string::npos &&
            BundleText.find("\"task_spec_blob\":\"" + Task.TaskSpecBlobId + "\"") !=
                std::string::npos &&
            BundleText.find("\"task_spec_blob_id\":\"" + Task.TaskSpecBlobId + "\"") !=
                std::string::npos &&
            BundleText.find("\"assignment_event_id\":\"" + Task.AssignmentEventId + "\"") !=
                std::string::npos,
        "Local ReviewBundle does not state its local-only status");
  const auto ReviewEvents = Events.ReadTaskEvents("ED-0002");
  Check(std::any_of(ReviewEvents.begin(), ReviewEvents.end(), [](const WorkflowEvent& Event) {
          return Event.EventType == "FileChangeObserved" && Event.Result == "Modified";
        }) &&
            std::any_of(ReviewEvents.begin(), ReviewEvents.end(), [](const WorkflowEvent& Event) {
              return Event.EventType == "ReviewBundleCreated";
            }),
        "File and ReviewBundle events were not appended");
  ReviewBundleData SameReviewer = Bundle;
  SameReviewer.ReviewerAgentId = SameReviewer.AuthorAgentId;
  ExpectWorkflowError(
      [&] { WriteLocalReviewBundle(Fixture.Root, Task, SameReviewer,
                                   BundleRoot / "ED-0002/SameReviewer.json", Events); },
      "ReviewBundle accepted its author as the independent Reviewer");
  ReviewBundleData BadDiff = Bundle;
  BadDiff.ChangedPaths = {"Source/Agent/AgentRuntime.cpp"};
  ExpectWorkflowError(
      [&] { WriteLocalReviewBundle(Fixture.Root, Task, BadDiff,
                                   BundleRoot / "ED-0002/OutOfScope.json", Events); },
      "ReviewBundle accepted a changed path outside WriteSet");
  ExpectWorkflowError(
      [&] { WriteLocalReviewBundle(Fixture.Root, Task, Bundle,
                                   Fixture.Root / "InsideRepo.json", Events); },
      "ReviewBundle accepted a source-repository output path");
  ExpectWorkflowError(
      [&] { WriteLocalReviewBundle(Fixture.Root, Task, Bundle,
                                   BundleRoot / ".." / "Escaped" / "Traversal.json", Events); },
      "ReviewBundle accepted parent traversal");

  const auto ReparsePath = BundleRoot / "ReparseEscape";
  const auto OutsidePath = Fixture.Root.parent_path() / "OutsideReviewBundles";
  std::filesystem::create_directories(BundleRoot);
  std::filesystem::create_directories(OutsidePath);
  const DWORD SymlinkFlags = SYMBOLIC_LINK_FLAG_DIRECTORY |
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (CreateSymbolicLinkW(ReparsePath.c_str(), OutsidePath.c_str(), SymlinkFlags)) {
    ExpectWorkflowError(
        [&] { WriteLocalReviewBundle(Fixture.Root, Task, Bundle,
                                     ReparsePath / "Reparse.json", Events); },
        "ReviewBundle accepted a reparse-resolved output outside ReviewBundles");
  } else {
    const DWORD Error = GetLastError();
    throw std::runtime_error("Windows test environment cannot create the required reparse fixture; error " +
                             std::to_string(Error));
  }
}

void TestReviewBundleRevisionScopedValidations(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  const RepositoryFixture Fixture(Temp.Path / "Repository");
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Task.TaskRevision = 4;
  Task.ReleaseId = "Bootstrap";
  Task.OwnerAgentId = "/root/test-agent";
  Task.ReviewerAgentId = "/root/independent-reviewer";
  Task.Branch = "Task/ED-0002-ReviewBundleFixture";
  Task.Worktree = Fixture.Root.generic_string();
  Task.BaselineCommit = "10a38af152971b782eb422addaba14f4eabb4ba9";
  Task.CodeBaseCommit = Task.BaselineCommit;
  Task.TaskSpecCommit = std::string(40, '1');
  Task.TaskSpecBlobId = std::string(40, '2');
  Task.AssignmentEventCommit = std::string(40, '3');
  Task.AssignmentEventId = "assignment-r4-fixture";
  Task.RegisteredBudgetOverride.reset();
  Task.EventShard = "Workflow/Events/Bootstrap/P1.jsonl";

  const std::string R2DiffCommand =
      "git diff --check c4d2b94e0dbfb82d922634c1b175aab76f545567 "
      "ddcfac296c1cb50869bd5497bdc0739e8568a56a";
  const std::string R4Head(40, 'f');
  Task.FixedHeadCommit = R4Head;
  const std::string R4DiffCommand =
      "git diff --check " + Task.CodeBaseCommit + " " + R4Head;
  EventStore Events(Fixture.Root / Task.EventShard);

  WorkflowEvent Assignment;
  Assignment.EventId = Task.AssignmentEventId;
  Assignment.EventType = "TaskReworkStarted";
  Assignment.PhaseId = Task.PhaseId;
  Assignment.TaskId = Task.TaskId;
  Assignment.TaskRevision = Task.TaskRevision;
  Assignment.TaskSpecPath = Task.TaskPlanPath;
  Assignment.TaskSpecCommit = Task.TaskSpecCommit;
  Assignment.TaskSpecBlobId = Task.TaskSpecBlobId;
  Assignment.CodeBaseCommit = Task.CodeBaseCommit;
  Assignment.OwnerAgentId = Task.OwnerAgentId;
  Assignment.ReviewerAgentId = Task.ReviewerAgentId;
  Assignment.Branch = Task.Branch;
  Assignment.Worktree = Task.Worktree;
  Events.Append(Assignment);

  WorkflowEvent StaleR2;
  StaleR2.EventType = "CommandExecuted";
  StaleR2.PhaseId = Task.PhaseId;
  StaleR2.TaskId = Task.TaskId;
  StaleR2.TaskRevision = 2;
  StaleR2.Command = R2DiffCommand;
  StaleR2.WorkingDirectory = Fixture.Root.string();
  StaleR2.EvidencePath = (Fixture.Root.parent_path() / "R2-diff-check.log").string();
  Events.Append(StaleR2);

  std::vector<std::string> R4Commands = Task.ValidationCommands;
  R4Commands.push_back(R4DiffCommand);
  for (std::size_t Index = 0; Index < R4Commands.size(); ++Index) {
    WorkflowEvent Current;
    Current.EventType = "CommandExecuted";
    Current.PhaseId = Task.PhaseId;
    Current.TaskId = Task.TaskId;
    Current.TaskRevision = 4;
    Current.Command = R4Commands[Index];
    Current.WorkingDirectory = Fixture.Root.string();
    const auto CurrentEvidence = Fixture.Root.parent_path() /
        (Fixture.Root.filename().string() + ".build/R4-validation-" +
         std::to_string(Index) + ".log");
    WriteText(CurrentEvidence, "synthetic revision 4 evidence\n");
    Current.EvidencePath = CurrentEvidence.string();
    Current.TaskSpecPath = Task.TaskPlanPath;
    Current.TaskSpecCommit = Task.TaskSpecCommit;
    Current.TaskSpecBlobId = Task.TaskSpecBlobId;
    Current.AssignmentEventCommit = Task.AssignmentEventCommit;
    Current.AssignmentEventId = Task.AssignmentEventId;
    Current.CodeBaseCommit = Task.CodeBaseCommit;
    Current.HeadCommit = R4Head;
    Events.Append(Current);
  }
  WorkflowEvent OtherTask;
  OtherTask.EventType = "CommandExecuted";
  OtherTask.PhaseId = Task.PhaseId;
  OtherTask.TaskId = "ED-0099";
  OtherTask.TaskRevision = 4;
  OtherTask.Command = "ctest --preset unrelated";
  Events.Append(OtherTask);

  const auto TaskEvents = Events.ReadTaskEvents(Task.TaskId);
  auto MixedEvents = TaskEvents;
  MixedEvents.push_back(OtherTask);
  const auto Validations = CollectTaskRevisionValidations(Task, MixedEvents);
  Check(Validations.size() == R4Commands.size(),
        "ReviewBundle validation collection did not isolate the current task revision");
  Check(std::none_of(Validations.begin(), Validations.end(), [&](const ValidationResult& Value) {
          return Value.Command == R2DiffCommand;
        }),
        "ReviewBundle validation collection included a stale TaskRevision 2 command");
  Check(std::any_of(Validations.begin(), Validations.end(), [&](const ValidationResult& Value) {
          return Value.Command == R4DiffCommand;
        }),
        "ReviewBundle validation collection omitted the current CodeBase/Head diff-check");

  ReviewBundleData Bundle;
  Bundle.TaskSpecPath = Task.TaskPlanPath;
  Bundle.TaskSpecCommit = Task.TaskSpecCommit;
  Bundle.TaskSpecBlobId = Task.TaskSpecBlobId;
  Bundle.AssignmentEventCommit = Task.AssignmentEventCommit;
  Bundle.AssignmentEventId = Task.AssignmentEventId;
  Bundle.CodeBaseCommit = Task.CodeBaseCommit;
  Bundle.BaseCommit = Task.BaselineCommit;
  Bundle.HeadCommit = R4Head;
  Bundle.AuthorAgentId = Task.OwnerAgentId;
  Bundle.ReviewerAgentId = "/root/independent-reviewer";
  Bundle.ChangedPaths = {"Tools/WorkflowController/Main.cpp"};
  Bundle.ChangeKinds = {"Modified"};
  Bundle.Validations = Validations;
  Bundle.Events = TaskEvents;
  const auto BundleRoot = Fixture.Root.parent_path() / "ReviewBundles";
  const auto BundlePath = BundleRoot / "ED-0002-R4/ReviewBundle.json";
  WriteLocalReviewBundle(Fixture.Root, Task, Bundle, BundlePath, Events);
  const std::string BundleText = ReadText(BundlePath);
  const auto ValidationsStart = BundleText.find("\"validations\":[");
  const auto EventsStart = BundleText.find("],\"events\":[", ValidationsStart);
  Check(ValidationsStart != std::string::npos && EventsStart != std::string::npos,
        "ReviewBundle omitted its validation or event section");
  const std::string ValidationSection =
      BundleText.substr(ValidationsStart, EventsStart - ValidationsStart);
  Check(ValidationSection.find(R2DiffCommand) == std::string::npos,
        "Serialized ReviewBundle validations included a stale TaskRevision 2 command");
  Check(ValidationSection.find(R4DiffCommand) != std::string::npos &&
            BundleText.find("\"task_spec_commit\":\"" + Task.TaskSpecCommit + "\"") !=
                std::string::npos &&
            BundleText.find("\"task_spec_blob\":\"" + Task.TaskSpecBlobId + "\"") !=
                std::string::npos &&
            BundleText.find("\"task_spec_blob_id\":\"" + Task.TaskSpecBlobId + "\"") !=
                std::string::npos &&
            BundleText.find("\"assignment_event_commit\":\"" +
                            Task.AssignmentEventCommit + "\"") != std::string::npos &&
            BundleText.find("\"base_commit\":\"" + Task.BaselineCommit + "\"") !=
                std::string::npos &&
            BundleText.find("\"head_commit\":\"" + R4Head + "\"") !=
                std::string::npos,
        "Serialized ReviewBundle omitted TaskSpec/assignment pins or exact code Base/Head");
  Check(BundleText.find("\"TaskRevision\":2") != std::string::npos &&
            BundleText.find("\"TaskRevision\":4") != std::string::npos,
        "ReviewBundle event history did not preserve historical revisions explicitly");

  ReviewBundleData StaleSameRevisionHistory = Bundle;
  WorkflowEvent UnboundR4Command;
  UnboundR4Command.EventId = "unbound-r4-command";
  UnboundR4Command.EventType = "CommandExecuted";
  UnboundR4Command.PhaseId = Task.PhaseId;
  UnboundR4Command.TaskId = Task.TaskId;
  UnboundR4Command.TaskRevision = Task.TaskRevision;
  UnboundR4Command.TaskSpecCommit = Task.TaskSpecCommit;
  UnboundR4Command.TaskSpecBlobId = Task.TaskSpecBlobId;
  UnboundR4Command.AssignmentEventCommit = Task.AssignmentEventCommit;
  UnboundR4Command.AssignmentEventId = Task.AssignmentEventId;
  UnboundR4Command.CodeBaseCommit = Task.CodeBaseCommit;
  UnboundR4Command.Command = Task.ValidationCommands.front();
  StaleSameRevisionHistory.Events.push_back(UnboundR4Command);
  ExpectWorkflowError(
      [&] { WriteLocalReviewBundle(Fixture.Root, Task, StaleSameRevisionHistory,
                                   BundleRoot / "ED-0002-R4-stale/ReviewBundle.json", Events); },
      "ReviewBundle serialized a same-revision validation event without its fixed HeadCommit pin");

  WorkflowEvent MissingEvidence;
  MissingEvidence.EventType = "CommandExecuted";
  MissingEvidence.PhaseId = Task.PhaseId;
  MissingEvidence.TaskId = Task.TaskId;
  MissingEvidence.TaskRevision = Task.TaskRevision;
  MissingEvidence.TaskSpecCommit = Task.TaskSpecCommit;
  MissingEvidence.TaskSpecBlobId = Task.TaskSpecBlobId;
  MissingEvidence.AssignmentEventCommit = Task.AssignmentEventCommit;
  MissingEvidence.AssignmentEventId = Task.AssignmentEventId;
  MissingEvidence.CodeBaseCommit = Task.CodeBaseCommit;
  MissingEvidence.HeadCommit = Task.FixedHeadCommit;
  MissingEvidence.Command = Task.ValidationCommands.front();
  MissingEvidence.WorkingDirectory = Task.Worktree;
  Events.Append(MissingEvidence);
  const auto CurrentEventsWithMissingEvidence = Events.ReadTaskEvents(Task.TaskId);
  ExpectWorkflowError(
      [&] { (void)CollectTaskRevisionValidations(Task, CurrentEventsWithMissingEvidence); },
      "Current-revision validation with an empty EvidencePath was collected");
  std::vector<WorkflowEvent> CurrentEventsWithMissingFile = Bundle.Events;
  MissingEvidence.EventId = "missing-evidence-file-event";
  MissingEvidence.EvidencePath =
      (Fixture.Root.parent_path() / "MissingEvidence.log").string();
  CurrentEventsWithMissingFile.push_back(MissingEvidence);
  ExpectWorkflowError(
      [&] { (void)CollectTaskRevisionValidations(Task, CurrentEventsWithMissingFile); },
      "Current-revision validation with an inaccessible EvidencePath was collected");
}

void TestForbiddenGovernanceOperations() {
  ExpectWorkflowError([] { RejectChangeApproval(); },
                      "Controller approved a ChangeRequest");
  ExpectWorkflowError([] { RejectMerge(); },
                      "Controller merged a task branch");
  ExpectWorkflowError([] { RejectUnscopedHistoryRead(); },
                      "Controller allowed an unscoped history read");
}

}  // namespace

int main() {
  try {
    OwnedTempDirectory Temp;
    TestTaskRoutingAndAssignment(Temp.Path / "Routing");
    TestPinnedTaskSpecAssignment(Temp.Path / "PinnedTaskSpec");
    TestReadSetAndWriteSetBoundaries(Temp.Path / "Boundaries");
    TestTaskOwnedPathResolution(Temp.Path / "TaskPaths");
    TestReadSetYamlFragmentSelectors(Temp.Path / "YamlSelectors");
    TestIsolatedWorktree(Temp.Path / "Unused");
    TestEventsCheckpointsAndTaskScopedMessages(Temp.Path / "Persistence");
    TestBudgetOverrideEvent(Temp.Path / "Budget");
    TestValidationAndReviewBundle(Temp.Path / "Validation");
    TestReviewBundleRevisionScopedValidations(Temp.Path / "RevisionScopedBundle");
    TestForbiddenGovernanceOperations();
    std::cout << "WorkflowController AC-1..AC-6 contract fixtures passed\n";
    return 0;
  } catch (const std::exception& Error) {
    std::cerr << "WorkflowControllerContractFailure: "
              << RedactSensitiveText(Error.what()) << '\n';
    return 1;
  }
}
