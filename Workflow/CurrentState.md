# 当前状态

日期：2026-10-27

## 仓库与分支

- 原始 `H:/Qbot/EmulativeDialog` 保持只读、未跟踪且未被切换；集成状态保存在 `H:/Qbot/EmulativeDialog.integration` 的 `codex/integration`，任务实现使用独立分支/worktree。GitHub 默认分支已核实为 `main`。
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

- 权威主线：`origin/main = 8f8ab66`；本地 main 工作区干净。远端分支正好 4 个：`main` + `Task/ED-0113-R4-ChatMemory-impl` + `Task/ED-0114-R2-UsageLimits` + `Task/ED-0143-BoostJsonDependency-rebase`。暂不推送新分支，等现有 PR 合并释放名额。
- 当前开放 PR：#14 ED-0143（远端 Head `3baa40d`，Worker03 正在把 pinned 子模块获取移入 populate 阶段并确保静态链接状态不传播）、#20 ED-0114（远端 Head `82c5c0c`，Worker02 正在补 Total-only 安全预算与 Runtime/ToolCall 的批准续跑门）、#23 ED-0113（远端 Head `a3d060f`，敏感字段门已补，Worker06 固定 Head 复审中）。
- 已并入 main 的 AgentRuntime 行为 PR：#15 ED-0107 EventProjector、#16 ED-0110 TurnCancellation、#17 ED-0104 Responses adapter、#18 ED-0106 Anthropic adapter、#19 ED-0108 Core-authorized ToolCall、#22 ED-0111 bounded retry；PR #12 ED-0141 同时把 SessionState/ContextSelector 测试注册进 CTest。
- 未推送的 ED-0112/R2 恢复实现 Head `9b31251` 已加入五种终态区分、Operation/idempotency reconciliation、请求/上下文漂移拒绝、Unsupported、安全字段 sentinel 与 single-writer 约束；Windows 全量构建与 CTest 16/16 通过，两个生产源覆盖率为 87.38%/94.48%。Worker06 固定 Head 复审与远端 PR 尚待完成。
- PR #14、#20、#23 外，ED-0108 已合并。当前 Worker01/02/03/05 与两个 Reviewer 槽并行推进 ED-0112、预算闭环、Boost.JSON、Memory scope、固定 Head 审查；所有改动位于隔离 worktree。
- Coordinator 对 `main@3fb4db8` 实际执行 Windows 原生 CMake configure/build 与全量 `ctest --test-dir H:/Qbot/EmulativeDialog.build/windows-agent-slice`：configure 0、build 0、14/14 tests 通过。`main@8f8ab66` 后续只新增了 ED-0112 合同 CR 与状态记录；结果包含 SessionState、ContextSelector、InstructionAssembler、EventProjector、ToolCall、TurnCancellation、ProviderOutcomePolicy、SQLite probe/store；尚未包含 Provider、Memory 与 Network 独立合同测试目录。
- Codex 上游基线/许可证仍由 `Workflow/CodexCapabilityMap.yaml` 锁定为 `openai/codex@970111f6cb62cc02fe36a82ea7d71b37a2889c97`、Apache-2.0；地图列出 15 项 Keep/Redesign（100% 已关联 TaskSpec），但关联不代表实现完成。
- 尚未闭环：ED-0105 Chat Completions adapter、ED-0112 恢复审查/PR、ED-0113 GroupMemory/tombstone 修复与 ED-0144 SQLite adapter、ED-0114 新 Head 复审/最终集成验证、Provider/Memory/Network 测试根 CTest、异步协程化 AgentRuntime、经授权的 QQ Adapter。CodexCapabilityMap 中所有未有合并 C++ 证据的 Keep/Redesign 继续保持未完成。

### 继续执行增量

- `origin/main = be2fd36`；PR #14 ED-0143 已 squash 合并（Boost JSON gitlinks 在固定 ExternalProject populate 阶段获取，verify 只读）。远端分支为 main + ED-0113 + ED-0114，数量为 3。
- PR #14 固定 Head `8de7a36` 已由 Worker04 APPROVE；热缓存根构建与 Boost.JSON CTest 3/3 通过，离线缺失依赖 fail-closed。冷缓存真实下载因本机 Schannel/TLS 断连未能完成，已如实记录。
- ED-0112/R2 恢复实现新 Head `9b31251` 已本地提交并基于 `main@5ac8966`：五种终态区分、Operation/idempotency reconciliation、上下文/请求漂移拒绝、Unsupported、秘密字段 sentinel、single writer；全量 CTest 16/16，两个生产源覆盖率 87.38%/94.48%。尚未推送，等待分支名额与独立固定 Head 审查。
- ED-0113/R4 PR #23 最新 Head `a3d060f` 已补敏感字段策略测试；Worker06 复审仍发现 GroupMemory consent 列表不能跨 Principal 读取 grantor 记录、RestoreBackup 可覆盖既有 tombstone、fixture 未真实执行；Worker05 正在修复。
- ED-0114/R2 PR #20 新 Head `1ba0cb0` 已处理 Total-only/partial usage 安全阻断、Runtime 与 ToolCallCoordinator 预算门、Core 批准的新 RequestId 续跑；待独立固定 Head 复审，并重基到最新 main。
- Coordinator 在 PR #14 合并后以独立 Windows 原生构建验证 `main@be2fd36`：configure 0、全量 build 0、CTest 17/17 通过（Boost.JSON 探针、缺 gitlink fail-closed、winpthread 链接检查均包括在内）。冷缓存 clone 受本机 Schannel/TLS 断连影响；热缓存 build 与离线失败关闭通过。
- AgentRuntime 当前仍为同步采样；异步 C++20/Boost.Asio 回合循环仍未实现。
