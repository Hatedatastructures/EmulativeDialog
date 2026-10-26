# 当前状态

日期：2026-10-26

## 仓库证据
- 根路径：H:/Qbot/EmulativeDialog。
- 原始 `main` 仍是 unborn branch，原工作区中的项目目录仍未跟踪。为保护它并提供隔离基线，协调者从只读 workspace snapshot 建立本地 bootstrap commit `6e9b727ab88ae910d70069d9b9f19a9912f2bcb4`；当前集成工作树是 `codex/integration`，原工作区未切换或覆盖。
- origin 指向公开 GitHub 仓库 Hatedatastructures/EmulativeDialog。`codex/integration` 已从协调状态提交 b4521757e3029b240eb896947a03f7075303bacb 推送到 4fb639d0eb11e68d52b73dff2ee0774455dd00ff，包含 ED-0008 经 R5 ReviewBundle 审查后的本地合并。GitHub connector 创建 PR 返回 403 Resource not accessible by integration，gh CLI 未登录；仅声称本地 ReviewBundle 与 integration merge，不声称远端 PR。
- ED-0006 Windows 原生 C++20 BuildProbe、ED-0007 四份 V1 合同和 ED-0008/R5 Controller 已独立审查并本地合入。ED-0014/R9 的 C++ 冲突恢复器固定 Head `0b9d31f0b896d0bfc2b5e9c365d217c93feb848f` 已审查并合入 `fc8b2628db8e750258b12f6eed6734aa9a9406f4`；ED-0019/R1 仅向 P1 EventShard 追加五条批准的 resolution，保留原始 175,936 字节前缀并经独立审查，合入 `acd4e55e8cd55c3358c697b22e2470c098642904`。ED-0015/R3 ModelProviderPort V1.1、ED-0016/R2 ChannelAdapterPort、ED-0017 V1.1 合同和 ED-0018/R2 MockChannel 测试替身已按独立 ReviewBundle 集成；它们仍不是 AgentRuntime 或真实 Provider/QQ 实现。GitHub PR 创建插件对多项请求均返回 403 `Resource not accessible by integration`，未声称创建远端 PR。
- 长任务控制入口为 Workflow/TaskIndex.yaml；任务按阶段分片，Agent 不逐项加载全计划，也不为每个小任务创建 Markdown。
- 多 Agent 任务使用独立分支/worktree、冻结写集、独立 Reviewer、单一 IntegrationOwner 和结构化事件；EnforcementMatrix 中 MAP-001 有 ManualGate，其余多数 P1 MUST 仍为 Planned。Worker05 独立审核认为 P1 ExitGate 仍缺 MAN-003 覆盖与已签署的 scoped manual 路径，当前保持 Pending。
- ED-0008/R5 固定 S/A/C/B/H 经 Worker02 Approve/Verified；RV-003..RV-010 已关闭，Controller configure/build/full CTest（2/2）通过。ED-0019/R1 的真实 P1 验证显示：未解决的 Base 上 `context ED-0006/R1` 因 EventId `378e2cbe-98e9-4419-8418-287ef16b7c74` 退出 1；应用五条 resolution 的 Head 临时副本同一命令退出 0 并仅在临时副本追加 ContextLoaded。P1 ExitGate 仍待 MAN-003 与 Coordinator 签署评估；产品 AgentRuntime 代码尚未开始。
- 六个稳定槽保持 Worker01-Worker06 身份。当前 Worker01 执行 ED-0013/R4 协作规范与 MAN-003；Worker02 独立 Reviewer；Worker03 执行 ED-0020 FakeProvider 测试替身；Worker06 为 ED-0020 Reviewer；Worker05 已完成 P1 Gate 独立审计；Worker04 暂无新的 Ready 独立任务。ED-0020 只改 Tests/ 中的离线 C++ 支持代码，不是 AgentRuntime。
- Architecture/ModuleMap.yaml 是分片索引；详细 owner 与写路径按域存于 Architecture/Modules/。
- `Workflow/CodexCapabilityMap.yaml` 已锁定 `openai/codex@970111f6cb62cc02fe36a82ea7d71b37a2889c97` 与 Apache-2.0 行为参考边界；24 项已分类，15 项 Keep/Redesign 仍须在 ED-0101..ED-0115 中登记完整任务合同。正式产品实现任务尚未 Ready；本轮只注册 ED-0020 测试替身。TUI 参照库未锁定但不阻塞 Agent 核心，微信继续 Deferred。
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
3. Worker01 按 ED-0013/R4 在六条冻结路径内完成正式协作规范和 EnforcementMatrix 的 MAN-003 P1 覆盖；Worker02 固定 Head 复审。GitHub PR/branch protection 的 403 环境限制须明确保留。
4. Worker03 按 ED-0020 实现离线 FakeProvider 测试替身，Worker06 独立审查；该测试支持完成后仍要先通过 P1 ExitGate，再登记 ED-0101..ED-0115 完整 TaskSpecs，然后开始 Core 身份/策略与 AgentRuntime 第一条产品纵向切片。
5. QQ 官方授权接入与 TUI 参照分别按后续 Epic 推进；TUI 不阻塞 Agent 核心，微信保持 Deferred。

## 验证账本
已存在 Windows 原生 C++20 BuildProbe 与 C++ WorkflowController。ED-0008/R5 configure/build/CTest（2/2）与 ED-0014/R9 严格 MinGW C++20 configure/build/Contract CTest（1/1）及覆盖率均有固定 Bundle。ED-0015/R3 ModelProviderPort C++20 contract executable 在集成工作树编译/运行退出 0；ED-0018/R2 MockChannel 合同测试在集成工作树编译、链接、运行退出 0，gcov 对 MockChannelAdapter.cpp 为 97/97（100%）。ED-0019/R1 真实 P1 事件冲突 Base/Head 上下文前后对照见本页及 Bundle。当前已集成的 Port/Mock 测试代码仍不构成产品 AgentRuntime 行为；P1 ExitGate 仍等待 MAN-003 与签署评估。
