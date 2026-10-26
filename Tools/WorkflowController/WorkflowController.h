#pragma once

#include <filesystem>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace EmulativeDialog::WorkflowController {

class WorkflowError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class EventStore;

struct ContextBudgetOverride {
  std::string TaskId;
  int TaskRevision = 0;
  std::size_t MaxReferenceFiles = 0;
  std::string Reason;
  std::string ApprovedByRole;
  std::string ApprovalEventId;
  std::string ApprovalEventRef;
};

class VerifiedContextBudgetOverride final {
 public:
  const ContextBudgetOverride& Value() const noexcept { return Override; }
  const std::string& TaskSpecCommit() const noexcept { return FrozenTaskSpecCommit; }
  const std::string& TaskSpecBlobId() const noexcept { return FrozenTaskSpecBlobId; }
  const std::string& AssignmentEventCommit() const noexcept {
    return FrozenAssignmentEventCommit;
  }
  const std::string& AssignmentEventId() const noexcept { return FrozenAssignmentEventId; }
  const std::string& BudgetCorrectionEventCommit() const noexcept {
    return FrozenBudgetCorrectionEventCommit;
  }
  const std::string& BudgetCorrectionEventId() const noexcept {
    return FrozenBudgetCorrectionEventId;
  }

 private:
  VerifiedContextBudgetOverride(ContextBudgetOverride Override,
                                std::string TaskSpecCommit,
                                std::string TaskSpecBlobId,
                                std::string AssignmentEventCommit,
                                std::string AssignmentEventId,
                                std::string BudgetCorrectionEventCommit,
                                std::string BudgetCorrectionEventId)
      : Override(std::move(Override)),
        FrozenTaskSpecCommit(std::move(TaskSpecCommit)),
        FrozenTaskSpecBlobId(std::move(TaskSpecBlobId)),
        FrozenAssignmentEventCommit(std::move(AssignmentEventCommit)),
        FrozenAssignmentEventId(std::move(AssignmentEventId)),
        FrozenBudgetCorrectionEventCommit(std::move(BudgetCorrectionEventCommit)),
        FrozenBudgetCorrectionEventId(std::move(BudgetCorrectionEventId)) {}

  ContextBudgetOverride Override;
  std::string FrozenTaskSpecCommit;
  std::string FrozenTaskSpecBlobId;
  std::string FrozenAssignmentEventCommit;
  std::string FrozenAssignmentEventId;
  std::string FrozenBudgetCorrectionEventCommit;
  std::string FrozenBudgetCorrectionEventId;
  friend class EventStore;
};

struct TaskSpec {
  std::string TaskId;
  int TaskRevision = 0;
  std::string ReleaseId = "Bootstrap";
  std::string PhaseId;
  std::string TaskPlanPath;
  std::string Title;
  std::string Goal;
  std::string State;
  std::string OwnerAgentId;
  std::string Branch;
  std::string Worktree;
  std::string BaselineCommit;
  std::string ReviewerAgentId;
  std::string TaskSpecCommit;
  std::string TaskSpecBlobId;
  std::string AssignmentEventCommit;
  std::string AssignmentEventId;
  std::string CodeBaseCommit;
  std::string FixedHeadCommit;
  std::string BudgetCorrectionEventCommit;
  std::string BudgetCorrectionEventId;
  std::string EventShard;
  std::string CommunicationShard;
  std::vector<std::string> DependsOn;
  std::vector<std::string> ReadSet;
  std::vector<std::string> WriteSet;
  std::vector<std::string> ForbiddenSet;
  std::vector<std::string> SharedLocks;
  std::vector<std::string> ValidationCommands;
  std::vector<std::string> StaticChecks;
  std::size_t MaxReferenceFiles = 12;
  std::optional<ContextBudgetOverride> RegisteredBudgetOverride;
};

struct AssignmentRequest {
  std::string TaskId;
  int TaskRevision = 0;
  std::string OwnerAgentId;
  std::string Branch;
  std::filesystem::path Worktree;
  std::string BaseCommit;
};

struct ContextFile {
  std::string RelativePath;
  std::string Content;
};

struct ProcessResult {
  unsigned long ExitCode = 0;
  std::string Output;
};

class ProcessRunner {
 public:
  virtual ~ProcessRunner() = default;
  virtual ProcessResult Run(const std::vector<std::string>& Arguments,
                            const std::filesystem::path& WorkingDirectory) = 0;
};

