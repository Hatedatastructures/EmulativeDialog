#pragma once

#include <filesystem>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace EmulativeDialog::WorkflowController {

class WorkflowError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ContextBudgetOverride {
  std::string TaskId;
  int TaskRevision = 0;
  std::size_t MaxReferenceFiles = 0;
  std::string Reason;
  std::string ApprovedByRole;
  std::string ApprovalEventId;
  std::string ApprovalEventRef;
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
  std::string EventShard;
  std::string CommunicationShard;
  std::vector<std::string> DependsOn;
  std::vector<std::string> ReadSet;
  std::vector<std::string> WriteSet;
  std::vector<std::string> ForbiddenSet;
  std::vector<std::string> SharedLocks;
  std::vector<std::string> ValidationCommands;
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
};

class EventStore;

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
};

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
    const std::optional<ContextBudgetOverride>& BudgetOverride = std::nullopt);

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
  explicit EventStore(std::filesystem::path EventPath);
  void Append(const WorkflowEvent& Event) const;
  std::vector<WorkflowEvent> ReadTaskEvents(const std::string& TaskId,
                                            std::size_t MaximumEvents = 1000) const;
  std::optional<ContextBudgetOverride> FindContextBudgetOverride(
      const TaskSpec& Task) const;

 private:
  std::filesystem::path EventPath;
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

ValidationResult RunFrozenValidation(const TaskSpec& Task,
                                     const std::string& ExactCommand,
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

[[noreturn]] void RejectChangeApproval();
[[noreturn]] void RejectMerge();
[[noreturn]] void RejectUnscopedHistoryRead();

}  // namespace EmulativeDialog::WorkflowController
