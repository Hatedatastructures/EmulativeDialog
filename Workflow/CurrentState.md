# 当前状态

日期：2026-10-28

## 仓库与分支

- 当前唯一集成目标是 GitHub 默认分支 `main`；代码任务在独立分支/worktree 实施，构建和评审产物放在 `H:/Qbot/EmulativeDialog.integration-port-contract.build/` 等仓库外目录。主工作区仅由 Coordinator 更新治理账本；不重置、不清理用户文件。
- R7 固定 Base `ae13196829f9ac639f375c9afa07b0566340e3d8` → Head `195ee1632287e24cd8b94c59634227a70f6f28bf`，Worker02 **Approve / Verified**，只含六条 WriteSet 路径。只读 ReviewBundle 位于 `H:/Qbot/ReviewBundles/EmulativeDialog-ED-0013-R7`，SHA256SUMS digest `7e1b3bf21fa05497d31208d265f3640924e4f8fe90238bc57e27173d7547c482`。
- R7 已在本地无冲突合入，集成提交为 `761b80b6b9082fc9227d63498398e3dfd530ca31`。GitHub 插件建 PR 返回 HTTP 403 `Resource not accessible by integration`；未创建 PR。该 bootstrap LocalReviewBundle 和本地合并不等于产品 PR。
- TaskSpec 注册提交 `c5306e27c4ec8bad8ff45d1ac98a01ed0faabd82` 已推送，`origin/codex/integration` 已核实与本地同步。该提交登记 P2/P3 15 个能力 TaskSpec、五个必要基础任务和能力图追踪；注册事件为 `c0afddb6-7fe2-4d19-986a-56d94860a8da`。

## 阶段与实际实现

- ED-0006 Windows 原生 C++20 BuildProbe、ED-0007 冻结合同、ED-0008 WorkflowController，以及 ED-0015/0016/0017 ports/合同、ED-0018 MockChannel、ED-0020 FakeProvider 测试支持均已本地集成；ED-0019 只追加已批准的事件冲突 resolution 并保留原始前缀。
- P1 ExitGate 已在源快照 `89c2c6f8002df64c990db17ccecc438aa14fea93` 上通过 Coordinator 签署，范围仅为 P1/bootstrap。MAN-003 的 20 个 RuleId 均获独立 Reviewer Approve，并在事件 `a4de8064-e080-4331-9981-ac62b1ee6c6c` 记录；证据清单 SHA256 为 `770f3b3d0913e49cf70a2aff16b6afa44c1e7754dab179d5d201cba5c0205ecc`。
- P2 EntryGate 已通过，范围允许 ED-0109 → ED-0125 → ED-0101 → ED-0124 的首条 C++ 纵向切片。ED-0124/R9 已完成并合并；早期 ED-0109/0125/0101 使用 LocalReviewBundle 的例外仅是历史首片记录，后续任务必须走正式 PR。
- 已在 Workflow/Plans/P2.yaml、Workflow/Plans/P3.yaml 登记原有 Keep/Redesign TaskSpec。ED-0109/R4、ED-0125/R3、ED-0101/R5 的固定 Head 审查和本地集成证据保留；ED-0124/R9 已把 AgentRuntime VerticalSlice 与 WorkflowController 合同测试接入 Windows 根 CMake/CTest，并通过固定 Head 审查与 GitHub PR #1 squash 合并。
- 2026-10-27 GitHub 插件已成功创建并合并 PR #1 到 `main`；这是 R9 根构建/WorkflowController fixture 集成，不是异步 Codex AgentRuntime 重写。PR 的 Windows 本地验收和独立审查记录在 Task ED-0124/R9；PR 未配置在线 checks。
- ED-0101/R5 已有同步 C++ AgentRuntime 回合行为；R9 已完成首片根构建/CTest 验收。Codex Agent 核心重写仍在进行，下一步先建立锁定 Boost 1.92.0 的 Windows C++20/Asio 协程构建基线，再冻结异步 Transport/Provider 合同并实现异步 AgentRuntime。

## 目标范围与下一项产品切片

