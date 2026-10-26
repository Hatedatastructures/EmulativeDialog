# 当前状态

日期：2026-10-26

## 仓库证据
- 根路径：H:/Qbot/EmulativeDialog。
- 原始 `main` 仍是 unborn branch，原工作区中的项目目录仍未跟踪。为保护它并提供隔离基线，协调者从只读 workspace snapshot 建立本地 bootstrap commit `6e9b727ab88ae910d70069d9b9f19a9912f2bcb4`；当前集成工作树是 `codex/integration`，原工作区未切换或覆盖。
- origin 指向公开 GitHub 仓库 Hatedatastructures/EmulativeDialog。最新已推送的协调基准为 10e23ac736be5403c74848e2bf6aa2e62ee7fcae；本地 integration 已记录 R5 审查并合入 ED-0008，远端推送将在该事件/状态提交完成后进行。创建 PR 时 GitHub connector 返回 403 Resource not accessible by integration，gh CLI 未登录；未声称已创建远端 PR。
- ED-0006 Windows 原生 C++20 BuildProbe 与 ED-0007 四份 V1 合同均已独立审查并本地合入；GitHub PR 创建被 connector 的 403 权限阻止且 CLI 未登录，未声称已建 PR。ED-0009 Responses、ED-0010 Chat Completions、ED-0011 Anthropic Messages、ED-0012 Memory/Context 离线夹具均已独立审查并本地合入；ED-0010 merge 为 5355df76c87519d1730a4ff30ef635b22f9008ad，ED-0011 merge 为 3f63b920ddab43d73ee0ef5db381d8911a9f40ac。这些夹具任务不包含 C++ 实现，也不解除 P1 ExitGate。
- 长任务控制入口为 Workflow/TaskIndex.yaml；任务按阶段分片，Agent 不逐项加载全计划，也不为每个小任务创建 Markdown。
- 多 Agent 任务要求独立分支/worktree、冻结写集、独立 Reviewer、协调者单写状态、PR 集成队列和结构化事件轨迹。EnforcementMatrix 中未实现门全部标为 Planned。
- ED-0008 R4 被 Worker02 以 RequestChanges 拒绝；R5 固定 S/A/C/B/H 复审现为 Approve/Verified，RV-003..RV-010 已关闭。Coordinator 更正事件 C=`10e23ac736be5403c74848e2bf6aa2e62ee7fcae` / EventId=`ba850a30-380b-4ae2-9a12-dc1ea9f40d79` 只修正ApprovalReason到冻结TaskSpec原文；13项ReadSet与MaxReferenceFiles=13不变。R5 Bundle 长84,354字节、SHA256 `035567CB622AE33B9F33C592637E2FDC05D696E58D9A7B016289784D880CDD63`。四条固定验证均退出码0，完整CTest 2/2。两次bundle失败按各自Head记录为历史；ED-0008-RV-011是非阻断的P2 selector follow-up。固定H已通过无冲突预演并本地合并为 `4eea6819c0bf6091a4c0119b74b95abb49157c41`；远端PR未创建，集成提交待推送。P1 ExitGate尚待Coordinator核验Manual/Automated Enforcement Matrix证据，产品AgentRuntime代码尚未开始。
- 六个稳定 Agent 会话当前均在运行，但 P2 尚未扩展，所以本段合同/依赖审查仍是候选准备，不构成正式写入任务分配；Workflow/AgentPool.yaml 的形式化槽状态须在P2展开时由Coordinator追加槽位事件再同步。Worker01保留BuildAndDependencyOwner槽，负责ED-0008集成并准备ED-0119；Worker02保留固定R5 Reviewer/Core槽并准备ED-0117/ED-0109；Worker03准备ED-0116与Provider CR；Worker04审阅ED-0116/ED-0105；Worker05审阅Anthropic与Provider CR；Worker06准备Storage/Memory/Context依赖。当前没有P2实现任务标Ready或开始写产品C++。
- Architecture/ModuleMap.yaml 是分片索引；详细 owner 与写路径按域存于 Architecture/Modules/。
- `Workflow/CodexCapabilityMap.yaml` 已锁定 `openai/codex@970111f6cb62cc02fe36a82ea7d71b37a2889c97` 与 Apache-2.0 行为参考边界；24 项能力已分类，15 项 Keep/Redesign 具备拟议 TaskId。正式 P2/P3 TaskSpecs 须在实现分配前登记。TUI 参照库仍未锁定，且不阻塞 Agent 核心。
- 任何阶段不得把本状态描述为“已实现”或“已构建”。

