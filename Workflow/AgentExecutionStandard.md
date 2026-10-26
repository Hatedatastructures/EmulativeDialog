# AI Agent 执行与长任务治理规范

本规范管理开发期 Agent 对大型代码库进行长期重写时的任务、上下文、并行协作、纠错、审查和留痕。EmulativeDialog 发布运行时与开发期 Agent 是两个不同系统；本规范只约束开发流程。

## 一、治理目标与规则等级

把“Codex 级重写”作为长期项目管理，不把上游代码行数直接变成任务数量，也不要求每个小改动配一篇 Markdown。项目计划按“项目目标 → 发布/阶段 → Epic → 能力切片 → PR 任务 → 检查点”逐层细化；只有准备进入实施的阶段才展开具体任务。

规则分为三类：

- **MUST**：身份与权限、安全和隐私、已冻结合同、C++ 运行时边界、任务写集、分支隔离、独立审查、数据保护。不得由执行 Agent 自行豁免。
- **SHOULD**：模块拆分、Boost 选型、可维护性做法、默认验证范围。可提出有证据的偏离，由规则 owner 审核并记录回退方案。
- **MAY**：不改变外部行为的局部实现选择。由任务 owner 在写集内决定。

规则优先级：安全/数据不变量 > 已批准的版本化合同 > 已冻结任务范围与验收条件 > SHOULD 默认 > MAY 选择。发生冲突时先停下冲突路径、保留独立工作，并提交变更请求；不得静默改目标或降低验收条件。

## 二、少量常驻资料与按需加载

### 资料分层

| 层级 | 资料 | 加载规则 |
|---|---|---|
| 常驻入口 | 根 AGENTS.md、Workflow/CurrentState.md、Workflow/TaskIndex.yaml | 每个新任务只读一次；入口必须短，只负责路由与当前状态 |
| 任务包 | 当前任务记录、Architecture/ModuleMap.yaml 中当前模块条目、任务列出的合同/决策 | 只读当前任务及明确依赖项 |
| 实现上下文 | 被写入的源文件、直接调用方、相关测试与必要构建配置 | 按任务 ReadSet 和具体符号按需读取 |
| 历史档案 | 已关闭任务、旧 PR、已解决沟通、阶段事件日志 | 仅在当前任务引用具体记录时查询，不整批注入上下文 |

TaskIndex 是定位索引，不是任务正文。任务定义按阶段或模块放入分片清单，当前任务通过 TaskId 查询；不得把全部任务、全部阶段计划、历史事件或聊天转录载入上下文。任务完成后保留结构化记录，不复制成逐任务 Markdown 报告。

### 加载顺序

1. 读取常驻入口的必要字段，不重复阅读完整项目目标。
2. 通过 TaskId 从当前分片取出一个不可变任务版本。
3. 读取 ModuleMap 中该模块的 owner、写路径、合同入口和依赖，不加载其他模块地图正文。
4. 阅读任务明确列出的合同、ADR 和平台证据。
5. 在实现前只读取理解改动所需的源码、直接调用方和相关测试。
6. 若范围不足，先查询符号/路径索引；仍不足时按“上下文扩展”流程增加精确路径。

任务记录必须区分 ReadSet、OnDemandSet 和 ForbiddenSet。ReadSet 写具体文件、合同 ID 或符号，不得用整个仓库、整个 Source 目录或宽泛 glob 代替。目录只有在任务本身是目录盘点时才能列入，并须说明盘点目的。

每次扩大读取范围，记录 ContextExpanded 事件：TaskId、路径/符号、提出者、必要原因、关联的验收项，以及是否影响写集。仅仅发现“附近还有很多代码”不是扩大范围的理由。超出本任务范围的缺陷先发 FollowUpProposal。

### 上下文预算

- 常驻入口资料的设计目标不超过可用上下文的 10%；任务说明、模块地图摘录和合同的合计目标不超过 25%。
- 实现开始后至少保留一半可用上下文给当前源代码、修改方案、验证证据和交接余量。
- 默认 ReadSet 最多 12 个文件。超出时先拆任务、收窄接口或建立可复用的模块摘要；确有必要可由任务 owner 记录例外与理由。
- 必须读取的合同不能为了节省上下文而跳过。若必读资料超预算，应拆任务或由合同 owner 提供有来源的窄范围摘录。
- 上述预算是启动检查和治理工具的目标。工具尚未实现自动测量前，Agent 必须在任务记录中估算并披露超限，不得声称已经自动强制。

任何情况下都不得为方便而读入全部源树、完整历史、所有任务卡或全部事件日志。存在 CodeGraph 时使用其符号/调用路径查询；没有索引时使用精确路径、符号和调用方的定向检索。

