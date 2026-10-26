# ADR-0002：以 C++ 重实现 Codex 聊天 Agent 核心

状态：产品 owner 已确认目标方向；Codex 上游仓库/提交、QQ 接入模式和细分合同仍待 P0 核验。

## 背景

EmulativeDialog 的主要任务是把 Codex Agent 核心改写为 C++，并面向 QQ 聊天做专用设计。目标不是启动 Rust Codex、调用其 app-server 或在外面包一层 QQ 转发器。产品还需支持 OpenAI Responses、OpenAI-compatible Chat Completions 和 Anthropic Messages，并具备聊天上下文、记忆和工具调用编排。

聊天产品不需要 Codex 的所有桌面开发功能。用户明确要求去掉主机命令拦截、主机状态查询等聊天场景无关能力。本阶段以 QQ 为首个渠道；微信暂缓。

## 决定

- **主目标**：把聊天产品范围内所有保留或重设计的 Codex Agent 核心能力完整用 C++ 重实现，而不是只做一个空循环、Provider 转发器或 Rust Codex 包装层。
- **纳入范围**：指令与回合编排、会话状态、上下文选择、三种模型协议、流式事件、工具调用生命周期、聊天记忆、取消、错误处理和可恢复状态。CodexCapabilityMap 在 P0 锁定完整的目标能力分项。
- **逐项追踪**：每项 Codex 上游能力明确标记 Keep、Redesign、Remove 或 Defer。Keep/Redesign 必须继续追踪到目标 C++ 模块、合同、TaskId、实现状态和验收证据；能力地图不能替代 C++ 实现。
- **明确排除**：Codex CLI/IDE 用户界面、编码工作流、Shell/主机命令拦截、主机状态查询、任意文件访问和通用开发工具。不把这些功能偷偷加入聊天 Agent。
- **首个渠道**：当前阶段只推进 QQ。微信接入不进入当前任务和验收，未来须另行开启账号面与授权决策。
- **实现方法**：按稳定模块和行为切片逐步实现，每个切片独立合同、WriteSet、验收和 PR；不按上游行数拆任务，不要求逐行翻译。

## 验收含义

“Codex 改写完成”仅能用于 ADR-0002 批准范围内的 Agent 核心：目标能力映射完整，所有 Keep/Redesign 能力已有 C++ 实现和合同证据，Remove/Defer 均有明确决定。单独完成仓库调研、C++ 空工程、一个 Provider 或 TUI 都不能代表整个目标已完成。

## 后果

- Goals/ProductGoals.md、Workflow/Roadmap.md 和 Workflow/Plans 中的任务都必须遵守本决定。
- Source baseline 未锁定前可以做独立接口/平台调研，但不能宣称能力映射完整。
- 当前可并行的 Provider、Memory、Core/Agent、TUI 或 QQ 任务须有冻结合同和互不重叠写集。
- 任何要恢复 CLI/IDE/主机工具或开启微信的需求，须提交 ChangeRequest；不得由 Agent 擅自扩展。
