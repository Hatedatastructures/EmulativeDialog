# 当前状态

日期：2026-10-27

## 仓库与分支

- 原始 `H:/Qbot/EmulativeDialog` 保持只读、未跟踪且未被切换；全部集成工作在 `H:/Qbot/EmulativeDialog.integration` 的 `codex/integration` 分支。
- R7 固定 Base `ae13196829f9ac639f375c9afa07b0566340e3d8` → Head `195ee1632287e24cd8b94c59634227a70f6f28bf`，Worker02 **Approve / Verified**，只含六条 WriteSet 路径。只读 ReviewBundle 位于 `H:/Qbot/ReviewBundles/EmulativeDialog-ED-0013-R7`，SHA256SUMS digest `7e1b3bf21fa05497d31208d265f3640924e4f8fe90238bc57e27173d7547c482`。
- R7 已在本地无冲突合入，集成提交为 `761b80b6b9082fc9227d63498398e3dfd530ca31`。GitHub 插件建 PR 返回 HTTP 403 `Resource not accessible by integration`；未创建 PR。该 bootstrap LocalReviewBundle 和本地合并不等于产品 PR。
- TaskSpec 注册提交 `c5306e27c4ec8bad8ff45d1ac98a01ed0faabd82` 已推送，`origin/codex/integration` 已核实与本地同步。该提交登记 P2/P3 15 个能力 TaskSpec、五个必要基础任务和能力图追踪；注册事件为 `c0afddb6-7fe2-4d19-986a-56d94860a8da`。

## 阶段与实际实现

- ED-0006 Windows 原生 C++20 BuildProbe、ED-0007 冻结合同、ED-0008 WorkflowController，以及 ED-0015/0016/0017 ports/合同、ED-0018 MockChannel、ED-0020 FakeProvider 测试支持均已本地集成；ED-0019 只追加已批准的事件冲突 resolution 并保留原始前缀。
- P1 ExitGate 已在源快照 `89c2c6f8002df64c990db17ccecc438aa14fea93` 上通过 Coordinator 签署，范围仅为 P1/bootstrap。MAN-003 的 20 个 RuleId 均获独立 Reviewer Approve，并在事件 `a4de8064-e080-4331-9981-ac62b1ee6c6c` 记录；证据清单 SHA256 为 `770f3b3d0913e49cf70a2aff16b6afa44c1e7754dab179d5d201cba5c0205ecc`。
- P2 EntryGate 已通过，范围仅允许 ED-0109 → ED-0125 → ED-0101 → ED-0124 的首条 C++ 纵向切片，并已接受 CR-P2-FIRSTSLICE-001：若 GitHub 插件不能建 PR，可为这四项使用固定 Head 的独立 LocalReviewBundle，同时单独记录 PR 尝试；这不满足后续 LargeScaleProductImplementation 正式 PR 门。
- 已在 `Workflow/Plans/P2.yaml`、`Workflow/Plans/P3.yaml` 登记 15 个 Keep/Redesign 正式 TaskSpec（ED-0101..ED-0115）。它们仍须逐项修订可执行的当前 Owner/Reviewer、ReadSet/WriteSet、依赖、合同 pin、BaselineCommit 与验证命令后才能分派；当前尚无 AgentRuntime 产品代码分支。
- 远端 PR 平台查询确认仓库 `codex/integration` 未保护（`protected=false`）、规则集为空，GitHub 插件 PR 创建返回 HTTP 403 `Resource not accessible by integration`，本机 `gh` 未登录。LargeScaleProductImplementation 仍 BlockedUnverified；首条切片按上述窄范围本地评审路径推进。
- **Codex AgentRuntime 产品 C++ 源码仍未实现。** FakeProvider、MockChannel、Port、能力图、合同与任务计划均不是 AgentRuntime 成果；不得报告首条产品纵向切片已完成。

## 目标范围与下一项产品切片

- 保持 ADR-0002 范围：Windows 原生 C++20；Responses、兼容 Chat Completions、Anthropic Messages；首渠道 QQ；微信 Deferred。CLI/IDE、开发工作流、Shell/主机状态、任意文件/网络/MCP 工具、TUI 不阻塞 Agent 核心。
- P1 Gate 签署后，首条切片按依赖执行 **ED-0109 Core 身份/策略 → ED-0101 AgentRuntime 回合与 Mock/Fake 端到端行为 → ED-0124 根 CMake/CTest 集成**。验收要求 Mock 入站身份可信、拒绝/去重不调用 Runtime、FakeProvider 只在完整终态后交付答案、Core 只发送一次授权文本并核对相关 Mock 回执；Core 与 Agent 生产源逐模块覆盖率至少 80%。
- ED-0120 冻结共用 HTTPS Transport Port；首条切片通过后，ED-0121 WinHTTP/Schannel 与 ED-0104/0105/0106 三个 Provider Adapter 可用互不重叠写集并行推进。Context、Memory、取消、工具、错误恢复等按 P2/P3 依赖继续分派。
- 真实 QQ 登录/发送仍须等待官方授权路径和能力证据；本轮不接微信。

## 当前验证与下一步

- R7 Bundle 的 9 个内容哈希已核验；固定 Head 的六路径核对和 `git diff --check` 退出 0；MAN-003 的 20/20 RuleId 与 blocker union 一致；10 个 Markdown 链接无断链。
- 现成 MinGW C++20 `WorkflowController.exe query` 对 ED-0101..ED-0115 及 ED-0120..ED-0124 共 20 个 TaskSpec 查询均退出 0，状态均为 Blocked（产品门未通过）。
- 下一步：将 ED-0109/R3 修订为经固定合同/API、Owner/Reviewer、分支/worktree、工具版本与覆盖率命令审阅的 R4；随后只创建该任务的独立 worktree/branch，完成 Core 身份策略 C++ 代码、80% 覆盖与独立 LocalReviewBundle。之后依序处理 ED-0125、ED-0101、ED-0124。