## 三、从大型重写拆成可集成的工作

### 分解层级

- **项目目标**：用户要交付的完整产品行为和不变量。
- **阶段/发布**：具有明确退出门的长期里程碑。
- **Epic**：一个可独立推进的业务/技术领域，可持续多个阶段。
- **能力切片**：一条可演示、可验证的纵向行为，或一个必须先冻结的公共合同。
- **PR 任务**：单一 owner、单一工作树、明确写集、能独立审查和合并的最小完整改动。
- **检查点**：长任务内的进度快照，不是新任务，不额外创建一篇 Markdown。

Codex 上游即使达到数百万行，也不按行数、文件数或子目录机械切成数千任务。先锁定上游提交和源码范围，再按包/子系统、公开行为、运行边界和依赖关系制作能力地图。每个相关能力标记为保留、重设计、移除、暂缓之一，映射到新模块、合同、验收证据和来源范围。只有被当前切片需要的代码才进入上下文；不做无目标的逐行翻译。

Codex 聊天 Agent 核心重写范围按 ADR-0002 执行，不由每个任务重新解释。不得把 CLI、IDE、宿主命令、主机状态或其他已排除工具擅自加入聊天 Agent。能力地图是实际 C++ 重写的追踪表：映射到目标模块后必须生成实现任务；“调研完成”不能代替 C++ 实现，也不能关闭该能力。

### PR 任务切分规则

一个 PR 任务应满足：

- 有一个明确的用户可见或模块合同结果。
- 有一个主要模块 owner 和互不重叠的 WriteSet。
- 能写出可判定的验收条件和针对性验证。
- 变更能由独立审查者在一个 PR 中完整理解。

以下情况必须拆分或先设合同任务：同时改变多个独立模块的公共 API；同一任务跨多个 Provider/Channel 且实现可独立；验收无法用同一组证据判断；需要同时做产品决策、协议设计和大规模实现；写集与其他 Ready 任务重叠。

不要为了缩小数字把强耦合改动拆成大量空 PR；也不要把一个长期 Epic 直接派给单个 Agent 并称为一个任务。简单、同一行为、同一 owner 的小改动可以合并在一个 PR 任务中。

### Codex 源码映射

在开始有意义的 C++ 迁移前，必须固定上游仓库、提交、许可证边界和相关构建/包结构。能力地图至少追踪：上游子系统或公开行为、决策（保留/重设计/移除/暂缓）、目标模块、合同版本、依赖任务和验收证据。

地图的完整度按“与目标聊天产品相关的上游能力是否都有决策”衡量，不按读过多少行源码衡量。未映射能力不能被 Agent 默认为必需或不需要；属于聊天产品之外的能力须有明确范围决定。上游升级到新提交时，新增/变化的模块进入差异盘点，不重扫无关历史。

## 四、长任务跨会话续作

每个活动任务都必须能在没有原聊天记录的情况下恢复。检查点保存：

- TaskId、版本、Epic、owner、工作树、分支、基准提交和当前提交。
- 当前验收项及状态；已完成的行为和引用证据。
- 未完成项、当前失败或不确定点、已尝试方案及结果。
- 下一条具体动作、所需文件/符号、阻塞原因和待回复消息 ID。
- 最近一次定向验证及其退出结果。

检查点只记录事实和可执行下一步，不保存完整 Prompt、模型内部推理或重复的源代码。上下文压缩/新会话恢复时先读当前任务检查点和当前差异，再按需打开文件；不从头重读全部计划与聊天历史。

检查点触发时机固定为：每个验收项完成后、上下文即将压缩/会话即将结束时、遇到 Blocker 时、任务 owner 或分支基准变化时、PR 提交前。普通命令和每次小编辑由事件记录器追踪，不要求 Agent 每一步都写一篇进度说明。

任务规格在 Ready 时冻结为版本。任何目标、验收、WriteSet、合同版本或风险等级变化，都产生新 TaskRevision 和 ChangeRequest；旧版本与其证据保留。任务 owner 不得通过重写验收条件让失败变成通过。

## 五、六个稳定 Agent 槽与并行