- 保持 ADR-0002 范围：Windows 原生 C++20；Responses、兼容 Chat Completions、Anthropic Messages；首渠道 QQ；微信 Deferred。CLI/IDE、开发工作流、Shell/主机状态、任意文件/网络/MCP 工具、TUI 不阻塞 Agent 核心。
- P1 Gate 签署后，首条切片按依赖执行 ED-0109 Core 身份/策略 → ED-0125 MockChannel 授权接缝 → ED-0101 C++ AgentRuntime 回合 → ED-0124 根 CMake/CTest 集成。ED-0109/R4 与 ED-0125/R3 已集成；ED-0101/R5 五路径实现已于 e2f7246269a2debfed3a164e4f14b8347b5e3ef5 squash 集成并推送。Worker06 固定 Head Reviewer Approve；Windows CMake 构建通过、CTest 2/2 通过，unique-source gcov JSON v2 coverage 为 143/160 (89.38%)。验收仍要求 Mock 入站身份可信、拒绝/去重不调用 Runtime、FakeProvider 只在完整终态后交付答案、Core 只发送一次授权文本并核对相关 Mock 回执；Core 与 Agent 生产源逐模块覆盖率至少 80%。
- ED-0120/R2 仍为 Blocked：当前仓库尚无冻结的 HttpTransportContract/Port，旧 TaskSpec 目标为 WinHTTP/Schannel。新建 ED-0131 先验证锁定 Boost 1.92.0 的 C++20 Asio；之后须通过 ChangeRequest 将 Transport 合同改为 awaitable/Asio，再启动网络和 Provider 实现。
- 真实 QQ 登录/发送仍须等待官方授权路径和能力证据；本轮不接微信。

## 当前验证与下一步

- R7 Bundle 的 9 个内容哈希已核验；固定 Head 的六路径核对和 `git diff --check` 退出 0；MAN-003 的 20/20 RuleId 与 blocker union 一致；10 个 Markdown 链接无断链。
- WorkflowController query H:/Qbot/EmulativeDialog.integration P2 ED-0109 4 显示 ED-0109 已 Merged。ED-0125/R3 Base..Head 仅有 3/3 WriteSet 路径；MSYS2 g++ 16.1.0 的五 TU 编译、链接、行为测试均 exit 0，Coordinator 复跑测试 exit 0；本任务生产 TU 覆盖率按 TaskSpec 为 NotApplicable。固定 Head Reviewer Approve；GitHub PR 尝试 HTTP 403；本地 ReviewBundle manifest SHA256 为 2B88C491E552378E4F984D6EB4D2CBC90F2D4511D9C77EB753BE9480C1C7521B，单一 squash 集成提交 5be9f296dddcf9c7c254afc8d908e2d00d675ae0 已推送。
- ED-0101/R5 TaskSpec 已在 commit ad2a3f6bfe4c534f69b20edd095a72d0df22fae7 固定；CR-ED-0101-003 已由 Worker02 Owner 与 Worker01 independent reviewer 批准，六项依赖均 Merged。assignment baseline a28e02523b286f9957fb2d60012241b567494654；Controller 创建并验证 branch Task/ED-0101-R5AgentRuntimeSlice/worktree H:/Qbot/EmulativeDialog.ED-0101-R5。Worker02 完成五路径 AgentRuntime 实现并提交一个模块提交；Worker06 固定 Head review Approve。Controller 构建/CTest 2/2/coverage 命令全部 exit 0；TaskSpec unique-source coverage 143/160 (89.38%)。ReviewBundle SHA256 为 F1AA79FDDD1D21BBEC8D97A546CB3037662305EF1A22DF3500A197BB38DB71C；集成提交 e2f7246269a2debfed3a164e4f14b8347b5e3ef5 已推送，ED-0124 是下一门。
- GitHub plugin repo metadata on 2026-10-27 confirms default branch `main` and `admin/push/pull` permissions. PR #1 (`ED-0124/R9`) was created through the plugin, independently approved, squash-merged to `main` as `38cbf4e3d55406a961c8adb7c947bd9fde04690e`, and its remote task branch was retired. The previous 403s for ED-0109/R4, ED-0125/R3, and ED-0101/R5 remain historical.
- ED-0124/R9 fixed Base `8029d6f83dba91cad580753facc2e69fc4e00e35` → Head `5b45482fc9348e0285bbed139c16b1dc4de5b63d`; Worker06 independently approved. The four-path code diff integrated the AgentRuntime test target and focused Windows preset, added `WorkflowControllerTestWorkspace.hpp`, relocated fixtures to the build `T` root, and added marker/prefix/parent/read-only/reparse-safe cleanup. Configure, build, focused CTest, and coverage target exited 0; `WorkflowController.Contract` and `AgentRuntime.VerticalSlice` passed; gcov JSON v2 coverage was 144/177 (81.36%). Stale `.gcda` was retained under central Diagnostics; no generated source-tree files were added.
- ED-0131/R1 was superseded before assignment under CR-ED-0131-002 after Owner review found that its frozen CTest command did not load the required CMake PATH. ED-0131/R2 is Proposed with the same Goal/WriteSet/Acceptance and corrected ReadSet/preset commands; no source branch/worktree exists yet. PR #1 completed build/test integration but does not complete the async AgentRuntime rewrite.

