#include "WorkflowController.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <chrono>

namespace {

using namespace EmulativeDialog::WorkflowController;

int ParseRevision(const std::string& Value) {
  std::size_t Parsed = 0;
  const int Revision = std::stoi(Value, &Parsed);
  if (Parsed != Value.size() || Revision <= 0) {
    throw WorkflowError("TaskRevision must be a positive integer");
  }
  return Revision;
}

std::size_t ParseIndex(const std::string& Value) {
  std::size_t Parsed = 0;
  const unsigned long long Index = std::stoull(Value, &Parsed);
  if (Parsed != Value.size()) {
    throw WorkflowError("Validation command index must be an integer");
  }
  return static_cast<std::size_t>(Index);
}

TaskSpec LoadTask(const std::filesystem::path& RepositoryRoot,
                  const std::string& PhaseId,
                  const std::string& TaskId) {
  return LoadTaskSpec(RepositoryRoot, "Workflow/TaskIndex.yaml", PhaseId, TaskId);
}

std::map<std::string, std::string> LoadDependencyStates(
    const std::filesystem::path& RepositoryRoot,
    const TaskSpec& Task) {
  std::map<std::string, std::string> States;
  for (const std::string& DependencyId : Task.DependsOn) {
    States.emplace(DependencyId,
                   LoadTaskState(RepositoryRoot, "Workflow/TaskIndex.yaml", DependencyId));
  }
  return States;
}

AssignmentRequest CurrentAssignment(const TaskSpec& Task) {
  return {Task.TaskId, Task.TaskRevision, Task.OwnerAgentId, Task.Branch,
          std::filesystem::path(Task.Worktree), Task.BaselineCommit};
}

std::filesystem::path EventLogPath(const std::filesystem::path& RepositoryRoot,
                                   const TaskSpec& Task) {
  return ResolveTaskEventPath(RepositoryRoot, Task);
}

std::filesystem::path CheckpointPathFor(const std::filesystem::path& RepositoryRoot,
                                        const TaskSpec& Task) {
  return ResolveTaskCheckpointPath(RepositoryRoot, Task);
}

std::filesystem::path ValidationEvidencePathFor(const TaskSpec& Task,
                                                const std::filesystem::path& RepositoryRoot,
                                                std::size_t CommandIndex) {
  const auto Worktree = Task.Worktree.empty()
                            ? RepositoryRoot
                            : std::filesystem::path(Task.Worktree);
  const auto BuildSibling = Worktree.parent_path() /
                            (Worktree.filename().string() + ".build");
  const auto Tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return BuildSibling / "WorkflowControllerEvidence" / Task.TaskId /
         ("r" + std::to_string(Task.TaskRevision)) /
         ("Validation-" + std::to_string(CommandIndex) + "-" +
          std::to_string(Tick) + ".log");
}

std::filesystem::path GitDiffEvidencePathFor(
    const TaskSpec& Task,
    const std::filesystem::path& RepositoryRoot) {
  const auto Worktree = Task.Worktree.empty()
                            ? RepositoryRoot
                            : std::filesystem::path(Task.Worktree);
  const auto BuildSibling = Worktree.parent_path() /
                            (Worktree.filename().string() + ".build");
  const auto Tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return BuildSibling / "WorkflowControllerEvidence" / Task.TaskId /
         ("r" + std::to_string(Task.TaskRevision)) /
         ("GitDiffCheck-" + std::to_string(Tick) + ".log");
}

std::string Trim(std::string Value) {
  while (!Value.empty() &&
         (Value.back() == '\n' || Value.back() == '\r' || Value.back() == ' ' ||
          Value.back() == '\t')) {
    Value.pop_back();
  }
  return Value;
}

void PrintUsage() {
  std::cerr <<
      "WorkflowController commands:\n"
      "  query <repo> <phase> <task-id> <revision>\n"
      "  validate-assignment <repo> <phase> <task-id> <revision> <agent> <branch> <worktree> <base>\n"
      "  context <repo> <phase> <task-id> <revision>\n"
      "  context <repo> <phase> <task-id> <revision> <task-spec-commit> <task-spec-blob> <assignment-event-commit> <assignment-event-id>\n"
      "  create-worktree <repo> <phase> <task-id> <revision>\n"
      "  run-validation <repo> <phase> <task-id> <revision> <command-index>\n"
      "  run-validation <repo> <phase> <task-id> <revision> <task-spec-commit> <task-spec-blob> <assignment-event-commit> <assignment-event-id> <command-index>\n"
      "  checkpoint-save <repo> <phase> <task-id> <revision> <next-action>\n"
      "  checkpoint-recover <repo> <phase> <task-id> <revision>\n"
      "  messages <repo> <phase> <task-id>\n"
      "  message-add <repo> <phase> <task-id> <revision> <sender> <recipient> <type> <subject> <summary>\n"
      "  message-close <repo> <phase> <task-id> <revision> <message-id> <resolver-agent-id>\n"
      "  review-bundle <repo> <phase> <task-id> <revision> <task-spec-commit> <task-spec-blob> <assignment-event-commit> <assignment-event-id> <reviewer-agent-id> <output-path>\n"
      "  approve-change <task-id> <change-request-id>  (always rejected)\n"
      "  merge <task-id>  (always rejected; IntegrationOwner only)\n"
      "  history  (unscoped query always rejected)\n";
}

std::vector<std::string> SplitLines(const std::string& Text) {
  std::vector<std::string> Lines;
  std::size_t Start = 0;
  while (Start < Text.size()) {
    const std::size_t End = Text.find('\n', Start);
    std::string Line = Text.substr(Start, End == std::string::npos
                                             ? std::string::npos
                                             : End - Start);
    Line = Trim(std::move(Line));
    if (!Line.empty()) {
      Lines.push_back(std::move(Line));
    }
    if (End == std::string::npos) {
      break;
    }
    Start = End + 1;
  }
  return Lines;
}

struct FileChange {
  std::string Status;
  std::vector<std::string> Paths;
};

std::vector<FileChange> ParseNameStatusDiff(const std::string& Text) {
  std::vector<FileChange> Changes;
  for (const std::string& Line : SplitLines(Text)) {
    const std::size_t FirstTab = Line.find('\t');
    if (FirstTab == std::string::npos) {
      throw WorkflowError("Git name-status output is malformed");
    }
    const std::string Status = Line.substr(0, FirstTab);
    const std::size_t SecondTab = Line.find('\t', FirstTab + 1);
    if (Status.starts_with("R") || Status.starts_with("C")) {
      if (SecondTab == std::string::npos) {
        throw WorkflowError("Git rename/copy output is incomplete");
      }
      Changes.push_back({Status, {Line.substr(FirstTab + 1, SecondTab - FirstTab - 1),
                                  Line.substr(SecondTab + 1)}});
    } else {
      Changes.push_back({Status, {Line.substr(FirstTab + 1)}});
    }
  }
  return Changes;
}

void VerifyTaskAssignment(const std::filesystem::path& RepositoryRoot,
                          const TaskSpec& Task,
                          const AssignmentRequest& Request) {
  ValidateAssignment(Task, Request, LoadDependencyStates(RepositoryRoot, Task));
}

std::vector<WorkflowEvent> ReviewBundleEventHistory(
    const TaskSpec& Task,
    const std::string& HeadCommit,
    const std::vector<WorkflowEvent>& Events) {
  std::vector<WorkflowEvent> History;
  for (const WorkflowEvent& Event : Events) {
    if (Event.TaskId != Task.TaskId) {
      continue;
    }
    if (Event.TaskRevision < Task.TaskRevision) {
      History.push_back(Event);
      continue;
    }
    if (Event.TaskRevision != Task.TaskRevision) {
      continue;
    }
    if (Event.EventId == Task.AssignmentEventId ||
        (Task.RegisteredBudgetOverride &&
         Event.EventId == Task.RegisteredBudgetOverride->ApprovalEventId)) {
      History.push_back(Event);
      continue;
    }
    const bool CurrentTaskPin = Event.TaskSpecCommit == Task.TaskSpecCommit &&
                                Event.TaskSpecBlobId == Task.TaskSpecBlobId &&
                                Event.AssignmentEventCommit == Task.AssignmentEventCommit &&
                                Event.AssignmentEventId == Task.AssignmentEventId &&
                                Event.CodeBaseCommit == Task.CodeBaseCommit;
    if (CurrentTaskPin &&
        (Event.EventType != "CommandExecuted" || Event.HeadCommit == HeadCommit)) {
      History.push_back(Event);
    }
  }
  return History;
}

int RunCommand(const std::vector<std::string>& Arguments) {
  if (Arguments.size() < 2) {
    PrintUsage();
    return 2;
  }
  const std::string& Command = Arguments[1];
  if (Command == "approve-change") {
    RejectChangeApproval();
  }
  if (Command == "merge") {
    RejectMerge();
  }
  if (Command == "history") {
    RejectUnscopedHistoryRead();
  }

  if (Command == "query") {
    if (Arguments.size() != 6) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; query must use the frozen revision");
    }
    std::cout << "TaskId=" << Task.TaskId << '\n'
              << "TaskRevision=" << Task.TaskRevision << '\n'
              << "PhaseId=" << Task.PhaseId << '\n'
              << "State=" << Task.State << '\n'
              << "Title=" << RedactSensitiveText(Task.Title) << '\n'
              << "OwnerAgentId=" << RedactSensitiveText(Task.OwnerAgentId) << '\n'
              << "Branch=" << RedactSensitiveText(Task.Branch) << '\n'
              << "BaselineCommit=" << Task.BaselineCommit << '\n';
    return 0;
  }

