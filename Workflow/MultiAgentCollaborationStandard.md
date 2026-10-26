# 多 Agent 协作规范

## 范围与决策边界

本规范约束开发期任务分派、实施、评审和集成。Coordinator 负责任务路由与集成队列；六个 Worker 槽跨相关任务保持稳定身份。任务沟通可以报告事实或提出建议，但不能改变已冻结的 Goal、Acceptance、ReadSet、WriteSet、依赖或优先级。范围变化必须经批准的 ChangeRequest 发布为新的 TaskRevision。

本规范建立 P1 范围的人工证据路径，不代表 P1 ExitGate 已通过，不代表产品代码已经完成，也不表示 AgentRuntime 已实现。当前阶段门的状态以 [Workflow/Plans/P1.yaml](Plans/P1.yaml) 为准；Coordinator 完成证据评估与签署前，ExitGate 保持 Pending。

## 稳定槽位与并行工作

| 槽位 | 稳定 Agent | P1 工作类型 | 职责 |
|---|---|---|---|
| Worker01 | /root/workflow_controller | 支持 | Windows 构建、AgentWorkflowController、任务与工作流工具 |
| Worker02 | /root/agent_core | 实现 | Codex AgentRuntime 与 Core |
| Worker03 | /root/provider_responses | 实现 | OpenAI Responses Provider |
| Worker04 | /root/provider_chat | 实现 | OpenAI-compatible Chat Completions Provider |
| Worker05 | /root/provider_anthropic | 实现 | Anthropic Messages Provider |
| Worker06 | /root/memory_context | 支持 | Memory/Context 合同、夹具和评审支持；合同与依赖冻结后可承担独立实现任务 |

当前四个并行产品实现槽为 Worker02–Worker05，Worker01 和 Worker06 是 P1 协作模型中的两个支持槽。Worker06 保留 Memory/Context 职责；后续实现仍须由 Coordinator 分派独立且 Ready 的 TaskSpec。Coordinator 单独负责计划、审批与集成，不计入六个子代理槽。

每次分派的有效并行数为六个配置槽、宿主支持容量、依赖已满足且 WriteSet 独立的 Ready 任务数三者中的最小值。没有独立 Ready 任务时，槽位保持空闲；不得为了填满槽位制造重复工作。相关任务完成后优先复用原稳定槽；宿主会话结束时从检查点恢复，不得声称原会话仍在运行。

## 冻结、隔离与实施

实施前由 Coordinator 冻结 TaskSpec：唯一 TaskId 和 TaskRevision、Owner、独立 Reviewer、精确 ReadSet 和 WriteSet、依赖、Acceptance、验证命令及 SharedLocks。ReadSet 默认最多 12 个引用文件；超限必须有任务范围内获批的例外理由和事件引用。不得载入无关任务历史或全量源码树。

只有依赖满足且状态为 Ready 的任务可以开始。依赖模块的公共合同必须先冻结。每个实现任务使用独立分支和 Git worktree，并固定 BaseCommit。任务只能修改冻结的 WriteSet；共享合同、根 CMake/依赖文件、TaskIndex、AgentPool 分派和集成分支由登记的单一 owner 排队维护。沟通消息不能授权额外写入。

作者记录实际变更路径，按可审查的模块切片提交任务分支，并在远端权限允许时推送；推送后通知 Coordinator，并交付固定的 BaseCommit 和 HeadCommit。验证记录包含实际命令、工作目录、退出码和可读取的证据路径。按模块测量新增或变更生产 C++ 代码的行覆盖率，至少为 80%；报告分母、排除项和工具版本。缺少报告或低于阈值即不通过；不得排除业务分支、复用旧报告、缩小分母或把文档变更说成产品实现。

### 远端分支上限

仓库远端最多保留 4 个分支，包含默认集成分支 `codex/integration`；因此最多同时存在 3 个任务/PR 分支。Coordinator 创建任务分支前必须用 GitHub 插件核对当前分支数，并把分支预算加入 Ready 独立任务的并行度计算。任务完成独立审查并合入目标分支后，将任务分支快进到集成头作为合并结果确认，再删除远端任务分支引用；本地 ReviewBundle、TaskSpec、提交和已有 worktree 保留作审计，不因分支清理而删除或重置。若 PR 插件拒绝创建 PR，记录精确错误并按 TaskLifecycle 使用独立 LocalReviewBundle；本地 Bundle 不能冒充 PR。

## 评审、PR 与集成

Reviewer 必须与作者不同。Reviewer 对固定的 BaseCommit..HeadCommit 检查精确 WriteSet、验收证据、安全和任务风险。Finding 必须关联具体路径/行，并由作者在自己的任务分支修复。固定 Head 在评审期间不得改变；如需修复，产生新的 Head 和对应验证证据。

