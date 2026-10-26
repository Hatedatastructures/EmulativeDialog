# 当前状态

日期：2026-10-26

## 仓库证据
- 根路径：H:/Qbot/EmulativeDialog。
- 原始 `main` 仍是 unborn branch，原工作区中的项目目录仍未跟踪。为保护它并提供隔离基线，协调者从只读 workspace snapshot 建立本地 bootstrap commit `6e9b727ab88ae910d70069d9b9f19a9912f2bcb4`；当前集成工作树是 `codex/integration`，原工作区未切换或覆盖。
- 远端 `origin` 指向公开 GitHub 仓库 `Hatedatastructures/EmulativeDialog`；已发布 `codex/integration` 和 ED-0006 任务分支。创建 PR 时 GitHub connector 返回 `403 Resource not accessible by integration`，`gh auth status` 也显示 CLI 未登录。因此 ED-0006 按 bootstrap 规则使用已批准的本地 ReviewBundle，不声称有远端 PR。
- ED-0006 已交付并本地合入 Windows 原生 C++20 BuildProbe（merge `71bed25a257b018f1e2a8c6e34ca751133135f30`），post-merge configure/build/CTest 全部通过并获独立审查批准；远端 PR 未创建，原因见仓库/PR 权限记录。ED-0007 四份 V1 合同已修正、独立审查通过并本地合入（merge `7745fb1019c6a7afdd8b54f89a9894568fbf38a0`）；Provider/Memory 实现现在可依赖冻结合同，但产品 AgentRuntime 切片仍等 P1 的 Controller gate。ED-0008 首次复审发现 BaseCommit 对应的任务记录未包含冻结的 CMakeLists 写集；CR-ED-0008-002 已批准将 Base 改为 b0f5defc2cb14c3e066cd9f7d9ed71bb80a87042，Worker01 已将 ED-0008 重基到 Revision 3；Coordinator 发现 R3 ReviewBundle 混入 R2 Validation command，正修正精确 TaskRevision 过滤并生成 R4 包。R2 包保留。ED-0009 Responses R2 夹具已独立复审通过并本地合入（merge `c17a39ce3e4ff3904c7fb4a00e715adedf99d183`）；ED-0010 Chat Completions 复审发现一个 P2 fixture 歧义，Worker04 正在原写集修正；ED-0011 Anthropic 夹具已提交 Worker03 独立复审，ED-0012 R2 已将 envelope 限于 Core-only SelectionInputs，独立复审通过并本地合入（merge `b06fb0e86a9d1160fa3a212661ca748cccaf9ed9`）；这些任务均不写产品 C++ 实现或共享接口。
- 长任务控制入口为 Workflow/TaskIndex.yaml；任务按阶段分片，Agent 不逐项加载全计划，也不为每个小任务创建 Markdown。
- 多 Agent 任务要求独立分支/worktree、冻结写集、独立 Reviewer、协调者单写状态、PR 集成队列和结构化事件轨迹。EnforcementMatrix 中未实现门全部标为 Planned。
- P0 的 ED-0001 上游能力映射与 ED-0005 模块/治理门均已通过独立审查并合入本地 `codex/integration`；CodexCorePreparationGate 已通过。P1 的 ED-0006 build 与 ED-0007 四份合同均已独立审查并合并；ED-0008 Revision 3 的 Base/WriteSet 已对齐；当前修正 ReviewBundle 的跨 TaskRevision 证据过滤，完成后交 Worker02 对 R4 固定 Base/Head 复审。P1 ExitGate 通过前不启动产品 AgentRuntime 实现。QQ/TUI 仍保留独立领域门。
- Workflow/AgentPool.yaml 将 Worker01–Worker06 保持为用户指定的稳定职责；Worker01 正在修正 ED-0008 ReviewBundle 的 R2/R3 Validation 过滤，Worker02 已报告 R2 Base/WriteSet Major 并继续 fixed-commit 只读审查，Worker03/04/05/06 分别并行负责 Responses、Chat Completions、Anthropic、Memory/Context 夹具任务；ED-0009 已 Merged，ED-0010 在 P2 修正中，ED-0012 R2 Major 已独立复审关闭，夹具任务通过 AC-1..AC-4 并待集成。后续继续复用逻辑身份，不虚报已完成 Agent 会话仍在运行。
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
3. ED-0008 完成 R4 Bundle 证据过滤和固定 Head 复审；P1 ExitGate 通过前不启动产品 AgentRuntime 实现。
4. P1 通过后登记 ED-0101–ED-0115 的正式 TaskSpecs，再完成 Mock 消息 → Core policy → C++ AgentRuntime → FakeProvider → MockChannel 的可编译行为切片。
5. QQ 官方/授权接入与 TUI 参照分别按 P4/P5 推进；TUI 不阻塞 Agent 核心，微信继续 Deferred。

## 验证账本
已存在 Windows 原生 C++20 `BuildProbe` 与 C++ WorkflowController 目标；ED-0006 post-merge configure/build/CTest 通过（1/1），ED-0008 Revision 3 在 Base b0f5def 上 configure/build/CTest 通过（2/2），但 R3 Bundle 仍含 R2 diff-check 证据，正在修正过滤并将在新 Head 上复验。这些是构建和开发工具证据，不是产品 AgentRuntime 行为。尚无产品 AgentRuntime 行为和真实 QQ 收发证据。微信本阶段不做收发。平台调研摘要见 Research/Platforms/PlatformCapabilityMatrix.md。
