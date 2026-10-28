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

## 2026-10-28 继续执行状态（20:13 +08:00）

- 当前远端与本地 `main` 均为 `461735e50657bdc8ace070b46db3682cfb03460f`。PR #24（ED-0105/R7）已 squash merge；合并前固定 Base/Head 为 `4e4dbc7f26066f30d71da97a838e5f46e211a29b` / `cbe69782fdcba391bfe6ed99455cd19fe2bd7179`。Worker03 独立 Windows C++20 configure/build/CTest 1/1、gcov JSON v2 958/1046（91.59%）通过；Worker06 固定头审查 APPROVE。PR 描述记录了四路径 diff、两份 ReviewBundle 和本地验证。远端任务分支已删除。
- 远端分支共 3/4：`main@461735e`、`Task/ED-0114-R2-UsageLimits@a23f252`、`Task/ED-0144-MemorySqliteAdapter@9dc44d3`。开放 PR 为 #20 和 #25；不创建新远端分支。
- ED-0114 正在执行 TaskRevision 4（`CR-ED-0114-R3-COVERAGE-001`），仍用 PR #20 原分支。CR 保留 17 个最终差异路径，将共享 usage-normalization 测试移到 AgentRuntime 写根，补 Core ModuleId，并拆开 RequiredBuildTargets 与 RequiredCTestTests；CoreChatService 多对象覆盖率按唯一源码行统计，最终在全新 R4 build 验收。Worker02 当前 10 个未提交路径中，usage 测试仍待按 CR 从 Providers 父目录迁到 Agent 目录；其余代码已覆盖缺失/Unavailable usage、未 BeginRequest/已清理 turn sampling fail-closed、工具派发关闭与 Core turn 清理。四组定向行为目标通过；最终 R4 fresh build、CTest 和 7 个生产源 gcov 覆盖率尚未跑。作者先不重基；等本 TaskSpec 提交进 main 后再更新现有 PR 分支。
- ED-0144 的 PR #25 R2 固定 Base/Head 为 `4e4dbc7` / `9dc44d3`。Worker06 固定头审查发现 Major：`RecallBackup` 路径丢失 `ExpiresAt`，恢复后可能让有 TTL 的记忆变成永久记录；旧 gcov 目录的 LastTest 日志另有 libgcov checksum overwrite 警告，旧覆盖率证据不能用于 R3。已接受 `CR-ED-0144-R3-TTL-001`，保留四路径 WriteSet/物理 schema，改走语义等价的 `ListActiveRecords` 完整记录转换并增加未来 expiry 经备份、重启、恢复及到期后不可召回的真实 SQLite 测试。Worker05 实现、Worker03 验证预检、Worker06 新 Head 复审在原 PR 分支上继续；新鲜 R3 configure/build/CTest/gcov 尚未运行。
- 任务 Spec/事件由 Coordinator 单写者更新；Worker01 独立审查本次 CR/TaskSpec，Worker04 为 ED-0114 固定头 Reviewer。六槽当前任务分别为：Worker01 ED-0114 Spec review、Worker02 ED-0114/R4 实现、Worker03 ED-0144/R3 验证预检、Worker04 ED-0114/R4 复审、Worker05 ED-0144/R3 实现、Worker06 ED-0144/R3 复审准备。所有子代理继承主会话模型与推理强度；宿主不暴露实际显示模型标签，未猜测具体名称。
- 产品状态仍是聊天 Agent 重写未完成：main 已有同步单回合 AgentRuntime 与 QQ Mock → Core → FakeProvider → Mock 回执纵向切片。异步 C++20/Boost.Asio AgentRuntime、经 Core 授权的上下文压缩后正向续跑、其余 Keep/Redesign 能力和真实 QQ Adapter 尚未全部实现；微信保持 Deferred。官方 Codex 基线为 openai/codex@`970111f6cb62cc02fe36a82ea7d71b37a2889c97`（Apache-2.0）。

## 2026-10-28 当前协调快照（14:29 UTC）

