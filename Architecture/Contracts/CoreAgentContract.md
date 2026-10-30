# Core 与 Agent 合同 V1

状态：冻结候选（ED-0007@1；独立评审通过并合并后生效）。本合同规定产品运行时的 Core policy 与 C++ AgentRuntime 边界，不依赖 Codex 私有结构。

## 术语和信任边界

- `ChannelEnvelope` 的唯一 schema、必需字段和 Adapter 验证职责由 `ChannelAdapterContract.md` 定义。它必须由已登记的 `ChannelAdapterPort` 提交；来源验证失败、账号未登记或必要字段缺失时，不得把事件作为可信聊天请求接纳。
- AgentRuntime 消费 ChannelAdapterContract 定义的 `CanonicalMessage`，并将正文、引用文本、OCR/转写、附件内容、昵称、Provider 输出、工具参数/结果和记忆全部视为不可信数据。
- 只有 Core 能把 `(ChannelId, AccountId, SenderId)` 映射到产品主体并解析 `ConversationScope`、`MemoryScope`、策略与授权。模型自报的身份、账号、会话、群、角色或权限字段永远不构成授权。
- `MemoryWriteConsent` 不是 `ChannelEnvelope` 字段或 Adapter 声明。Core 仅能从绑定到已验证 Envelope、已映射 Principal 的当前 `CanonicalMessage` 中识别明确的记忆同意意图；正文仍是不可信输入，不能自证 Principal 或扩大授权。正文提出的目标 scope/类别只是请求，Core 必须将其解析并校验到该 Envelope 可授权的精确范围，存在歧义时先澄清。Core 必须记录已验证 Principal、精确 scope/类别、来源 MessageId、同意时间和 `ConsentId`。模型输出或 AgentRuntime 提案本身不构成同意。
- ProviderRequest 只含 Core/ContextSelector 选出的必要文本和内容部分；不发送原始平台对象、凭据、内部授权记录或真实渠道 ID。内部关联 ID 除非某协议映射明示且获准，否则不得进入远端请求。

## Core 的唯一所有权

Core 唯一负责：

- 渠道账号、可信 Envelope、发送者与产品主体的映射和会话路由；
- Provider/Channel profile、凭据引用和受保护凭据的解析。Core 只提供 scoped、不可序列化的 `CredentialLease`；Provider Adapter 只在单次请求内使用，Channel Adapter 可将连接所需凭据保留在当前进程的 active session 内存中，并必须在 Stop/profile change 时清除。两者都不得写入持久文件、日志或 Prompt。
- Action Allowlist、参数 schema、CapabilitySet、限频、一次性授权和审批；
- `ConversationScope` 与 `MemoryScope` 的解析、记忆授权、持久化事务和 `OperationRecord`；
- Provider/Channel Port 注册和版本化能力快照；
- 幂等键、外部副作用结果、`OperationState::UnknownOutcome`、`AgentTurn::OutcomeUnknown` 和恢复策略。

Core 不把身份、权限或副作用策略交给 prompt、Provider、AgentRuntime、Channel Adapter 或 TUI。`CoreAgentContract` 是 Core policy 的唯一权威定义。

Core 在调用边界解析 SecretRef 并提供 scoped `CredentialLease`。Provider Lease 只在单次请求期间有效；Channel Adapter 若连接需要认证材料，可在当前 active session 进程内存中持有至 Stop/logout/profile change。禁止复制到持久 profile、普通配置、Prompt、diagnostic、event、error 或 task log；内存凭据不得下发到 TUI。

## AgentRuntime 职责与请求流程

AgentRuntime 执行一个 `AgentTurn`：

