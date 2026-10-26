# 当前状态

日期：2026-10-27

## 仓库与分支

- 原始 `H:/Qbot/EmulativeDialog` 保持只读、未跟踪且未被切换；全部集成工作在 `H:/Qbot/EmulativeDialog.integration` 的 `codex/integration` 分支。
- R7 固定 Base `ae13196829f9ac639f375c9afa07b0566340e3d8` → Head `195ee1632287e24cd8b94c59634227a70f6f28bf`，Worker02 **Approve / Verified**，只含六条 WriteSet 路径。只读 ReviewBundle 位于 `H:/Qbot/ReviewBundles/EmulativeDialog-ED-0013-R7`，SHA256SUMS digest `7e1b3bf21fa05497d31208d265f3640924e4f8fe90238bc57e27173d7547c482`。
- R7 已在本地无冲突合入，集成提交为 `761b80b6b9082fc9227d63498398e3dfd530ca31`。GitHub 插件建 PR 返回 HTTP 403 `Resource not accessible by integration`；未创建 PR。该 bootstrap LocalReviewBundle 和本地合并不等于产品 PR。
- TaskSpec 注册提交 `c5306e27c4ec8bad8ff45d1ac98a01ed0faabd82` 已推送，`origin/codex/integration` 已核实与本地同步。该提交登记 P2/P3 15 个能力 TaskSpec、五个必要基础任务和能力图追踪；注册事件为 `c0afddb6-7fe2-4d19-986a-56d94860a8da`。

## 阶段与实际实现

- ED-0006 Windows 原生 C++20 BuildProbe、ED-0007 冻结合同、ED-0008 WorkflowController，以及 ED-0015/0016/0017 ports/合同、ED-0018 MockChannel、ED-0020 FakeProvider 测试支持均已本地集成；ED-0019 只追加已批准的事件冲突 resolution 并保留原始前缀。
- P1 ExitGate 当前仍为 **Pending**。R7 的 MAN-003 覆盖 20 个 P1 RuleId，但逐条实际证据、Reviewer 决定和 Coordinator 签署尚待完成；Worker05 的独立复核此前正确指出这些状态仍是 Pending。
- 已在 `Workflow/Plans/P2.yaml`、`Workflow/Plans/P3.yaml` 登记 15 个 Keep/Redesign 正式 TaskSpec（ED-0101..ED-0115），精确列出 Owner/Reviewer、ReadSet/WriteSet、依赖、Acceptance 和验证命令。另有必要的网络/构建/存储基础任务 ED-0120..ED-0124。TaskSpec 仍为 Blocked，尚未分派产品代码。
- **Codex AgentRuntime 产品 C++ 源码仍未实现。** FakeProvider、MockChannel、Port、能力图、合同与任务计划均不是 AgentRuntime 成果；不得报告首条产品纵向切片已完成。

## 目标范围与下一项产品切片

- 保持 ADR-0002 范围：Windows 原生 C++20；Responses、兼容 Chat Completions、Anthropic Messages；首渠道 QQ；微信 Deferred。CLI/IDE、开发工作流、Shell/主机状态、任意文件/网络/MCP 工具、TUI 不阻塞 Agent 核心。
- P1 Gate 签署后，首条切片按依赖执行 **ED-0109 Core 身份/策略 → ED-0101 AgentRuntime 回合与 Mock/Fake 端到端行为 → ED-0124 根 CMake/CTest 集成**。验收要求 Mock 入站身份可信、拒绝/去重不调用 Runtime、FakeProvider 只在完整终态后交付答案、Core 只发送一次授权文本并核对相关 Mock 回执；Core 与 Agent 生产源逐模块覆盖率至少 80%。
- ED-0120 冻结共用 HTTPS Transport Port；首条切片通过后，ED-0121 WinHTTP/Schannel 与 ED-0104/0105/0106 三个 Provider Adapter 可用互不重叠写集并行推进。Context、Memory、取消、工具、错误恢复等按 P2/P3 依赖继续分派。
- 真实 QQ 登录/发送仍须等待官方授权路径和能力证据；本轮不接微信。

## 当前验证与下一步

- R7 Bundle 的 9 个内容哈希已核验；固定 Head 的六路径核对和 `git diff --check` 退出 0；MAN-003 的 20/20 RuleId 与 blocker union 一致；10 个 Markdown 链接无断链。
- 现成 MinGW C++20 `WorkflowController.exe query` 对 ED-0101..ED-0115 及 ED-0120..ED-0124 共 20 个 TaskSpec 查询均退出 0，状态均为 Blocked（产品门未通过）。
- 下一步：为 MAN-003 逐 RuleId 绑定 P1/bootstrap 证据与独立评审字段，交 Worker05 固定状态复核，再由 Coordinator 签署 P1 ExitGate。只有该门通过后才创建实现 worktree 并派发 C++ 代码任务。