## 2026-10-28 协调状态快照

- 权威基线：`origin/main = f47067d5661b277682f3d3f0659fd4334aca39df`。本地 `H:/Qbot/EmulativeDialog` 原为无提交孤儿状态，已通过 `git stash push -u` 保存旧未跟踪快照后用 `git checkout -B main origin/main` 对齐远端；工作区现为干净。
- 远端分支：`main`、`Task/ED-0131-BoostAsioBuildFoundationR2`、`Task/ED-0140-CollabPrFlow`，共 3 个，低于 4 上限。
- 已合并：PR #2（ED-0131 前置 Boost 门）、PR #3（ED-0102/ED-0103 合同冻结）、PR #4（ED-0102/R3 SessionState，`a1ad397`）、PR #5（ED-0103/R5 ContextSelector，`f47067d`）。
- 待审 PR：#6 `Task/ED-0131-BoostAsioBuildFoundationR2 @ a6caa42`（Reviewer Worker03/Worker04/Worker06 固定 Head）、#7 `Task/ED-0140-CollabPrFlow @ a9891e7`（Reviewer Worker04）。
- 基线验证（Coordinator 本人执行）：`cmake --preset windows-agent-slice` configure 退出码 0；`cmake --build --preset windows-agent-slice --parallel 6` 退出码 0；`ctest --preset windows-agent-slice` 2/2 通过（WorkflowController.Contract、AgentRuntime.VerticalSlice）。
- 已解锁并派发：ED-0120/R2（HttpTransportContract 冻结 → Worker01）、ED-0115/R2（InstructionAssembler → Worker02）、ED-0122/R3（SQLite 依赖固定 → Worker05）。三者依赖均已 Merged。
- 仍 Blocked：ED-0104/0105/0106（三 Provider，等 ED-0120 合并）、ED-0107（等三 Provider）、ED-0108/0110/0111/0114（等 ED-0107）、ED-0112（等 ED-0108/0110/0111）、ED-0113（等 ED-0123←ED-0122）、ED-0121（等 ED-0120）、ED-0123（等 ED-0122）。
- 现有实际 C++ AgentRuntime 行为（已在 main）：`Source/Agent/AgentRuntime.cpp` 同步回合循环、输入映射、事件序列校验、取消、错误映射、终态门；`Tests/Contract/Agent/AgentRuntimeVerticalSliceTests.cpp` 为 QQ Mock → Core → AgentRuntime → FakeProvider → Mock 回执纵向切片。
- 明确未完成：异步协程化 AgentRuntime、三 Provider 真实 wire adapter、工具调用往返、流式事件投影、统一取消生命周期、失败重试、恢复状态、聊天记忆持久化、QQ 真实接入。不得把上述已合并内容表述为 Codex 重写完成。

## 2026-10-28 继续执行状态（当前）

