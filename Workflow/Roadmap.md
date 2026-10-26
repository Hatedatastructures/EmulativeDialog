# 全 C++ 聊天 Agent 重写路线图

每个阶段有明确退出条件。接口先于平行实现；平台不支持的能力留在 Unsupported，不用私有 fallback 填空。

## P0：范围、账号面与能力证据
- 按 ADR-0002 执行：主任务是 C++ 重实现 Codex 聊天 Agent 核心；不重新打开已确定的聊天专用范围。
- 固定 Codex 上游仓库/提交，并按 Agent 子系统和可观察能力建保留、重设计、移除、暂缓映射；不按行数安排重写任务。
- 选定一个 TypeScript TUI 参照库及精确版本，定义公开 API 对照和零未批准差异门。
- 核实并选择 QQ 接入账号/平台类型和授权接口。
- 为昵称、图片/语音、贴纸、反应、拍一拍、主动消息建立平台证据矩阵。
- 微信本阶段延期，不进入调研、实现和验收。
- 锁定产品 identity disclosure、MemoryRetention、Boost/TLS、Windows terminal 支持范围。
- CodexCorePreparationGate：Codex 目标能力映射和模块 owner 冻结后开放 P1/C++ 基础与控制器工作。
- QQGate：QQ 授权与能力证据只控制 QQ Adapter 开工。
- TuiGate：TUI 参照版本与 API 范围只控制 TUI 开工。
- 微信保持 Deferred；三个领域门互不扩大或替代 Codex 核心重写目标。

## P1：C++ 工程与接口合同
- 建立 C++20 Windows x64 CMake skeleton、PascalCase 静态检查和依赖登记。
- 冻结 CoreAgent、ModelProvider、ChannelAdapter、MemoryScope、TUI contracts。
- 先实现 C++ 开发期 AgentWorkflowController：按 TaskId 装载上下文、检查状态/写集、创建隔离 worktree、维护检查点/消息/事件，并记录 PR 审查与合并证据。
- 先用 FakeProvider/FakeChannel 编译和跑契约 fixture，不请求真实账号。
- Gate：WorkflowController 的隔离、任务版本、ReadSet/WriteSet、事件恢复和独立审查门均可实际执行；正式远端 PR、集成分支、分支保护和 required checks 已配置；Windows native clean build、合同文档和基础失败语义可复现。未通过此门不得开始大规模 Agent/Provider/Channel/TUI 实现。

## P2：C++ 重实现 Codex 聊天 Agent 核心
- 按 CodexCapabilityMap 逐项实现所有 Keep/Redesign 能力：Agent 回合循环、会话状态、上下文/记忆、工具调用生命周期、流式事件、取消、错误处理和恢复。
- 先打通一条 QQ Mock ingress → C++ Core/AgentRuntime → FakeProvider → ChannelReceipt 的可执行纵向切片，再按模块与 Provider 能力矩阵扩展。
- 每条能力映射到 C++ 源码模块、合同、TaskId、验收和 PR；禁止把“完成调研/目录映射”报告为 Codex 重写完成。
- Core 是唯一 policy/authorization owner；模型看不到宿主命令或未授权工具。
- Gate：所有当前阶段的 Keep/Redesign 能力都有 C++ 实现证据；重复事件、上下文隔离、权限拒绝、超时未知结果、工具调用状态和取消/恢复验证通过。

## P3：三种 Provider 适配器
- Responses、Chat Completions、Anthropic 三组模块并行开发，写集互不重叠。
- 使用 in-memory transport fixtures 验证 requests、SSE、usage、错误、limit、cancel。
- Gate：三个 Adapter 产出同一 CanonicalModelEvent 序列；所有不支持字段显式失败。

## P4：QQ 渠道适配器
- 先实现 Mock/fixture adapter，再为 P0 已确认的 QQ 账号面实现 C++ adapter。
- 按能力矩阵逐项实现消息、媒体和获准社交动作；真实账号 qualification 单独进行。
- 微信保持 Deferred，不与 QQ 共享身份或能力假设。
- Gate：QQ 目标账号授权、速率限制、失败回执、撤销和恢复有证据。

## P5：自研 C++ TUI
- TUI 拆成 TerminalSession、Unicode/CellBuffer、DiffRenderer、Layout、Component API、InputRouter 和 parity harness 等可独立合并的能力切片；每个切片沿用稳定 TUI Epic，不为每个小控件创建单独说明文档。
- 实现 chat timeline、multiline input、streaming、channel selector、memory view/forget 和 profile settings。
- Gate：参照库批准 API 覆盖率 100%、未批准差异为零；Windows Terminal 的中文/Emoji、resize、粘贴、输入/焦点、布局、流式输出和异常退出恢复均有可重复的差异场景证据。

## P6：集成和发行
- 端到端连通 Mock ingress、Agent、Provider fixture、Channel receipt 和 TUI。
- 构建同一 C++ runtime；生成依赖/许可清单、数据迁移、日志脱敏和恢复说明。
- Gate：PR 都合入、风险关闭、目标 Windows Release 和一次最终 smoke 通过。

## 阶段升级规则

新需求、平台变化、协议字段变化或反复失败都通过 ChangeRequest 更新路线图。不要用全仓文档改写替代一个小型决策记录。