1. 接受 Core 提交的可信 Envelope 与 CanonicalMessage；拒绝重复的 `StableDeduplicationId` 和无效会话路由。
2. 从 Core 取得解析后的 `ConversationScope`、当前 capability snapshot、显式允许的记忆记录和本轮策略摘要。记忆同意由 Core 根据当前已验证用户消息处理；AgentRuntime 和 Provider 只能提出待用户确认的记忆候选。
3. 由 ContextSelector 构造有界 ModelRequest；系统/开发规则不能被历史、摘要、模型输出或记忆覆盖。
4. 只通过 `ModelProviderPort` 发起一个或多个 ProviderRequest；消费规范化流事件和每请求唯一终态。
5. `ToolCallReady` 只表示完整的模型请求。Provider 终态为 `ResponseCompleted` 后，AgentRuntime 才能把 action proposal 交 Core 校验；Provider function call 从不授权执行。
6. Core 校验工具名、参数、schema、当前 AgentTurn、Envelope、作用域、能力、授权和频率。只有带有效 `AuthorizationRef` 的 `CoreAuthorizedAction` 可以交 `ChannelAdapterPort` 执行。
7. 工具结果仅在 Core 接收确定的 ChannelReceipt 后作为下一次模型输入；未知副作用结果不得作为成功回执注入。
8. AgentRuntime 形成面向用户的最终文字或已获准动作；所有出站发送由 Core/Channel Port 执行。最终成功要求对应的成功 ChannelReceipt。

## 授权操作和恢复

`OperationRecord` 至少保存 `OperationId`、`IdempotencyKey`、`AgentTurnId`、目标 Scope、ActionId、策略版本、可选的 `MemoryConsentId`、状态和脱敏结果类别。每个外部写操作必须先建立记录并执行 Core 授权，再开始调用 Adapter。

OperationState 为 `Prepared`、`Authorized`、`Dispatched`、`Succeeded`、`Rejected`、`Unsupported`、`Failed` 或 `UnknownOutcome`。进入 `Dispatched` 后若 Adapter 没有给出确定回执，Core 必须把记录置为 `UnknownOutcome`；此状态不可由 Agent 改为成功，也不得盲目重发。仅当同一 IdempotencyKey 的平台查询确认原动作未执行，或经用户重新授权建立新操作时，才可发起后续写操作。

## AgentTurn 状态机

| 状态 | 含义和允许的后续状态 |
|---|---|
| `Accepted` | Core 已验证 Envelope、主体、路由和去重；可到 `ContextSelected` 或拒绝终态。 |
| `ContextSelected` | 作用域和有界上下文固定；进入 `Generating`。 |
| `Generating` | 存在一个 ProviderRequest；收到完整工具提案后到 `AwaitingAuthorizedAction`，最终回复后到 `Sending`。 |
| `AwaitingAuthorizedAction` | Core 正在作策略判定或等待审批；拒绝/unsupported 可反馈给 Agent 继续生成，不代表发送成功。 |
| `Sending` | Core 已授权并调用 ChannelAdapterPort；成功回执可返回 `Generating`（工具结果续跑）或 `Completed`（最终回复），确定失败可到 `Failed`，unsupported 可到 `Unsupported`。 |
| `Completed` | 唯一成功终态；最终用户可见回复或动作具有成功 ChannelReceipt。 |
| `Failed` | 确定的 Provider、策略、持久化或 Adapter 错误终态。 |
| `Cancelled` | 取消发生在任何外部写操作 dispatch 之前，或取消已确定且无未决副作用。 |
| `Unsupported` | 当前 Action/目标能力明确不支持，且没有成功回执；Agent 不得宣称完成。 |
| `OutcomeUnknown` | Provider stream 或外部写操作在 dispatch 后无确定终态/回执；禁止自动重放。 |

AgentTurn 的终态集合固定为 `Completed`、`Cancelled`、`Failed`、`Unsupported`、`OutcomeUnknown`。恢复记录必须保留重启时的原始 phase、tool execution state 和 operation state；恢复决策不得把五个终态折叠成单一的 `AlreadyTerminal` 结果。

持久化记录不得把 `Completed`、`Cancelled`、`Failed` 或 `Unsupported` 与仍处于 `Dispatched` 的 tool execution 或 operation 状态组合。dispatch 后缺少确定结果/回执时必须记录为 `OutcomeUnknown`；无法验证该不变量的 checkpoint 必须 fail closed，不能生成确定终态决策或重放。

Tool execution 必须按 `NotRequested` → `Authorized` → `Dispatched` → `Completed`/`Failed`/`UnknownOutcome` 推进；不得跳过授权或 dispatch 来记作完成/失败。dispatch 后无确定结果时只能记为 `UnknownOutcome` 并设置 `UnknownSideEffect`。已持久化的 `ToolExecutionState::Dispatched` 不得在后续 revision 中清除或退回 `NotRequested`/`Authorized`，只能转为 `Completed`、`Failed` 或 `UnknownOutcome`。已成功的 operation 不能证明并行 Provider request 已结束；若 checkpoint 仍处于 `ProviderDispatched` 或 `ProviderStreaming`，恢复必须将 turn 隔离为 `OutcomeUnknown`，同时保留 operation 的 `Succeeded` 状态，并禁止重放。

