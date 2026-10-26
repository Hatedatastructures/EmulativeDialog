# 任务、分支与 PR 生命周期

本文件定义开发任务状态、隔离分支、独立审查和集成冲突处理。任务/阶段的选择与上下文加载规则见 Workflow/AgentExecutionStandard.md。

## 状态机

| 状态 | 进入条件 | 允许动作 |
|---|---|---|
| Proposed | 目标或依赖尚未冻结 | 补证据、拆分、讨论，不写产品代码 |
| Ready | TaskSpec 完整、依赖满足、合同版本锁定、写集无冲突 | 等待协调者分配 |
| Assigned | 已绑定 AgentId、分支、worktree、基准提交 | 完成环境与权限核对 |
| Active | Agent 已读取必需上下文并开始实施 | 仅在批准的写集内变更、记录检查点 |
| Review | HeadCommit 固定，提交 PR/ReviewBundle | 独立 Reviewer 只读审查和验证 |
| Rework | 有具体 Finding 或受影响目标验证失败 | 作者在原写集内修复；范围变化另走 ChangeRequest |
| Verified | 全部验收项有证据，阻塞级 Finding 已处理 | 等待依赖顺序与合并队列 |
| MergeQueued | 目标分支、依赖和合并基准已确认 | 集成者串行处理 merge/rebase/conflict |
| Merged | PR 已合并且提交记录可追溯 | 更新能力地图、阶段状态和后继任务 |
| Blocked | 外部能力、未决产品选择、权限/安全风险或同因重复失败阻塞 | 保留工作与证据，只等待对应决策或条件变化 |
| Superseded | 获批的新任务版本取代旧任务 | 保留原记录，只读归档 |

禁止从 Active 直接跳到 Verified 或 Merged。TaskState 由 Coordinator 单写者更新；Agent 可提交状态建议和证据，不能自行替换任务规格或将自己任务标为通过。

## 任务记录与不可变性

任务正文以 TaskIndex 指向的阶段/模块分片结构化保存，不为每个任务建立一篇 Markdown。Ready 时冻结 TaskRevision、Goal、Acceptance、ReadSet、WriteSet、ForbiddenSet、Dependencies、ContractVersion、BaselineCommit、Validation 和 RiskLevel。

已冻结字段要变更时：

1. 提交 ChangeRequest，列出证据、影响路径、兼容/安全影响和回退方案。
2. Owner 或 ProjectOwner 决定接受、拒绝或延期。
3. 接受后生成新 TaskRevision，并显式关联旧版本；旧验收和旧证据保留。
4. 写集变化需重新做冲突检测和分支分配。

Agent 间的建议、私聊和 Reviewer 评论本身都不修改 TaskRevision。

## 分支与工作树

- 每个活动实现任务使用独立 Git worktree 和独立分支，不共享可写工作目录。
- 分支格式：Task/ED-<编号>-<PascalCase主题>；PR 标题包含 TaskId 与任务短名。
- 基准提交在分配时记录。一个任务的所有修改必须可由该基准与 HeadCommit 的 diff 复核。
- 禁止直接向集成分支提交、强推覆盖他人提交、跨任务拷贝未审查代码，或在其他 Agent 的 worktree 中解决冲突。
- 一个文件在同一时段只能有一个写 owner。共享合同、根构建定义、公共索引、依赖锁和发行配置采用模块地图中的单写者队列。
- 无远端 PR 能力时，使用与 PR 等价的本地 ReviewBundle，并明确标记为本地审查；不得声称已经创建或合并 GitHub PR。
- 本地 ReviewBundle 只用于仓库/控制器的 bootstrap 阶段。开始大规模产品实现前，必须配置正式 Git 远端、集成分支、PR 平台和所需分支保护/检查；之后产品 PR 不得用本地包替代远端审查。

## PR 内容与审查

PR 必须包含：

- TaskId、TaskRevision、目标、关联 Epic、基准提交和依赖 PR。
- 本次新增/修改/删除路径及每项删除的理由和数据影响。
- Acceptance 到提交/测试/检查结果的逐项映射。
- 新增合同/行为、兼容影响、规则偏离、未解决问题和后继任务。
- 实际执行的命令、目录和退出码；未执行项明确写明原因。

审查者须与作者分离，对固定 BaseCommit/HeadCommit 审查。审查检查：写集、模块边界、API 简洁性、身份/授权、安全、错误与取消语义、并发/生命周期、测试质量、日志脱敏、命名与格式，以及任务报告是否和 diff 一致。

Finding 必须包含严重级别、文件/符号/行或可复现证据、影响、修复验收条件。作者逐条回复，修复后只重看变化部分和受影响验证；安全、合同、持久化或平台副作用的 Finding 必须由独立 Reviewer 明确关闭。

审查级别：

- Blocker：安全/数据破坏、权限绕过、身份伪造、写集越界、丢失数据、接口不兼容且未批准。不得合并。
- Major：主要验收失败、无法恢复的错误/取消、无效状态转换、核心路径缺少验证。不得合并。
- Minor：局部可维护性或非阻塞表现问题。修复或建立明确的 FollowUpTask 后方可按风险决定。

## 合并队列与冲突处理

IntegrationOwner 是集成分支和合并队列的唯一写 owner。合并顺序按合同、依赖图、数据迁移顺序和风险确定；不以谁先完成来覆盖依赖。

发现冲突时：

1. 暂停冲突 PR 的合并，固定两边 BaseCommit/HeadCommit 和冲突文件清单。
2. 判断冲突属于合同先后、同一文件重叠、上游基准过期还是双方语义不一致。
3. 只让相关作者/模块 owner 提供意图、兼容要求和行为证据；消息不改变对方任务。
4. 由 IntegrationOwner 在集成分支解决纯文本/机械冲突；语义冲突必须由合同 owner 决策，必要时建立 ChangeRequest 或合并任务。
5. 冲突解决结果形成独立 merge commit 或可追溯 PR 更新；原作者不得把对方改动覆盖掉。
6. 独立 Reviewer 检查冲突文件、最终 diff 与依赖行为；只运行这些变更受影响的目标。
7. 合并完成后记录最终 commit，并通知依赖它的活动任务更新基准。

若两个 PR 写集重叠，默认不并行写。只有明确的不同文件范围和路径级锁定才能并行；如果无法可靠隔离，拆成先后任务。不能让多个 Agent 在同一文件上“边写边协调”。

## 合并门

1. CI/Review diff 全部落在 WriteSet 或获批 ChangeRequest 新增的路径内。
2. 分支基准足够新；目标分支保护规则与 required checks 通过。
3. 所有 Acceptance 都有可定位证据；没有把未跑的验证写成通过。
4. MUST 规则已有自动检查或指定独立签字人，且该门实际通过。
5. Blocker/Major Findings 已关闭；未关闭的 Minor 有责任人、任务 ID 和决定依据。
6. API/数据/权限/安全/平台副作用变更已经对应合同版本和独立审查。
7. 删除与迁移具有备份/回滚或明确的数据保留方案。
8. PR 摘要、TaskClosed 事件、合并提交和 CurrentState 更新彼此一致。

全量测试不是每个小 PR 的默认门；按任务影响范围执行定向检查。阶段集成和发行候选才执行阶段/发布级验证。

## PR 摘要约定

Workflow/TaskReportTemplate.md 是 PR 描述格式模板，复制到 PR 中填写，不为每个任务另存一个 Markdown 文件。原始命令事件与完整变化由结构化事件记录和 Git 保存。