  if (Command == "validate-assignment") {
    if (Arguments.size() != 10) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    const AssignmentRequest Request{Arguments[4], ParseRevision(Arguments[5]), Arguments[6],
                                    Arguments[7], std::filesystem::path(Arguments[8]),
                                    Arguments[9]};
    VerifyTaskAssignment(RepositoryRoot, Task, Request);
    std::cout << "AssignmentAccepted TaskId=" << Task.TaskId
              << " TaskRevision=" << Task.TaskRevision << '\n';
    return 0;
  }

  if (Command == "context") {
    if (Arguments.size() != 6 && Arguments.size() != 10) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    WindowsProcessRunner Runner;
    const TaskSpec Task = Arguments.size() == 10
                              ? LoadTaskSpecFromPinnedSnapshot(
                                    RepositoryRoot, Arguments[3], Arguments[4], Arguments[6],
                                    Arguments[7], Arguments[8], Arguments[9], Runner)
                              : LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; context request must use the frozen revision");
    }
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    const auto BudgetOverride = Events.FindContextBudgetOverride(Task);
    const auto Files = AssembleReadSetContext(RepositoryRoot, Task, 1024U * 1024U,
                                               8U * 1024U * 1024U, BudgetOverride,
                                               Arguments.size() == 10 ? &Runner : nullptr);
    WorkflowEvent Event;
    Event.EventType = "ContextLoaded";
    Event.ReleaseId = Task.ReleaseId;
    Event.PhaseId = Task.PhaseId;
    Event.TaskId = Task.TaskId;
    Event.TaskRevision = Task.TaskRevision;
    Event.AgentId = Task.OwnerAgentId;
    Event.Branch = Task.Branch;
    Event.Result = "ReadSetOnly";
    Event.TaskSpecPath = Task.TaskPlanPath;
    Event.TaskSpecCommit = Task.TaskSpecCommit;
    Event.TaskSpecBlobId = Task.TaskSpecBlobId;
    Event.AssignmentEventCommit = Task.AssignmentEventCommit;
    Event.AssignmentEventId = Task.AssignmentEventId;
    Event.CodeBaseCommit = Task.CodeBaseCommit;
    for (const ContextFile& File : Files) {
      Event.EvidenceRefs.push_back(File.RelativePath);
    }
    Events.Append(Event);
    for (const ContextFile& File : Files) {
      std::cout << "===== " << File.RelativePath << " =====\n" << File.Content << '\n';
    }
    return 0;
  }

  if (Command == "create-worktree") {
    if (Arguments.size() != 6) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; worktree request must use the frozen revision");
    }
    const AssignmentRequest Assignment = CurrentAssignment(Task);
    VerifyTaskAssignment(RepositoryRoot, Task, Assignment);
    WindowsProcessRunner Runner;
    const WorktreeResult Result = CreateOrVerifyWorktree({RepositoryRoot, Assignment}, Runner);
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    WorkflowEvent Event;
    Event.EventType = Result.Created ? "WorktreeCreated" : "WorktreeVerified";
    Event.ReleaseId = Task.ReleaseId;
    Event.PhaseId = Task.PhaseId;
    Event.TaskId = Task.TaskId;
    Event.TaskRevision = Task.TaskRevision;
    Event.AgentId = Task.OwnerAgentId;
    Event.Branch = Result.Branch;
    Event.Result = "Isolated";
    Event.EvidenceRefs = {Result.Worktree.string(), Result.BaseCommit, Result.HeadCommit};
    Events.Append(Event);
    std::cout << "Worktree=" << Result.Worktree.string() << '\n'
              << "Branch=" << Result.Branch << '\n'
              << "BaseCommit=" << Result.BaseCommit << '\n'
              << "HeadCommit=" << Result.HeadCommit << '\n'
              << "Created=" << (Result.Created ? "true" : "false") << '\n';
    return 0;
  }

  if (Command == "run-validation") {
    if (Arguments.size() != 7 && Arguments.size() != 11) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    WindowsProcessRunner Runner;
    TaskSpec Task = Arguments.size() == 11
                        ? LoadTaskSpecFromPinnedSnapshot(
                              RepositoryRoot, Arguments[3], Arguments[4], Arguments[6],
                              Arguments[7], Arguments[8], Arguments[9], Runner)
                        : LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; validation must use the frozen revision");
    }
    const std::size_t CommandIndex = ParseIndex(Arguments[Arguments.size() == 11 ? 10 : 6]);
    if (CommandIndex >= Task.ValidationCommands.size()) {
      throw WorkflowError("Validation command index is outside the frozen allowlist");
    }
    const auto WorkingDirectory = Task.Worktree.empty()
                                      ? RepositoryRoot
                                      : std::filesystem::path(Task.Worktree);
    if (!Task.TaskSpecCommit.empty()) {
      const WorktreeResult Verified = VerifyExistingWorktreeForReview(
          {RepositoryRoot, CurrentAssignment(Task)}, Task.State, Runner);
      Task.FixedHeadCommit = Verified.HeadCommit;
    } else {
      Task.FixedHeadCommit = Trim(Runner.Run(
          {"git", "-C", WorkingDirectory.string(), "rev-parse", "HEAD"},
          WorkingDirectory).Output);
    }
    const auto EvidencePath = ValidationEvidencePathFor(Task, RepositoryRoot, CommandIndex);
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    const ValidationResult Result = RunFrozenValidation(
        Task, Task.ValidationCommands[CommandIndex], WorkingDirectory, EvidencePath,
        Runner, Events, Task.OwnerAgentId, Task.TaskId + "-validation-" +
                                            std::to_string(CommandIndex));
    std::cout << "Command=" << Result.Command << '\n'
              << "WorkingDirectory=" << Result.WorkingDirectory.string() << '\n'
              << "ExitCode=" << Result.ExitCode << '\n'
              << "EvidencePath=" << Result.EvidencePath.string() << '\n'
              << "EventId=" << Result.EventId << '\n';
    return static_cast<int>(Result.ExitCode);
  }

  if (Command == "checkpoint-save") {
    if (Arguments.size() != 7) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; checkpoint must use the frozen revision");
    }
    const auto Worktree = Task.Worktree.empty() ? RepositoryRoot
                                                : std::filesystem::path(Task.Worktree);
    WindowsProcessRunner Runner;
    const std::string Head = Trim(Runner.Run(
        {"git", "-C", Worktree.string(), "rev-parse", "HEAD"}, Worktree).Output);
    Checkpoint Value;
    Value.TaskId = Task.TaskId;
    Value.TaskRevision = Task.TaskRevision;
    Value.Branch = Task.Branch;
    Value.Worktree = Worktree.string();
    Value.BaseCommit = Task.BaselineCommit;
    Value.HeadCommit = Head;
    Value.NextAction = Arguments[6];
    const auto CheckpointPath = CheckpointPathFor(RepositoryRoot, Task);
    CheckpointStore::Save(CheckpointPath, Value);
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    WorkflowEvent Event;
    Event.EventType = "CheckpointSaved";
    Event.ReleaseId = Task.ReleaseId;
    Event.PhaseId = Task.PhaseId;
    Event.TaskId = Task.TaskId;
    Event.TaskRevision = Task.TaskRevision;
    Event.AgentId = Task.OwnerAgentId;
    Event.Branch = Task.Branch;
    Event.Result = "Saved";
    Event.EvidenceRefs = {CheckpointPath.string(), Task.BaselineCommit, Head};
    Events.Append(Event);
    std::cout << "CheckpointSaved TaskId=" << Value.TaskId
              << " TaskRevision=" << Value.TaskRevision << '\n';
    return 0;
  }

  if (Command == "checkpoint-recover") {
    if (Arguments.size() != 6) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; checkpoint recovery must use the frozen revision");
    }
    const Checkpoint Value = CheckpointStore::Recover(
        CheckpointPathFor(RepositoryRoot, Task), Task.TaskId, Task.TaskRevision);
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    WorkflowEvent Event;
    Event.EventType = "CheckpointRecovered";
    Event.ReleaseId = Task.ReleaseId;
    Event.PhaseId = Task.PhaseId;
    Event.TaskId = Task.TaskId;
    Event.TaskRevision = Task.TaskRevision;
    Event.AgentId = Task.OwnerAgentId;
    Event.Branch = Value.Branch;
    Event.Result = "Recovered";
    Event.EvidenceRefs = {CheckpointPathFor(RepositoryRoot, Task).string(),
                          Value.BaseCommit, Value.HeadCommit};
    Events.Append(Event);
    std::cout << "TaskId=" << Value.TaskId << '\n'
              << "TaskRevision=" << Value.TaskRevision << '\n'
              << "Branch=" << RedactSensitiveText(Value.Branch) << '\n'
              << "Worktree=" << RedactSensitiveText(Value.Worktree) << '\n'
              << "BaseCommit=" << Value.BaseCommit << '\n'
              << "HeadCommit=" << Value.HeadCommit << '\n'
              << "NextAction=" << RedactSensitiveText(Value.NextAction) << '\n';
    return 0;
  }

  if (Command == "messages") {
    if (Arguments.size() != 5) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.CommunicationShard.empty()) {
      throw WorkflowError("TaskIndex does not route a communication shard for this phase");
    }
    const auto MessagePath = RepositoryRoot / Task.CommunicationShard;
    TaskScopedMessageStore Messages(MessagePath);
    for (const TaskScopedMessage& Message :
         Messages.ReadOpenForTask(Task.TaskId, Task.TaskRevision)) {
      std::cout << "MessageId=" << Message.MessageId << '\n'
                << "Type=" << Message.MessageType << '\n'
                << "Subject=" << RedactSensitiveText(Message.Subject) << '\n'
                << "Summary=" << RedactSensitiveText(Message.Summary) << '\n';
    }
    return 0;
  }

  if (Command == "message-add") {
    if (Arguments.size() != 11) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; communication message must use the frozen revision");
    }
    if (Task.CommunicationShard.empty()) {
      throw WorkflowError("TaskIndex does not route a communication shard for this phase");
    }
    const auto Tick = std::chrono::steady_clock::now().time_since_epoch().count();
    TaskScopedMessage Message{"Message-" + Task.TaskId + "-" + std::to_string(Tick),
                              Task.TaskId, Task.TaskRevision, Arguments[6], Arguments[7],
                              Arguments[8], "Open", Arguments[9], Arguments[10], {}};
    TaskScopedMessageStore Messages(RepositoryRoot / Task.CommunicationShard);
    Messages.Append(Message);
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    WorkflowEvent Event;
    Event.EventType = "AgentMessageSent";
    Event.ReleaseId = Task.ReleaseId;
    Event.PhaseId = Task.PhaseId;
    Event.TaskId = Task.TaskId;
    Event.TaskRevision = Task.TaskRevision;
    Event.AgentId = Message.SenderAgentId;
    Event.Branch = Task.Branch;
    Event.Result = "Open";
    Event.EvidenceRefs = {Message.MessageId};
    Events.Append(Event);
    std::cout << "MessageId=" << Message.MessageId << '\n';
    return 0;
  }

  if (Command == "message-close") {
    if (Arguments.size() != 8) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    const TaskSpec Task = LoadTask(RepositoryRoot, Arguments[3], Arguments[4]);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; message resolution must use the frozen revision");
    }
    if (Task.CommunicationShard.empty()) {
      throw WorkflowError("TaskIndex does not route a communication shard for this phase");
    }
    TaskScopedMessageStore Messages(RepositoryRoot / Task.CommunicationShard);
    const TaskScopedMessage Closed = Messages.ResolveForTask(
        Task.TaskId, Task.TaskRevision, Arguments[6]);
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    WorkflowEvent Event;
    Event.EventType = "AgentMessageResolved";
    Event.ReleaseId = Task.ReleaseId;
    Event.PhaseId = Task.PhaseId;
    Event.TaskId = Task.TaskId;
    Event.TaskRevision = Task.TaskRevision;
    Event.AgentId = Arguments[7];
    Event.Branch = Task.Branch;
    Event.Result = "Closed";
    Event.EvidenceRefs = {Closed.MessageId};
    Events.Append(Event);
    std::cout << "MessageResolved=" << Closed.MessageId << '\n';
    return 0;
  }

  if (Command == "review-bundle") {
    if (Arguments.size() != 12) {
      PrintUsage();
      return 2;
    }
    const auto RepositoryRoot = std::filesystem::absolute(Arguments[2]);
    WindowsProcessRunner Runner;
    TaskSpec Task = LoadTaskSpecFromPinnedSnapshot(
        RepositoryRoot, Arguments[3], Arguments[4], Arguments[6], Arguments[7],
        Arguments[8], Arguments[9], Runner);
    if (Task.TaskRevision != ParseRevision(Arguments[5])) {
      throw WorkflowError("Stale TaskRevision; ReviewBundle must use the frozen revision");
    }
    if (Task.CodeBaseCommit.empty() || Task.Worktree.empty() ||
        Arguments[10] != Task.ReviewerAgentId || Arguments[10] == Task.OwnerAgentId) {
      throw WorkflowError("ReviewBundle requires the pinned CodeBase, assigned worktree, and independent Reviewer");
    }
    const WorktreeResult VerifiedWorktree = VerifyExistingWorktreeForReview(
        {RepositoryRoot, CurrentAssignment(Task)}, Task.State, Runner);
    const auto Worktree = VerifiedWorktree.Worktree;
    const std::string Head = VerifiedWorktree.HeadCommit;
    Task.FixedHeadCommit = Head;
    const ProcessResult DiffResult = Runner.Run(
        {"git", "-C", Worktree.string(), "diff", "--name-status", "--find-renames",
         Task.CodeBaseCommit, Head}, Worktree);
    if (DiffResult.ExitCode != 0) {
      throw WorkflowError("Unable to collect the fixed Git diff for ReviewBundle");
    }
    ReviewBundleData Bundle;
    Bundle.TaskSpecPath = Task.TaskPlanPath;
    Bundle.TaskSpecCommit = Task.TaskSpecCommit;
    Bundle.TaskSpecBlobId = Task.TaskSpecBlobId;
    Bundle.AssignmentEventCommit = Task.AssignmentEventCommit;
    Bundle.AssignmentEventId = Task.AssignmentEventId;
    Bundle.CodeBaseCommit = Task.CodeBaseCommit;
    Bundle.BaseCommit = Task.CodeBaseCommit;
    Bundle.HeadCommit = Head;
    Bundle.AuthorAgentId = Task.OwnerAgentId;
    Bundle.ReviewerAgentId = Arguments[10];
    EventStore Events(EventLogPath(RepositoryRoot, Task));
    for (const FileChange& Change : ParseNameStatusDiff(DiffResult.Output)) {
      for (const std::string& ChangedPath : Change.Paths) {
        Bundle.ChangedPaths.push_back(ChangedPath);
        Bundle.ChangeKinds.push_back(Change.Status);
      }
    }
    const auto GitDiffEvidence = GitDiffEvidencePathFor(Task, RepositoryRoot);
    const ValidationResult GitDiffValidation = RunTypedGitDiffCheck(
        Task, Head, Worktree, GitDiffEvidence, Runner, Events, Task.OwnerAgentId,
        Task.TaskId + "-r" + std::to_string(Task.TaskRevision) + "-git-diff-check");
    if (GitDiffValidation.ExitCode != 0) {
      throw WorkflowError("Typed GitDiffCheck failed; evidence was recorded but no ReviewBundle was written");
    }
    const auto TaskEvents = Events.ReadTaskEvents(Task.TaskId);
    Bundle.Validations = CollectTaskRevisionValidations(Task, TaskEvents);
    Bundle.Events = ReviewBundleEventHistory(Task, Head, TaskEvents);
    const std::filesystem::path RequestedOutput = std::filesystem::absolute(Arguments[11]);
    WriteLocalReviewBundle(RepositoryRoot, Task, Bundle, RequestedOutput, Events);
    std::cout << "LocalReviewBundle=" << RequestedOutput.string() << '\n'
              << "TaskSpecCommit=" << Task.TaskSpecCommit << '\n'
              << "TaskSpecBlobId=" << Task.TaskSpecBlobId << '\n'
              << "AssignmentEventCommit=" << Task.AssignmentEventCommit << '\n'
              << "AssignmentEventId=" << Task.AssignmentEventId << '\n'
              << "CodeBaseCommit=" << Task.CodeBaseCommit << '\n'
              << "HeadCommit=" << Head << '\n'
              << "RemotePrCreated=false\n";
    return 0;
  }

  PrintUsage();
  return 2;
}

}  // namespace

int main(int Argc, char** Argv) {
  try {
    std::vector<std::string> Arguments;
    Arguments.reserve(static_cast<std::size_t>(Argc));
    for (int Index = 0; Index < Argc; ++Index) {
      Arguments.emplace_back(Argv[Index]);
    }
    return RunCommand(Arguments);
  } catch (const std::exception& Error) {
    std::cerr << "WorkflowControllerError: "
              << RedactSensitiveText(Error.what()) << '\n';
    return 1;
  }
}