- Git 基线仍为 `main=origin/main=461735e50657bdc8ace070b46db3682cfb03460f`。远端分支 3/4：`main`、`Task/ED-0114-R2-UsageLimits`、`Task/ED-0144-MemorySqliteAdapter`；GitHub PR #20 和 #25 仍 Open。GitHub 登录已核实；当前两个 PR 没有 GitHub review submissions、check-runs 或 commit statuses。PR 分支相对 `main` 均落后，更新前先按各自固定任务基线审阅 rebase 语义。
- ED-0124/R9 固定头 ReviewBundle Manifest SHA256 为 `4EF7F4583E66307DFAEA51408E7FA2AE0218795CF8BA40AE70E0F95D992E5BE7`。Worker06 对固定 Base `8029d6f` / Head `5b45482` 的独立 bundle 审查为 Approve；Worker03 在 Head `5b45482` 的新鲜 Windows C++20 配置、构建、聚焦 CTest 2/2 和 gcov JSON v2 核验均通过。对应 `ReviewBundleCreated` 与 `P2ExitGatePassed` 事件已追加到 P2 JSONL。P2 只确认首条 QQ Mock→Core→C++ AgentRuntime→FakeProvider→Mock 回执纵向切片，不表示重写完成。
- P3 EntryGate 仍 Pending。Worker03 对已合并 PR #8 的 ED-0120/R2 固定 Base `f47067d` / Head `688df03` 做了 post-merge 独立复审，结果 RequestChanges；ReviewBundle Manifest SHA256=`A63408EAFF0D83CF7075D45209CA10E87C84F0EE1D93D28C366C77EEE741C0BE`。阻断项是 `HttpTransportPort` 不强制 resolver：fake 可用 `nullptr` 并返回成功，导致 endpoint grant 验证可绕过。Round 1 系统代理/直连和 Round 2 Proxy-Authorization、Host/Content-Length、压缩语义已闭合；R2 WriteSet 的 `.h` 与实际 `.hpp` 也需在修订中对齐。PR #8 已合并且 GitHub review 列表为空，但合同不得标 Frozen。
- 修复已作为独立任务 ED-0146/TaskRevision 1 提出：修改 Port 模板方法、WinHTTP 适配和三个 Provider 的测试 fake，强制 Core grant 验证，WriteSet 共 12 路径并要求 `WinHttpTransport.cpp` 覆盖率至少 80%。TaskSpec 仍 Proposed，等待 Owner/独立 Reviewer 对固定头审查；在此之前不能开始其实现。ED-0114/R4 与 ED-0144/R3 TaskSpec 也只在 Coordinator 未提交候选中；作者槽 Worker02、Worker05 均已停在原隔离工作树，未提交、重基或推送。不得把候选当作已冻结任务。
- 当前协调分支 `Task/ED-0146-GrantBoundaryTaskSpec` 仅含 Workflow/Plan/Event/状态候选；尚未提交、推送或创建 PR。下一步先完成 gate/contract 账本审查并提交一个固定头治理 PR；远端分支额度只剩 1 个。
- Agent 槽按可并行且 Ready 的独立任务使用，不为“占满”分派无关工作。当前子代理均继承主会话模型和推理强度；宿主未返回可核实的具体显示模型标签。
- 聊天 Codex Agent 重写仍未完成：当前 main 的 AgentRuntime 纵向切片是同步实现。异步回合循环、取消与恢复的完整统一生命周期、三协议 adapter 的最终合同兼容复验、Core 授权工具往返、Keep/Redesign 能力映射全量验收及授权后的真实 QQ Adapter 仍待继续；微信保持 Deferred。

## 2026-10-28 当前协调快照（15:42 UTC）

- `main` 与 `origin/main` 基线为 `461735e50657bdc8ace070b46db3682cfb03460f`。治理候选位于 `Task/ED-0146-GrantBoundaryTaskSpec`；远端仍只有 `main`、PR #20 `Task/ED-0114-R2-UsageLimits`、PR #25 `Task/ED-0144-MemorySqliteAdapter`，合计 3/4。当前候选尚未推送或建立 PR。
- ED-0146/R1 的 Owner、独立 Reviewer 和治理复核均为 RequestChanges，已记录到 P2 事件分片。ED-0146/R2 将 Port fail-closed guard 与 Core grant authority 拆成两个任务，移除无关写集，按四个独立 CMake 项目冻结五个实际测试目标，并为 public Port header 与 WinHTTP production source 分别指定 gcov JSON v2 覆盖率命令。R2 仍 Proposed，等待对固定提交的 Owner/Reviewer 审查。
- 新增候选 ED-0147/R1（Core 注册 profile 后签发并校验 grant）和 ED-0148/R1（Memory 中 ProposalId optional 精确匹配）。ED-0147 是 P3 EntryGate 前置；ED-0148 仅阻塞 durable Memory 激活，不属于 HTTP EntryGate。相关 P3 proposal 事件已归入 P3 分片，事件 ID 无重复且 PhaseId 与分片一致。
- P3 EntryGate 保持 Pending，必须等 ED-0146 与 ED-0147 的合同和实现均通过验证。ED-0114/R4 TaskSpec 的 Worker02 Owner 与 Worker04 独立 Reviewer 已对 Base `461735e`、Head `204cfe4` Approve；但其实现仍受 P3 gate 阻塞。PR #20 的 R2 固定 Head `a23f252` 被 Worker03 RequestChanges，发现 usage 缺失 fail-open、自授权续跑、turn ledger 无生产清理、拒绝 Handoff 后 pending 未清理、WriteSet/coverage 清单不匹配；R4 源码未完成新鲜验证和固定头审查。
- PR #25 仍 Open。ED-0144/R3 继续被 P3 gate、未合入的 TaskSpec 与 ED-0148 consent 修复阻塞；其原 PR worktree/分支保持原样。
- 本次只验证任务查询、YAML 任务解析、JSONL 结构和 `git diff --check`，没有运行 C++ 构建或测试。代码重写仍未完成；main 上已有同步 AgentRuntime 纵向切片，异步 C++20 AgentRuntime 生命周期及三种 Provider 的最终 wire adapter 等能力仍待实现与验收。下一步是固定候选提交，完成三组独立 TaskSpec 审查，然后建立正式 PR 并尽快进入 C++ 实现任务。
