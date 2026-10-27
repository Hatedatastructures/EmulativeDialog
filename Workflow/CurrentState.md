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
- 已在 Workflow/Plans/P2.yaml、Workflow/Plans/P3.yaml 登记 15 个 Keep/Redesign 正式 TaskSpec。ED-0109/R4 Core 身份/策略与 ED-0125/R3 MockChannel 授权接缝已分别通过固定 Head 独立审查并在 codex/integration 本地集成；ED-0125 的单一模块提交为 5be9f296dddcf9c7c254afc8d908e2d00d675ae0，ReviewBundle 位于 H:/Qbot/ReviewBundles/EmulativeDialog-ED-0125-R3。ED-0101 AgentRuntime 与 ED-0124 根构建仍未完成。
- GitHub 插件分别尝试为 ED-0109/R4 和 ED-0125/R3 创建 PR，均返回 HTTP 403 Resource not accessible by integration；本机 gh 未登录。ED-0125 按 CR-P2-FIRSTSLICE-001 使用固定 Head LocalReviewBundle 并本地集成；无 GitHub PR。该首片例外不满足 LargeScaleProductImplementation 正式 PR 门，后续 Provider/Memory/QQ 任务仍 BlockedUnverified。
- AgentRuntime 回合循环和完整产品 C++ 纵向切片仍未实现。ED-0109 Core 身份/策略与 ED-0125 MockChannel 接缝已集成；ED-0101 AgentRuntime 和 ED-0124 根 CMake/CTest 仍未完成，不能报告第一条产品纵向切片或 Codex 重写已完成。

## 目标范围与下一项产品切片

- 保持 ADR-0002 范围：Windows 原生 C++20；Responses、兼容 Chat Completions、Anthropic Messages；首渠道 QQ；微信 Deferred。CLI/IDE、开发工作流、Shell/主机状态、任意文件/网络/MCP 工具、TUI 不阻塞 Agent 核心。
- P1 Gate 签署后，首条切片按依赖执行 ED-0109 Core 身份/策略 → ED-0125 MockChannel 授权接缝 → ED-0101 C++ AgentRuntime 回合 → ED-0124 根 CMake/CTest 集成。ED-0109/R4 与 ED-0125/R3 已集成；ED-0101/R4 TaskSpec 已接受并固定，仍需在分派时验证全部依赖、冻结 BaselineCommit 并创建隔离 worktree，然后实现 C++ AgentRuntime。验收要求 Mock 入站身份可信、拒绝/去重不调用 Runtime、FakeProvider 只在完整终态后交付答案、Core 只发送一次授权文本并核对相关 Mock 回执；Core 与 Agent 生产源逐模块覆盖率至少 80%。
- ED-0120 冻结共用 HTTPS Transport Port；首条切片通过后，ED-0121 WinHTTP/Schannel 与 ED-0104/0105/0106 三个 Provider Adapter 可用互不重叠写集并行推进。Context、Memory、取消、工具、错误恢复等按 P2/P3 依赖继续分派。
- 真实 QQ 登录/发送仍须等待官方授权路径和能力证据；本轮不接微信。

## 当前验证与下一步

- R7 Bundle 的 9 个内容哈希已核验；固定 Head 的六路径核对和 `git diff --check` 退出 0；MAN-003 的 20/20 RuleId 与 blocker union 一致；10 个 Markdown 链接无断链。
- WorkflowController query H:/Qbot/EmulativeDialog.integration P2 ED-0109 4 显示 ED-0109 已 Merged。ED-0125/R3 Base..Head 仅有 3/3 WriteSet 路径；MSYS2 g++ 16.1.0 的五 TU 编译、链接、行为测试均 exit 0，Coordinator 复跑测试 exit 0；本任务生产 TU 覆盖率按 TaskSpec 为 NotApplicable。固定 Head Reviewer Approve；GitHub PR 尝试 HTTP 403；本地 ReviewBundle manifest SHA256 为 2B88C491E552378E4F984D6EB4D2CBC90F2D4511D9C77EB753BE9480C1C7521B，单一 squash 集成提交 5be9f296dddcf9c7c254afc8d908e2d00d675ae0 已推送。
- ED-0101/R4 TaskSpec 已在 commit c41bdc87fb10a70bfbe86ba7b0eb1cc692d0e6d3 固定；CR-ED-0101-002 已由 Worker02 Owner 和 Worker01 independent reviewer 批准。六项冻结依赖均经 WorkflowController 查询为 Merged；TaskState 仍 Blocked，尚无 TaskAssigned 事件或代码分支。下一步冻结 assignment BaselineCommit 并创建独立 R4 worktree，然后立即实现 AgentRuntime 行为。ED-0124 在 AgentRuntime code 验收后接入根 CMake/CTest。Provider 与 Memory/Context 继续等待首片和 LargeScale PR 门。
