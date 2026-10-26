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
#include <stdexcept>
#include <string>
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
    WriteText(Root / "Workflow/TaskTemplate.yaml", "TaskTemplateFixture: true\n");
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
              "      TargetedBuild: [cmake --preset windows-mingw]\n"
              "      ContractFixtures: [ctest --preset windows-mingw --output-on-failure]\n"
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
    ++Calls;
    LastArguments = Arguments;
    LastWorkingDirectory = WorkingDirectory;
    return {ExitCode, "Build complete\nAPI_TOKEN=fixture-secret\n"};
  }

  unsigned long ExitCode = 0;
  int Calls = 0;
  std::vector<std::string> LastArguments;
  std::filesystem::path LastWorkingDirectory;
};

void TestTaskRoutingAndAssignment(const std::filesystem::path& Root) {
  const RepositoryFixture Fixture(Root);
  const TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Check(Task.TaskId == "ED-0002" && Task.TaskRevision == 2,
        "Task routing did not return the requested revision");
  Check(Task.ValidationCommands.size() == 2,
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

void TestReadSetAndWriteSetBoundaries(const std::filesystem::path& Root) {
  const RepositoryFixture Fixture(Root);
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  const auto Context = AssembleReadSetContext(Fixture.Root, Task);
  Check(Context.size() == 1 && Context.front().RelativePath == "Workflow/TaskTemplate.yaml",
        "Context bundle did not contain exactly the frozen ReadSet");
  Check(Context.front().Content.find("TaskTemplateFixture") != std::string::npos,
        "Authorized ReadSet content was not assembled");
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
  ContextBudgetOverride Override{"ED-0002", 2, 14,
                                 "Approved controller fixture budget", "Coordinator",
                                 "budget-test",
                                 "Workflow/Events/Bootstrap/P1.jsonl#budget-test"};
  const auto ExpandedContext = AssembleReadSetContext(Fixture.Root, Large, 1024 * 1024,
                                                       8 * 1024 * 1024, Override);
  Check(ExpandedContext.size() == Large.ReadSet.size(),
        "Coordinator-authorized budget exception did not cover the ReadSet");
  ExpectWorkflowError(
      [&] { ValidateReadSetPath(Large, "Secrets/Private.txt"); },
      "Budget exception expanded the approved ReadSet paths");
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
  Event.Command = "cmake --preset windows-mingw --token fixture-secret";
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

  Checkpoint CheckpointValue;
  CheckpointValue.TaskId = "ED-0002";
  CheckpointValue.TaskRevision = 2;
  CheckpointValue.Branch = "Task/ED-0002-ControllerFixture";
  CheckpointValue.Worktree = (Temp.Path / "LinkedWorktree").string();
  CheckpointValue.BaseCommit = std::string(40, 'a');
  CheckpointValue.HeadCommit = std::string(40, 'b');
  CheckpointValue.NextAction = "Resume the bounded controller task";
  CheckpointValue.LastValidationResult = "api_token=fixture-secret was redacted";
  const auto CheckpointPath = Temp.Path / "Workflow/Checkpoints/ED-0002.yaml";
  CheckpointStore::Save(CheckpointPath, CheckpointValue);
  const Checkpoint Recovered = CheckpointStore::Recover(CheckpointPath, "ED-0002", 2);
  Check(Recovered.NextAction == CheckpointValue.NextAction &&
            Recovered.HeadCommit == CheckpointValue.HeadCommit,
        "Checkpoint did not resume after controller recreation");
  Check(ReadText(CheckpointPath).find("fixture-secret") == std::string::npos,
        "Checkpoint did not redact sensitive text");
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
}

void TestBudgetOverrideEvent(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  RepositoryFixture Fixture(Temp.Path / "Repository");
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Task.EventShard = "Workflow/Events/Bootstrap/P1.jsonl";
  for (int Index = 0; Index < 13; ++Index) {
    const std::string Relative = "Context/Approved" + std::to_string(Index) + ".txt";
    WriteText(Fixture.Root / Relative, "approved context fixture\n");
    Task.ReadSet.push_back(Relative);
  }
  const auto EventPath = Fixture.Root / Task.EventShard;
  EventStore Events(EventPath);
  Check(!Events.FindContextBudgetOverride(Task).has_value(),
        "Budget override was accepted without its Coordinator event");
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
  Events.Append(Approval);

  const auto VerifiedOverride = Events.FindContextBudgetOverride(Task);
  Check(VerifiedOverride.has_value(),
        "Matching Coordinator budget exception event was not recognized");
  const auto Context = AssembleReadSetContext(Fixture.Root, Task, 1024 * 1024,
                                               8 * 1024 * 1024, VerifiedOverride);
  Check(Context.size() == 14,
        "Task budget override did not preserve the exact ReadSet boundary");
}

void TestValidationAndReviewBundle(const std::filesystem::path& Root) {
  OwnedTempDirectory Temp;
  const RepositoryFixture Fixture(Temp.Path / "Repository");
  TaskSpec Task = LoadTaskSpec(Fixture.Root, "Workflow/TaskIndex.yaml", "P1", "ED-0002");
  Task.EventShard = "Workflow/Events/Bootstrap/P1.jsonl";
  FakeProcessRunner Runner;
  EventStore Events(Fixture.Root / Task.EventShard);
  const auto EvidencePath = Fixture.Root.parent_path() /
      (Fixture.Root.filename().string() + ".build/Validation/validation.log");
  const auto Result = RunFrozenValidation(Task, Task.ValidationCommands.front(),
                                          Fixture.Root, EvidencePath, Runner, Events,
                                          "/root/test-agent", "ED-0002-validation-0");
  Check(Result.ExitCode == 0 && Runner.Calls == 1,
        "Frozen validation command did not execute through the injected process runner");
  Check(ReadText(EvidencePath).find("fixture-secret") == std::string::npos,
        "Validation evidence did not redact output secrets");
  Check(Events.ReadTaskEvents("ED-0002").size() == 1,
        "Validation execution was not recorded as a task-scoped event");
  ExpectWorkflowError(
      [&] { (void)RunFrozenValidation(Task, "cmake --build --preset windows-mingw",
                                      Fixture.Root, Temp.Path / "Evidence/unlisted.log",
                                      Runner, Events, "/root/test-agent", "unlisted"); },
      "Unlisted validation command was executed");
  Task.ValidationCommands.push_back("cmake --preset windows-mingw && whoami");
  ExpectWorkflowError(
      [&] { (void)RunFrozenValidation(Task, Task.ValidationCommands.back(), Fixture.Root,
                                      Temp.Path / "Evidence/shell.log", Runner, Events,
                                      "/root/test-agent", "shell"); },
      "Shell operator in a frozen validation command was accepted");
  Check(Runner.Calls == 1, "Rejected validation command reached the process runner");

  ReviewBundleData Bundle;
  Bundle.BaseCommit = std::string(40, 'a');
  Bundle.HeadCommit = std::string(40, 'b');
  Bundle.AuthorAgentId = "/root/test-agent";
  Bundle.ReviewerAgentId = "/root/independent-reviewer";
  Bundle.ChangedPaths = {"Tools/WorkflowController/WorkflowController.cpp"};
  Bundle.ChangeKinds = {"Modified"};
  Bundle.Validations.push_back(Result);
  Bundle.Events = Events.ReadTaskEvents("ED-0002");
  const auto BundleRoot = Fixture.Root.parent_path() / "ReviewBundles";
  const auto BundlePath = BundleRoot / "ED-0002/ReviewBundle.json";
  WriteLocalReviewBundle(Fixture.Root, Task, Bundle, BundlePath, Events);
  const std::string BundleText = ReadText(BundlePath);
  Check(BundleText.find("LocalReviewBundle") != std::string::npos &&
            BundleText.find("remote_pr_created") != std::string::npos,
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
  Task.TaskRevision = 3;
  Task.ReleaseId = "Bootstrap";
  Task.OwnerAgentId = "/root/test-agent";
  Task.Branch = "Task/ED-0002-ReviewBundleFixture";
  Task.BaselineCommit = "b0f5defc2cb14c3e066cd9f7d9ed71bb80a87042";
  Task.EventShard = "Workflow/Events/Bootstrap/P1.jsonl";

  const std::string R2DiffCommand =
      "git diff --check c4d2b94e0dbfb82d922634c1b175aab76f545567 "
      "ddcfac296c1cb50869bd5497bdc0739e8568a56a";
  const std::string R3Head = "e32bdaf5ce85426504a661cd1ac00cad167014b1";
  const std::string R3DiffCommand =
      "git diff --check " + Task.BaselineCommit + " " + R3Head;
  EventStore Events(Fixture.Root / Task.EventShard);

  WorkflowEvent StaleR2;
  StaleR2.EventType = "CommandExecuted";
  StaleR2.PhaseId = Task.PhaseId;
  StaleR2.TaskId = Task.TaskId;
  StaleR2.TaskRevision = 2;
  StaleR2.Command = R2DiffCommand;
  StaleR2.WorkingDirectory = Fixture.Root.string();
  StaleR2.EvidencePath = (Fixture.Root.parent_path() / "R2-diff-check.log").string();
  Events.Append(StaleR2);

  const std::vector<std::string> R3Commands{
      "cmake --preset windows-mingw",
      "cmake --build --preset windows-mingw --target BuildProbe WorkflowController WorkflowControllerContractTests",
      "ctest --preset windows-mingw --output-on-failure",
      R3DiffCommand};
  for (std::size_t Index = 0; Index < R3Commands.size(); ++Index) {
    WorkflowEvent Current;
    Current.EventType = "CommandExecuted";
    Current.PhaseId = Task.PhaseId;
    Current.TaskId = Task.TaskId;
    Current.TaskRevision = 3;
    Current.Command = R3Commands[Index];
    Current.WorkingDirectory = Fixture.Root.string();
    Current.EvidencePath = (Fixture.Root.parent_path() /
                            ("R3-validation-" + std::to_string(Index) + ".log")).string();
    Events.Append(Current);
  }
  WorkflowEvent OtherTask;
  OtherTask.EventType = "CommandExecuted";
  OtherTask.PhaseId = Task.PhaseId;
  OtherTask.TaskId = "ED-0099";
  OtherTask.TaskRevision = 3;
  OtherTask.Command = "ctest --preset unrelated";
  Events.Append(OtherTask);

  const auto TaskEvents = Events.ReadTaskEvents(Task.TaskId);
  auto MixedEvents = TaskEvents;
  MixedEvents.push_back(OtherTask);
  const auto Validations = CollectTaskRevisionValidations(Task, MixedEvents);
  Check(Validations.size() == R3Commands.size(),
        "ReviewBundle validation collection did not isolate the current task revision");
  Check(std::none_of(Validations.begin(), Validations.end(), [&](const ValidationResult& Value) {
          return Value.Command == R2DiffCommand;
        }),
        "ReviewBundle validation collection included a stale TaskRevision 2 command");
  Check(std::any_of(Validations.begin(), Validations.end(), [&](const ValidationResult& Value) {
          return Value.Command == R3DiffCommand;
        }),
        "ReviewBundle validation collection omitted the current BaseCommit/HeadCommit diff-check");

  ReviewBundleData Bundle;
  Bundle.BaseCommit = Task.BaselineCommit;
  Bundle.HeadCommit = R3Head;
  Bundle.AuthorAgentId = Task.OwnerAgentId;
  Bundle.ReviewerAgentId = "/root/independent-reviewer";
  Bundle.ChangedPaths = {"Tools/WorkflowController/Main.cpp"};
  Bundle.ChangeKinds = {"Modified"};
  Bundle.Validations = Validations;
  Bundle.Events = TaskEvents;
  const auto BundlePath = Fixture.Root.parent_path() / "ReviewBundles" /
                          "ED-0002-R3/ReviewBundle.json";
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
  Check(ValidationSection.find(R3DiffCommand) != std::string::npos &&
            BundleText.find("\"base_commit\":\"" + Task.BaselineCommit + "\"") !=
                std::string::npos &&
            BundleText.find("\"head_commit\":\"" + R3Head + "\"") !=
                std::string::npos,
        "Serialized ReviewBundle omitted exact current BaseCommit/HeadCommit evidence");
  Check(BundleText.find("\"TaskRevision\":2") != std::string::npos &&
            BundleText.find("\"TaskRevision\":3") != std::string::npos,
        "ReviewBundle event history did not preserve historical revisions explicitly");
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
    TestReadSetAndWriteSetBoundaries(Temp.Path / "Boundaries");
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
