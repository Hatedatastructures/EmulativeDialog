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

- 当前产品源码与审查基线为 `main@1d8863b286f8e752edc06c2a671721af0a9a74f3`。远端恰有 4 个分支（main 加 3 个活动任务分支），预算已满；现有 PR 原分支用于修复，不创建新的远端分支。
- PR #23 ED-0113 已合并。PR #25 ED-0144/R1 固定 Head `daeb204fb4b87b3405a4c05beb111bfe8d5c3f52` 的 Windows C++20 configure/build 退出 0、CTest 4/4、gcov JSON v2 为 289/306（94.44%）。Worker06 固定头审查 `H:/Qbot/ReviewBundles/EmulativeDialog-ED-0144-R1-Review-daeb204/ReviewResult.md` 为 CHANGES_REQUESTED，Major 是 ProposalId 丢失。已接受 `CR-ED-0144-R2-DATA-001`，R2 保留物理 schema 和四路径 WriteSet，V1 兼容，无 ProposalId 仍用 V1；非空 ID 使用长度前缀十六进制 V2，避免 SQLite 文本读 NUL 截断并保留任意字节。TaskSpec 已指向 R2、基线 1d8863b、独立 `.build` 和 AC-4；待任务账本推送后 Worker05 在原 PR 分支重基并实施。R1 Head 不合并。
- PR #20 ED-0114/R2 固定 Base `5531594dfeb36b9419cda7a5a8faca6f6f3e4c0a`、Head `a23f252268d32a8b7315797f74a2562b72772d2c`。Worker04 的 finding 为缺失 usage 误放行、无 Core 生产续跑授权、账本无生产清理、3 条测试超出原 WriteSet。已接受 `CR-ED-0114-R3-001`：R3 仅实现 fail-closed、Core Process 结束时账本清理和漏列测试，固定赋值基线为 1d8863b；ModelProviderContract 为 V1.2。任务记录推送后 Worker02 重基既有分支开始实施。正向 Core 批准压缩续跑仍未实现，也未完成其合同门；PR #20 不得据此标记能力完成。
- PR #24 ED-0105 原固定 Head `d544b5ad0d59a472eb9f3772d3fc28411c18a73f` 有 8 项 Major finding。Worker01 已实现 R5 修复并在 R6 pinned offline cache 下重新 configure/build 成功、CTest 1/1 通过；fixture 检查又定位并修复了稳定 ItemId 比对、OutputLimited usage 和 SSE 错误样例 runner。R6 的 gcov stem 参数失败已由 coordinator 只读诊断定位：必须给 `-o` 传精确 `.gcno` 文件。`CR-ED-0105-R7-GCOV-001` 已接受为只改验证命令；R6 诊断结果为 JSON v2 `ChatCompletionsProvider.cpp` 958/1046（91.59%），GCC/gcov 16.1.0。作者和独立验证槽都要在固定新 Head 后复跑 R7 命令。
- 独立预检确认 R6 离线 Boost 缓存固定于 Boost 1.92.0 commit `afdfa32505af73e3d208144b3f623f0096cb62b6`，Asio/JSON gitlink 和 include 均存在；Worker03 待新 Head 后在独立 review `.build` 执行 Windows build、CTest、gcov 和 post-response timeout 回归。Provider 当前验证失败均已区分为 fixture/coverage 命令问题，不记为产品构建通过。
- Reviewer 另发现独立 Core 策略缺口：`ConsentMatches` 在 grant 无 ProposalId 时仍允许 request 带任意 ProposalId。它不属于 SQLite adapter 能修复的范围，也不在 ED-0144/R2 验收内；将单独立项，且 R2 不宣称修复 Core 授权策略。
- ED-0112/R2 是本地旧 Head 9b312513161a441eb4b94325be176348b34f7e9d，尚无 PR；旧构建/覆盖证据不是当前 main 验收证据。
- main 已有同步单回合 AgentRuntime 与 QQ Mock→Core→FakeProvider→Mock 回执纵向切片。异步 C++20/Boost.Asio AgentRuntime、Core 所有的上下文压缩授权续跑、全部 Keep/Redesign 能力的实现、真实 QQ Adapter 仍未完成；官方 Codex 基线为 openai/codex@970111f6cb62cc02fe36a82ea7d71b37a2889c97（Apache-2.0）。微信保持 Deferred。
- Worker01/02/03/04/05/06 六个稳定工作槽按 ED-0105 R7 验证/实现、ED-0114 R3 实现/审查、ED-0144 R2 实现/审查并行工作，均继承主会话模型与推理强度。宿主工具不暴露代理实际显示模型标签，故不猜测具体模型名。
