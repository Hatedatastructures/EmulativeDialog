# 当前状态

日期：2026-10-26

## 仓库证据
- 根路径：H:/Qbot/EmulativeDialog。
- 原始 `main` 仍是 unborn branch，原工作区中的项目目录仍未跟踪。为保护它并提供隔离基线，协调者从只读 workspace snapshot 建立本地 bootstrap commit `6e9b727ab88ae910d70069d9b9f19a9912f2bcb4`；当前集成工作树是 `codex/integration`，原工作区未切换或覆盖。
- 远端 `origin` 当前没有公布 HEAD 或分支；启动阶段使用本地 ReviewBundle，不声称已创建远端 PR。
- 本轮创建并修订中文治理、产品、合同、路线和平台能力文档；没有创建 C++ 源码、CMake 工程或测试。
- 长任务控制入口为 Workflow/TaskIndex.yaml；任务按阶段分片，Agent 不逐项加载全计划，也不为每个小任务创建 Markdown。
- 多 Agent 任务要求独立分支/worktree、冻结写集、独立 Reviewer、协调者单写状态、PR 集成队列和结构化事件轨迹。EnforcementMatrix 中未实现门全部标为 Planned。
- P0 候选切片见 Workflow/Plans/P0.yaml；ED-0001 上游能力映射已通过独立复审并合并到本地 `codex/integration`。ED-0005 的依赖已满足并进入 Ready；P0 核心门仍需 ED-0005 完成。
- Workflow/AgentPool.yaml 为八个逻辑槽位中的 Worker01/Worker02 绑定了独立审查与 ED-0001 作者角色；后续 Agent 调用须沿用这两个稳定身份，未声称已结束的会话仍在运行。
- Architecture/ModuleMap.yaml 是分片索引；详细 owner 与写路径按域存于 Architecture/Modules/。
- 已只读核验 Codex 上游候选提交与许可证；ED-0001 负责将它记录进能力地图并完成全目标范围映射。能力地图仍为空，不代表映射完成。TUI 参照库仍未锁定，且不阻塞 Agent 核心。
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
- Codex 上游仓库/提交和 Agent 能力映射仍待 P0 完成；这不再是是否实现 Codex 聊天 Agent 核心的范围问题。
- QQ 账号面和授权接口仍待核验；微信保持 Deferred。
- TUI 必须先锁定一个 TypeScript 参照库及精确版本；目标 API 映射覆盖率为 100%，未批准的行为差异为零。JSX 字面语法与原生 C++ 语法的冲突须单独明确，不得借此减少组件能力。

## 下一步
1. 按 ADR-0002 固定的聊天 Agent 范围锁定 Codex 上游仓库/提交，建立可追踪到 C++ 模块和验收的能力地图。
2. 完成 QQ 官方/授权接入证据；TUI 参照库只阻塞 TUI 自身实现，不阻塞 Codex Agent 核心。
3. 冻结 Codex Agent/Core、三 Provider、Memory/Context 的首批合同与模块写集。
4. 建立 C++ AgentWorkflowController 和 Windows 构建/PR 门。
5. 开始 C++ Codex Agent 核心纵向切片；按能力地图逐项实现、审查和更新状态。

## 验证账本
目前没有 C++ 源码、构建或真实 QQ 收发证据；微信本阶段不做收发。平台调研摘要见 Research/Platforms/PlatformCapabilityMatrix.md。