## 当前产品解释
- 主任务已由 ADR-0002 固定为：用 C++ 重实现 Codex 聊天 Agent 核心，完成标准覆盖所有批准的 Agent 能力，而非包装 Rust Codex。
- CLI/IDE UI、Shell/主机命令拦截、主机状态查询、任意文件工具等不属于当前聊天 Agent；新增或恢复必须走 ChangeRequest。
- OpenAI Responses、OpenAI-compatible Chat Completions、Anthropic Messages 三协议并列。
- 当前渠道优先级为 QQ；微信 Deferred，不进入本阶段调研或实现。
- C++ TUI 自研，采用 PascalCase 代码/文件命名；产品 memory 与开发任务账本分开。

## P0 未决项
- 见 Workflow/OpenDecisions.md。
- QQ 是否是 QQ Bot、个人 QQ 或其他明确授权的模式。
- 微信账号面留待未来阶段重新开启；当前不属于 P0 未决任务。
- Codex 上游基线和 24 项聊天 Agent 能力映射已由 ED-0001 锁定并审查；15 项 Keep/Redesign 已映射到拟议 TaskId，正式 P2/P3 TaskSpecs 会在实现阶段分片登记。
- QQ 账号面和授权接口仍待核验；微信保持 Deferred。
- TUI 必须先锁定一个 TypeScript 参照库及精确版本；目标 API 映射覆盖率为 100%，未批准的行为差异为零。JSX 字面语法与原生 C++ 语法的冲突须单独明确，不得借此减少组件能力。

## 下一步
1. P0 的 ED-0001 与 ED-0005 已通过独立审查并合入本地 `codex/integration`；P1 现已打开。
2. ED-0006 Windows C++20/CMake 基础和 ED-0007 Core/Provider/Memory/Channel 合同已通过独立审查并合并。
3. ED-0008已在固定R5审查通过后本地合入；Coordinator正在核对EnforcementMatrix中的关键MUST人工/自动执行路径，确认后再记录P1 ExitGate Passed。
4. P1 Gate通过后正式扩展P2并追加稳定槽位事件；按不重叠WriteSet推进RootBuild/ProviderPort/ChannelPort/WindowsPlatform/Storage/Core与Mock Context，随后完成Mock消息 → Core policy → C++ AgentRuntime → FakeProvider → MockChannel第一条可编译行为切片。
5. QQ 官方/授权接入与 TUI 参照分别按 P4/P5 推进；TUI 不阻塞 Agent 核心，微信继续 Deferred。

## 验证账本
已存在 Windows 原生 C++20 BuildProbe 与 C++ WorkflowController 目标；ED-0006 configure/build/CTest 通过（1/1）。ED-0008 R5固定H `df5c463ba45929da804b2c3c8a2ef21b25a3e13e` 的configure、指定targets build、完整CTest（2/2）和typed diff-check均退出码0；独立Reviewer Approve/Verified；R5 bundle SHA256=`035567CB622AE33B9F33C592637E2FDC05D696E58D9A7B016289784D880CDD63`。`codex/integration`本地合并提交M=`4eea6819c0bf6091a4c0119b74b95abb49157c41`，与H相比仅含协调状态/事件差异。P1 Gate等待EnforcementMatrix人工/自动路径最终核验。所有这些仍是开发工具证据，不是产品AgentRuntime行为。