Turn recovery checkpoint 的标识字段仅接受由可信 Core issuer 生成的域标签不透明 token：`turn_`、`request_`、`scope_`、`context_`、`operation_`、`idem_` 或 `action_` 后必须恰有 32 位小写十六进制值。普通文本、正文、凭据或未经域验证的引用不得写入 checkpoint 或恢复事件；Recovery component 不负责生成这些 token，生产 issuer/接线由独立 Core integration task 提供。多次工具调用必须保留每个已完成工具/operation 状态；只有一个已结束的工具周期经过新的 Provider request 到达 `ProviderCompleted`，并用新的 operation ID 和幂等键建立 `Prepared` 状态后，才可开始下一个工具周期；同一 ActionId 可以在新的获批 operation 中再次使用。

ProviderRequest 的 `ResponseCompleted` 表示该模型响应结束，不等于 AgentTurn `Completed`。带工具提案的响应由 Core 授权后续跑；最终答复只有成功发送回执后才算完成。状态记录关联 `AgentTurnId`、`RequestId`、`OperationId`、`ProviderProfileId` 和可用的 `ChannelReceiptId`。

ProviderRequest 的 `ResponseOutputLimited` 不得执行未完成的工具参数。AgentRuntime 只能在 Core 批准 ContextSelector 重新压缩/预算后，使用新 RequestId 发起有界续跑；若预算耗尽则 AgentTurn 以稳定 `OutputLimitReached` 错误结束。`ResponseFailed` 仅按 profile 明列的安全 retry policy 处理；`ResponseCancelled` 在无未决副作用时转 `Cancelled`；`ResponseOutcomeUnknown` 转 AgentTurn `OutcomeUnknown`，不重新生成或派发工具。

V1 的 Provider `TextDelta`/`RefusalDelta` 只作为 AgentEvent 内部流事件；不直接变为 ChannelAction。Core 只在完整 `ResponseCompleted` 后按 policy 审核最终文本/动作并授权发送。失败、截断、取消或未知终态不得把已有 partial text 当作成功回复。

## 取消、错误与恢复规则

- 取消在 ChannelAction dispatch 前发生：终止为 `Cancelled`，不得发送动作。
- 取消或进程中断发生在外部动作已 dispatch、回执未确认时：AgentTurn 终止为 `OutcomeUnknown`，不得重发。
- Provider stream 断开且没有终态时，AgentRuntime 不得假定响应已完成；不执行尚未被 ResponseCompleted 确认的工具提案。
- 重启恢复必须先加载持久化的 Turn/Operation 状态和 dedup 标记。只恢复安全的上下文关联；对 `Dispatched` 无回执记录执行 reconciliation，不重放。
- 所有错误使用稳定 ErrorCode 与脱敏说明；日志和崩溃报告不得含明文聊天、密钥或完整工具敏感参数。

## 本机 Core 接口

- UI/Core 通信仅使用带当前 Windows 用户 ACL 的版本化 JSON-RPC named pipe；禁止 loopback TCP。
- TUI 只调用 Core API；Provider/Channel 凭据不下发到 renderer。
- Core RPC 新增方法必须更新此合同、版本策略和调用方合同夹具。
- Provider 远端 HTTPS/WSS 是单独的出站连接，不得包装为本机 loopback 服务。

## V1 合同验收矩阵

- 模型声称的 SenderId/AccountId/权限与可信 Envelope 冲突时，Core 忽略模型字段并拒绝越权 Action。
- 未登记账号、未知 conversation、重放 dedup key、跨群/跨发送者 Scope 均失败关闭。
- 未授权/未声明工具、Schema 错误、超过频率限制和不支持 Channel capability 分别得到稳定拒绝结果。
- ProviderResponseCompleted 不会在工具结果回送前结束 AgentTurn；完整过程最多产生一个成功终态。
- 外部动作无回执、Provider stream 断连、取消、Core 重启分别得到确定状态；`OperationState::UnknownOutcome` 和 AgentTurn `OutcomeUnknown` 均不会自动重发。
- FakeProvider/MockChannel 的合同实现不需要依赖任一生产 Provider 或真实 QQ 账号。