- 项目固定六个逻辑工作槽：Worker01（Build/Workflow 支持）、Worker02（AgentRuntime/Core）、Worker03（OpenAI Responses）、Worker04（Chat Completions）、Worker05（Anthropic Messages）、Worker06（Memory/Context 支持）。Worker02-Worker05 是当前四个并行产品实现槽；Worker01 与 Worker06 在 P1 作为支持槽。Worker06 保留 Memory/Context 职责，待合同和依赖冻结后可由 Coordinator 分派独立实现任务。
- Coordinator 单独负责任务分派、公共合同和集成，不计入六个槽，也不与实现者共享写集。Reviewer 必须与作者不同。
- 有效并行数为 min(6, 宿主可用容量, Ready 且互相独立的任务数)。空闲槽保持空闲；不得为了填槽制造重复分析、无写集或不满足依赖的任务。
- Workflow/AgentPool.yaml 是稳定槽位注册表。相关 PR 完成后复用同一职责槽，不因小任务反复创建或关闭 Agent；可靠状态保存在 TaskCheckpoint，不依赖会话记忆。
- 每个任务先冻结 TaskId/Revision、BaseCommit、精确 ReadSet/WriteSet、依赖、验收、验证命令和 Reviewer，再分配独立 branch/worktree。每个可审查模块切片按节奏提交，提交应对应真实工作。合同、根构建、依赖锁、TaskIndex 和集成分支等共享资源由单一 owner 排队修改。
- Agent 只在已分派任务内工作。沟通消息可以报告发现或建议，但不能改变 Goal、Acceptance、WriteSet、依赖或优先级；范围变化走 ChangeRequest。

完整分派、PR、审查和冲突规则见 [Workflow/MultiAgentCollaborationStandard.md](MultiAgentCollaborationStandard.md)。

OpenAI 官方多 Agent 指南建议把并行用于真正独立的工作，给每个子任务清晰问题与预期结果，并协调会修改同一文件的工作。本项目在此基础上强制使用独立 worktree、冻结写集、独立 Reviewer 和集成队列：
https://developers.openai.com/api/docs/guides/agents-api/multi-agent

## 六、Agent 间沟通与任务边界

沟通通道与任务规格分离。Agent 可以询问、报告发现、提出接口建议、提交阻塞或交接信息；消息本身不是任务指令，不能改变收件人的 Goal、Acceptance、WriteSet、优先级或依赖。

每条 CoordinationMessage 至少包含：

- MessageId、TaskId、TaskRevision、SenderAgentId、RecipientAgentId/Role、时间和消息类型。
- 一句话主题、事实/问题/建议、可定位证据（路径、符号、行号或命令结果）。
- 对当前任务的影响、紧急度、希望的回复/决定和关联 ChangeRequest。

允许类型：Question、Finding、Proposal、Blocker、Handoff、Acknowledgement、DecisionLink。避免在工作消息中复制整段会话或无关日志。消息按 TaskId 和状态过滤；默认只加载当前任务的未解决消息，关闭消息进入只读归档。

跨模块建议必须同时发给协调者/接口 owner。协调者确认影响后可：

1. 记录为不改变原任务的建议，原 Agent 继续原范围；
2. 建立独立 FollowUpTask；
3. 创建 ChangeRequest，经有权 owner 批准后发布新任务修订；
4. 因安全、数据或合同风险暂停受影响部分。

除明确批准的 ChangeRequest 外，接收消息的 Agent 不得改对方任务文件、工作树或分支。发现严重风险时应立即上报；上报不等于自行改写范围。

## 七、自迭代、纠错和阻塞处理

标准循环：

1. **选取**：协调者从阶段门和未完成能力地图中产生候选任务，检查依赖、写集和验收。
2. **冻结**：任务规格版本化，确定 owner、分支、ReadSet、WriteSet、ForbiddenSet、合同版本和验证计划。
3. **实施**：Agent 在隔离工作树完成一个纵向能力切片，及时记录检查点。
4. **定向验证**：只运行任务合同要求和本次改动受影响的目标。
5. **独立审查**：Reviewer 对固定的 BaseCommit 与 HeadCommit 检查范围、行为、安全和证据。
6. **纠错**：作者在原写集内修复 Finding，逐条回应并重跑失败目标。
7. **集成**：按依赖与合并队列合并 PR，处理冲突后审查冲突结果。
8. **更新计划**：协调者更新能力地图、当前状态和下一批 Ready 候选；不能只凭 Agent 自述关闭任务。

失败处理：

- 先保存完整错误、命令、目录、退出码和相关提交。
- 分类为实现缺陷、环境/工具缺失、合同错误、外部平台限制、任务范围未决或上游变化。
- 实现缺陷只在当前写集内修复，只重跑失败或受影响目标。
- 代码修复连续三次仍由同一根因失败，必须停止循环，创建 Blocker 并给出证据、已尝试方案和决策选项。
- 遇到权限、数据破坏、外部副作用不确定、合同不一致或用户选择缺失时立即暂停该受影响路径；不必停止无依赖的其他任务。
- 禁止删除/跳过失败测试、放宽安全检查、伪造回执、降低验收条件或用整仓重跑掩盖根因。