class WindowsProcessRunner final : public ProcessRunner {
 public:
  ProcessResult Run(const std::vector<std::string>& Arguments,
                    const std::filesystem::path& WorkingDirectory) override;
};

struct WorktreeRequest {
  std::filesystem::path RepositoryRoot;
  AssignmentRequest Assignment;
};

struct WorktreeResult {
  std::filesystem::path Worktree;
  std::string Branch;
  std::string BaseCommit;
  std::string HeadCommit;
  bool Created = false;
};

struct WorkflowEvent {
  std::string EventId;
  std::string EventType;
  std::string OccurredAt;
  std::string ReleaseId = "Bootstrap";
  std::string PhaseId;
  std::string TaskId;
  int TaskRevision = 0;
  std::string AgentId;
  std::string AgentRole;
  std::string Branch;
  std::string CorrelationId;
  std::string Result;
  std::vector<std::string> EvidenceRefs;
  std::string Command;
  std::string WorkingDirectory;
  long ExitCode = 0;
  std::string EvidencePath;
  std::string ApprovalReason;
  std::size_t MaxReferenceFiles = 0;
  std::string TaskSpecPath;
  std::string TaskSpecCommit;
  std::string TaskSpecBlobId;
  std::string AssignmentEventCommit;
  std::string AssignmentEventId;
  std::string CodeBaseCommit;
  std::string HeadCommit;
  std::string BudgetCorrectionEventCommit;
  std::string BudgetCorrectionEventId;
  std::string CorrectionOfEventId;
  std::string CorrectedField;
  std::string OriginalApprovalReason;
  std::string CorrectedApprovalReason;
  std::string ChangeRequestId;
  std::string ContextBudgetApprovalEventId;
  std::string OwnerAgentId;
  std::string ReviewerAgentId;
  std::string Worktree;
  std::string OutputSummary;
  std::vector<std::string> ReadSetPaths;
};

struct Checkpoint {
  std::string TaskId;
  int TaskRevision = 0;
  std::string Branch;
  std::string Worktree;
  std::string BaseCommit;
  std::string HeadCommit;
  std::string LastCompletedAcceptance;
  std::string CurrentFindingOrFailure;
  std::vector<std::string> TriedApproaches;
  std::string NextAction;
  std::vector<std::string> OpenMessageIds;
  std::string LastValidationResult;
};

struct ValidationResult {
  std::string Command;
  std::filesystem::path WorkingDirectory;
  unsigned long ExitCode = 0;
  std::filesystem::path EvidencePath;
  std::string EventId;
  std::string HeadCommit;
};

std::vector<ValidationResult> CollectTaskRevisionValidations(
    const TaskSpec& Task,
    const std::vector<WorkflowEvent>& Events);

struct TaskScopedMessage {
  std::string MessageId;
  std::string TaskId;
  int TaskRevision = 0;
  std::string SenderAgentId;
  std::string RecipientAgentId;
  std::string MessageType;
  std::string Status;
  std::string Subject;
  std::string Summary;
  std::vector<std::string> EvidenceRefs;
};

struct ReviewBundleData {
  std::string TaskSpecPath;
  std::string TaskSpecCommit;
  std::string TaskSpecBlobId;
  std::string AssignmentEventCommit;
  std::string AssignmentEventId;
  std::string CodeBaseCommit;
  std::string BudgetCorrectionEventCommit;
  std::string BudgetCorrectionEventId;
  std::string BaseCommit;
  std::string HeadCommit;
  std::string AuthorAgentId;
  std::string ReviewerAgentId;
  bool RemotePrCreated = false;
  std::vector<std::string> ChangedPaths;
  std::vector<std::string> ChangeKinds;
  std::vector<ValidationResult> Validations;
  std::vector<WorkflowEvent> Events;
};

TaskSpec LoadTaskSpec(const std::filesystem::path& RepositoryRoot,
                      const std::filesystem::path& TaskIndexPath,
                      const std::string& PhaseId,
                      const std::string& TaskId);

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
    ProcessRunner& Runner);

std::string LoadTaskState(const std::filesystem::path& RepositoryRoot,
                          const std::filesystem::path& TaskIndexPath,
                          const std::string& TaskId);

bool IsAllowedStateTransition(const std::string& CurrentState,
                              const std::string& NextState);

