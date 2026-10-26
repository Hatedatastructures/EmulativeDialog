# 当前状态

日期：2026-10-26

## 仓库证据
- 根路径：H:/Qbot/EmulativeDialog。
- 原始 `main` 仍是 unborn branch，原工作区中的项目目录仍未跟踪。为保护它并提供隔离基线，协调者从只读 workspace snapshot 建立本地 bootstrap commit `6e9b727ab88ae910d70069d9b9f19a9912f2bcb4`；当前集成工作树是 `codex/integration`，原工作区未切换或覆盖。
- 远端 `origin` 指向公开 GitHub 仓库 `Hatedatastructures/EmulativeDialog`；已发布 `codex/integration` 和 ED-0006 任务分支。创建 PR 时 GitHub connector 返回 `403 Resource not accessible by integration`，`gh auth status` 也显示 CLI 未登录。因此 ED-0006 按 bootstrap 规则使用已批准的本地 ReviewBundle，不声称有远端 PR。
- ED-0006 Windows 原生 C++20 BuildProbe 与 ED-0007 四份 V1 合同均已独立审查并本地合入；GitHub PR 创建被 connector 的 403 权限阻止且 CLI 未登录，未声称已建 PR。ED-0009 Responses、ED-0010 Chat Completions、ED-0011 Anthropic Messages、ED-0012 Memory/Context 离线夹具均已独立审查并本地合入；ED-0010 merge 为 5355df76c87519d1730a4ff30ef635b22f9008ad，ED-0011 merge 为 3f63b920ddab43d73ee0ef5db381d8911a9f40ac。这些夹具任务不包含 C++ 实现，也不解除 P1 ExitGate。
- 长任务控制入口为 Workflow/TaskIndex.yaml；任务按阶段分片，Agent 不逐项加载全计划，也不为每个小任务创建 Markdown。
- 多 Agent 任务要求独立分支/worktree、冻结写集、独立 Reviewer、协调者单写状态、PR 集成队列和结构化事件轨迹。EnforcementMatrix 中未实现门全部标为 Planned。
- ED-0008 R4 尚未获批。Worker02 固定检查 Base b0f5defc2cb14c3e066cd9f7d9ed71bb80a87042 → Head 35186cb8e5b56942f8127f2a241f1b21f909eadb 时发现 Base 中只有 ED-0008 TaskRevision 1，缺少 R3 使用的预算覆盖、worktree 字段和验证命令；R4 validations 的 evidence_path 为空，并发现脱敏顺序、事件路径约束和 Review 状态 bundle 路径等 Major 问题。R4 ReviewBundle 不可批准；Worker02 正汇总正式 findings，Worker01 已提交最小 ChangeRequest 提案。P1 ExitGate 通过前不启动产品 AgentRuntime 写代码。
- Workflow/AgentPool.yaml 保持 Worker01–Worker06 六个稳定职责；Worker01 准备 ED-0008 作者回应，Worker02 独立复审，Worker03/04/05 并行准备各自 Provider C++ 实现 TaskSpec，Worker06 准备 Memory/Context TaskSpec。只读设计工作不替代正式 TaskSpec 注册和实现任务；后续继续复用逻辑身份，不虚报已完成 Agent 会话仍在运行。
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
3. 处理 ED-0008-RV-003..RV-007 Major findings，按 ChangeRequest 冻结新 TaskSpec/Base，再用新固定 Head 重建 ReviewBundle 并独立复审；P1 ExitGate 通过前不启动产品 AgentRuntime 实现。
4. P1 通过后登记 ED-0101–ED-0115 的正式 TaskSpecs，再完成 Mock 消息 → Core policy → C++ AgentRuntime → FakeProvider → MockChannel 的可编译行为切片。
5. QQ 官方/授权接入与 TUI 参照分别按 P4/P5 推进；TUI 不阻塞 Agent 核心，微信继续 Deferred。

## 验证账本
已存在 Windows 原生 C++20 BuildProbe 与 C++ WorkflowController 目标；ED-0006 post-merge configure/build/CTest 通过（1/1）。ED-0008 Head 35186cb8e5b56942f8127f2a241f1b21f909eadb 的 configure/build/CTest（2/2）与 diff check 均退出码 0，但固定 Base 的 TaskSpec 不支持该 Revision/命令，且审查发现多个 Major finding，因此这些运行结果不能作为 ED-0008 验收通过。ED-0009..0012 fixture JSON parse 和合并 diff check 通过。这些是构建/开发工具与离线协议数据证据，不是产品 AgentRuntime 行为。尚无产品 AgentRuntime 行为和真实 QQ 收发证据；微信本阶段不做收发。