Coordinator 使用已连接的 GitHub 插件尝试创建正式 Pull Request、填写完整任务摘要并请求独立固定 Head 评审。摘要包含 TaskId/TaskRevision、目标与依赖、固定 Base/Head、变更路径、Acceptance 对照、实际验证命令及退出码、Finding/风险和后续工作。作者提交并推送独立 task branch 后通知 Coordinator；只有插件返回 PR 编号和链接后才可报告 PR 已创建。插件返回权限错误时，记录精确状态码和错误。在 P1/bootstrap 范围内，只有独立 Reviewer 批准固定 Base/Head 的 LocalReviewBundle 且 IntegrationOwner 复核最终语义差异后，Coordinator 才可按 TaskLifecycle 批准本地集成；该本地审查/合并不是 GitHub PR，也不能满足 LargeScaleProductImplementation 的正式 PR、required checks 或 branch protection 条件。产品实现不能把 LocalReviewBundle 当作产品 PR。

### 平台证据快照

以下是 2026-10-26 的仓库与平台观察结果；大规模产品开发前应重新核验：

- 仓库元数据显示公开仓库 Hatedatastructures/EmulativeDialog 的默认分支为 codex/integration，且仓库启用了 Pull Request 功能。
- 分支 API 报告 codex/integration 未受保护，protection 为 disabled，检查执行策略为 off，required contexts 和 checks 均为空；rulesets 接口返回空列表。
- 查询详细分支保护规则返回 HTTP 403，错误为 Resource not accessible by integration。GitHub 插件创建 PR 的请求也返回相同 HTTP 403。本机 gh CLI 未认证。
- CurrentState 与 ED-0019 事件记录的是事件账本恢复已本地审查并合入；该本地集成和 ReviewBundle 不是产品代码 PR。

因此，仓库元数据虽然启用了 PR 功能，但当前连接身份没有创建 PR 的权限，分支保护和必需检查也未配置或无法验证。不得把本地提交或 ReviewBundle 说成远端 PR、受保护合并或已通过的必需检查。

IntegrationOwner 串行处理合并。合并前核对目标分支与远端 Head、依赖顺序、独立评审、必需检查和 WriteSet。出现冲突时暂停合并，固定双方 Base/Head，列出冲突路径，并区分文本冲突与语义冲突。IntegrationOwner 在集成 worktree 处理机械冲突；合同或行为冲突交由相关 owner 通过 ChangeRequest 决策。重跑受冲突影响的验证，审查最终合并差异和行为并请求最终语义复审，记录解决提交，并通知依赖任务更新基线。bootstrap LocalReviewBundle 本地集成须遵守同一冲突与最终语义审查流程；它不满足产品 PR/分支保护门。不得在其他作者的 worktree 内解决冲突，也不得强推覆盖对方分支。

## MAN-003 P1 人工证据路径

[Workflow/EnforcementMatrix.yaml](EnforcementMatrix.yaml) 中的 MAN-003 为 P1Blockers.ProductAgentImplementation 和 P1Blockers.LargeScaleProductImplementation 的每个不同 RuleId 建立人工证据登记。每一行列出 P1StageStatus、FutureGateStatus、责任 owner、核验控制、所需证据、明确通过条件、证据状态、独立评审和 Coordinator 签署状态。对 MERGE-001/002，P1 bootstrap 本地 ReviewBundle/集成条件与后续 LargeScale PR/保护条件分开记录；对 CODEX-001，P1 只核验 ED-0101 至 ED-0115 TaskSpec 登记，CodexRewriteRelease 的 C++ 实现和产品 PR 仍保持 Open。每项证据及决定至少绑定 TaskId、TaskRevision、BaseCommit、HeadCommit、SourceCommit、RuleId、适用时的 CapabilityCounts、证据引用、验证命令与退出码、Finding、Reviewer 身份与决定、Coordinator 签署引用。

收集证据、完成任务或编写文档都不会自动通过规则。独立 Reviewer 必须与作者不同。Coordinator 根据证据和 Reviewer Finding 作出范围明确的签署决定；未解决的阻塞项继续保持开放。矩阵状态为 Pending；Coordinator 将签署结论写入阶段计划前，P1 ExitGate 仍为 Pending。MAP-001 仍须满足现有 MAN-002 人工门。CODEX-001 保持开放：CapabilityMap 的映射不能代替各项 Keep/Redesign 能力的 C++ 实现和验收。

该登记只覆盖当前 P1 阻塞。TUI-001 不属于本门，只阻塞未来 TUI Epic。微信继续 Deferred；两者都不改变当前 QQ AgentRuntime 范围，也不阻塞 P1 Agent 核心门。

## 参考

- [Workflow/AgentPool.yaml](AgentPool.yaml)
- [Workflow/AgentExecutionStandard.md](AgentExecutionStandard.md)
- [Workflow/TaskLifecycle.md](TaskLifecycle.md)
- [Workflow/EnforcementMatrix.yaml](EnforcementMatrix.yaml)
- [Workflow/CurrentState.md](CurrentState.md)
- [Workflow/Plans/P1.yaml](Plans/P1.yaml)