void ValidateAssignment(const TaskSpec& Task,
                        const AssignmentRequest& Request,
                        const std::map<std::string, std::string>& DependencyStates);

std::vector<ContextFile> AssembleReadSetContext(
    const std::filesystem::path& RepositoryRoot,
    const TaskSpec& Task,
    std::size_t MaximumFileBytes = 1024U * 1024U,
    std::size_t MaximumTotalBytes = 8U * 1024U * 1024U,
    const std::optional<VerifiedContextBudgetOverride>& BudgetOverride = std::nullopt,
    ProcessRunner* PinnedSnapshotRunner = nullptr);

void ValidateReadSetPath(const TaskSpec& Task, const std::string& RelativePath);

void RequestAdditionalRead(const TaskSpec& Task,
                           const std::string& RelativePath,
                           const std::string& Reason,
                           const EventStore& Events,
                           const std::string& AgentId,
                           const std::string& CorrelationId);

void ValidateWriteSetPaths(const TaskSpec& Task,
                           const std::vector<std::string>& ChangedPaths);

std::vector<std::string> FindWriteSetConflicts(const TaskSpec& Left,
                                               const TaskSpec& Right);

std::string RedactSensitiveText(const std::string& Text);

class EventStore final {
 public:
  explicit EventStore(std::filesystem::path EventPath,
                      std::function<bool()> FlushOperation = {});
  void Append(const WorkflowEvent& Event) const;
  std::vector<WorkflowEvent> ReadTaskEvents(const std::string& TaskId,
                                            std::size_t MaximumEvents = 1000) const;
  std::optional<VerifiedContextBudgetOverride> FindContextBudgetOverride(
      const TaskSpec& Task) const;

 private:
  std::filesystem::path EventPath;
  std::function<bool()> FlushOperation;
};

class CheckpointStore final {
 public:
  static void Save(const std::filesystem::path& CheckpointPath,
                   const Checkpoint& Value);
  static Checkpoint Recover(const std::filesystem::path& CheckpointPath,
                            const std::string& ExpectedTaskId,
                            int ExpectedTaskRevision);
};

WorktreeResult CreateOrVerifyWorktree(const WorktreeRequest& Request,
                                      ProcessRunner& Runner);

WorktreeResult VerifyExistingWorktreeForReview(const WorktreeRequest& Request,
                                               const std::string& TaskState,
                                               ProcessRunner& Runner);

ValidationResult RunFrozenValidation(const TaskSpec& Task,
                                     const std::string& ExactCommand,
                                     const std::filesystem::path& WorkingDirectory,
                                     const std::filesystem::path& EvidencePath,
                                     ProcessRunner& Runner,
                                     const EventStore& Events,
                                     const std::string& AgentId,
                                     const std::string& CorrelationId);

ValidationResult RunTypedGitDiffCheck(const TaskSpec& Task,
                                      const std::string& HeadCommit,
                                      const std::filesystem::path& WorkingDirectory,
                                      const std::filesystem::path& EvidencePath,
                                      ProcessRunner& Runner,
                                      const EventStore& Events,
                                      const std::string& AgentId,
                                      const std::string& CorrelationId);

class TaskScopedMessageStore final {
 public:
  explicit TaskScopedMessageStore(std::filesystem::path MessagePath);
  void Append(const TaskScopedMessage& Message) const;
  TaskScopedMessage ResolveForTask(const std::string& TaskId,
                                   int TaskRevision,
                                   const std::string& MessageId) const;
  std::vector<TaskScopedMessage> ReadOpenForTask(const std::string& TaskId,
                                                 int TaskRevision,
                                                 std::size_t MaximumMessages = 100) const;

 private:
  std::filesystem::path MessagePath;
};

void WriteLocalReviewBundle(const std::filesystem::path& RepositoryRoot,
                            const TaskSpec& Task,
                            const ReviewBundleData& Bundle,
                            const std::filesystem::path& OutputPath,
                            const EventStore& Events);

std::filesystem::path ResolveTaskEventPath(
    const std::filesystem::path& RepositoryRoot,
    const TaskSpec& Task);

std::filesystem::path ResolveTaskCheckpointPath(
    const std::filesystem::path& RepositoryRoot,
    const TaskSpec& Task);

[[noreturn]] void RejectChangeApproval();
[[noreturn]] void RejectMerge();
[[noreturn]] void RejectUnscopedHistoryRead();

}  // namespace EmulativeDialog::WorkflowController