自动化纠错只能在已授权的 TaskSpec、WriteSet 和策略内进行。Agent 可以生成 FollowUpProposal；Coordinator/Owner 决定是否入队。Agent 不得自行修改治理规范或批准自己的 ChangeRequest。

## 八、任务轨迹与上下文外的持久化

Git branch、commit、PR diff 是文件变化的事实来源，不用全仓 Hash 代替差异审查。协作系统以分片 JSONL/结构化状态记录事件，按 ReleaseId、PhaseId、TaskId 查询；事件档案默认不进入模型上下文。

至少记录以下事件：

- TaskCreated / TaskRevisionApproved / TaskAssigned / TaskStarted / CheckpointSaved。
- ContextLoaded / ContextExpanded，记录精确路径或符号及理由。
- CommandExecuted，记录命令、工作目录、退出码、关联任务和执行者。
- FileChangeObserved，以 Git diff 为准记录 Added / Modified / Deleted 路径。
- AgentMessageSent / AgentMessageResolved。
- ReviewFindingAdded / ReviewFindingResolved / PRCreated / PRMerged。
- TaskBlocked / TaskResumed / TaskClosed，记录证据和责任 owner。

每条事件至少带有 EventId、EventType、OccurredAt、ReleaseId、PhaseId、TaskId/TaskRevision、AgentId、Branch、CorrelationId、Result 和 EvidenceRefs。命令事件还记录脱敏后的命令、工作目录、退出码、耗时和输出文件位置；文件变化事件记录 Added/Modified/Deleted 路径并关联 Git commit。事件只追加，不覆盖；修正错误记录时追加 Correction 事件引用原 EventId。

事件记录不保存密钥、原始聊天正文、完整 Prompt 或模型内部推理。命令和环境记录先脱敏，不能把密钥放进诊断参数、环境快照或错误输出。删除必须记录精确路径、原因、数据影响和备份/恢复信息。当前状态页只保留本阶段摘要、阻塞与下一步；详细轨迹留在事件存储和 GitHub/本地 PR 记录。

任务完成时用 PR 描述和结构化 TaskClosed 事件记录结果，不为每个小任务另写 Markdown。阶段结束时再生成一份面向人的阶段总结，避免每次迭代重复维护多套说明。

## 九、可执行门槛

MUST 规则必须在 Workflow/EnforcementMatrix.yaml 中对应到自动检查或明确的独立签字人、证据和阻塞阶段。未被自动化的规则须标记 ManualGate 和责任角色；不得声称“规范保证”但没有检查入口。实施阶段开始前，所有影响安全、写集、合同、分支隔离和 PR 的规则都必须有可执行门。 MAN-003 为 P1 阻塞 RuleId 提供逐项人工控制、证据、独立审查与 Coordinator 签署栏；矩阵和文档本身不构成签署，P1 ExitGate 在 Coordinator 评估前保持 Pending。

验证遵循任务级最小集合：新增/改变行为要有对应合同或行为验证；纯文档变更不运行 C++ 构建；阶段集成再执行阶段级检查。失败只重查失败目标，发行候选才执行完整发布检查。

### 控制器实施门

Markdown 和 YAML 能定义规则，不能独自强制任务状态、上下文边界、工作树隔离或事件留痕。进入大规模产品实现前，必须先交付 C++ 开发期 AgentWorkflowController，并通过自己的合同与故障恢复验收。该工具不属于聊天产品运行时，也不能自行审批产品决策。

控制器的最小能力：

- 按 TaskId 查询一个阶段分片中的任务，不把其他任务送入 Agent 上下文。
- 校验 TaskRevision、状态转换、依赖、ModuleId、ReadSet、WriteSet、共享锁和审批事件。
- 生成限于任务 ReadSet 的上下文包；额外读取要由 Agent 发起并记录原因。
- 创建/核验隔离 Git worktree 和分支，比较 PR diff 与批准 WriteSet。
- 维护稳定 Agent 槽和任务队列，通过当前可用且受支持的 subagent 接口分发；不依赖 Codex 私有未文档化接口。
- 保存跨会话检查点、追加脱敏事件、按 TaskId 过滤沟通消息。
- 只执行 TaskSpec 明列的验证命令，记录实际命令、工作目录、退出码和证据路径。
- 用 PR 平台适配器跟踪 Reviewer、Finding、MergeQueue 和最终提交；没有远端 PR 时输出明确标记的本地 ReviewBundle。

控制器无法维持底层 Agent 进程或会话时，保留稳定的 AgentRole/AgentId 逻辑槽，并从检查点恢复工作；不得为保持“在线”而虚报原会话仍在运行。协调者、状态库和集成队列在控制器未完成前可人工操作，但相关 P0/P1 任务不得声称这些 MUST 规则已自动强制。