- 当前产品代码基线为 `main@8a608831ce94020a0187cf586abd66a4834de966`（PR #23 ED-0113 merge commit）；本次 Coordinator 主线提交只更新任务/协作账本。远端分支共 3 个：`main`、`Task/ED-0105-R4-ChatCompletions`、`Task/ED-0114-R2-UsageLimits`；名额上限为 4。
- PR #23 已由 Worker06 固定头批准并 squash 合并；GroupMemory consent 读取、tombstone 防复活和 SC-008/FR-006 场景已进入 main。ED-0144/R1 现在负责把 ConversationMemory 接到 SQLite 生产存储。
- PR #20 ED-0114 仍开放，Base `5531594dfeb36b9419cda7a5a8faca6f6f3e4c0a`、Head `a23f252268d32a8b7315797f74a2562b72772d2c`。Worker04 固定头结论为 CHANGES_REQUESTED：缺失 usage 仍会放行工具/续采样；续跑批准只有测试直接调用而无 Core 生产授权路径；常驻预算账本没有生产清理；16 个 diff 路径中有 3 个测试路径未列入 13 项 WriteSet。Worker03 独立 Windows C++20 验证 configure/build 均退出 0、CTest 19/19、六个生产源覆盖率 85.20%–100%。作者已收到 finding，正在提出精确 ChangeRequest/TaskRevision；问题未修复前不合并。
- PR #24 ED-0105 的 R4 Head `d544b5ad0d59a472eb9f3772d3fc28411c18a73f` 固定头审查为 CHANGES_REQUESTED，发现 8 项 Major finding：超时重试、请求内容丢失、工具结果关联/类型、嵌套未知字段、unsupported image、fixture 未执行、malformed usage。Worker03 独立 Windows C++20 定向构建和 CTest 1/1 通过，以黑盒 probe 复现 response-head 后 timeout 被标成 retryable；coverage 835/935=89.30%。Coordinator 已接受 ED-0105/R5 CR，改用 `.hpp`、ModelProviderContract V1.2，并把 fixture ReadSet/执行验收列明；作者等待任务账本提交后重基修复，当前未合并。
- ED-0112/R2 恢复实现仍为本地 Head `9b312513161a441eb4b94325be176348b34f7e9d`，基于 `main@5ac8966`；Windows 全量构建、CTest 16/16，两个生产源覆盖率 87.38%/94.48%。独立固定头审查待 Worker06 完成 ED-0105/R5 后继续；目前未推送、未创建 PR。
- ED-0144/R1 在隔离 worktree `H:/Qbot/EmulativeDialog.ED-0144`、分支 `Task/ED-0144-MemorySqliteAdapter`、Base `8a608831` 已完成实际 SQLite adapter，Head `daeb204fb4b87b3405a4c05beb111bfe8d5c3f52`。Windows configure/目标/全目标 build 均退出 0，CTest 4/4；gcov JSON v2 为 289/306=94.44%。版本化可逆 `EDMemoryV1/<Category>/<0|1>` 编码保留独立 `UserConfirmed`，SQLite 重启读回六种 Provenance/confirmation 组合并拒绝 malformed/unknown/future version。代码已提交，待重基到最新 main、Worker06 固定头审查后推送 PR。
- 已确认 Windows GitHub CLI 登录为 `Hatedatastructures`，远端 `main` 为默认分支；本轮 GitHub 插件可读取、审查与合并 PR。子 Agent 槽按主会话模型/推理继承；插件未提供子 Agent 的精确模型标签读取接口。
- 过往 main `be2fd36` 的 Windows 根构建和 CTest 17/17 是 PR #23 之前的证据；PR #20 的 19/19 是其固定分支上的证据。PR #23 合并后新的 main 全量根测试尚未复跑。Provider、Memory 与 Network 合同测试的统一根 CMake/CTest 接入仍由 ED-0145 处理。
- Codex 官方上游基线仍为 `openai/codex@970111f6cb62cc02fe36a82ea7d71b37a2889c97`、Apache-2.0。CodexCapabilityMap 15 项 Keep/Redesign 均链接 TaskSpec，但未有实现证据的条目继续保持未完成。AgentRuntime 仍是同步回合；Core 授权上下文压缩续跑、异步 C++20/Boost.Asio 回合、真实 QQ Adapter 和剩余能力映射仍需继续实现；本阶段不接入微信。
