# 模型 Provider 合同 V1.2

状态：V1.2 候选。V1.1 是已审查的历史基线；下文保留 V1.1 请求、事件、错误和协议映射语义，V1.2 补充位于文末。V1.2 只补全 Chat Completions 与 Anthropic refusal 的协议映射，不改变 ModelProviderPort C++ 类型、CoreAgentContract V1 或授权行为，也不实现任何 Adapter。

## 职责和依赖方向

- `ModelProviderPort` 是 AgentRuntime、Core 与 Provider Adapter 之间唯一公共模型接口。AgentRuntime 只依赖此 Port，不依赖具体 Responses、Chat Completions 或 Anthropic 实现。
- Provider Adapter 是纯 C++ 协议边界：把 `CanonicalModelRequest` 编码成远端请求，并把响应流解码为 `CanonicalModelEvent`。
- Provider 不访问 Channel、Core 数据库、记忆存储、TUI、Windows Credential Store 或 Core 授权逻辑；不自行执行工具或平台副作用。
- `FakeProvider` 只在合同/行为验证中实现相同的 `ModelProviderPort`。它允许构造任意确定的事件序列/错误，不读取网络，也不引用任何生产 Adapter。
- Core 负责 profile、endpoint allowlist 与 CredentialStore。profile 只含不透明 `CredentialRef`；Core 在一次调用期间向 Adapter 提供短时、只读 `CredentialLease`，不把明文密钥写入 profile、Prompt、命令行、事件或日志。Adapter 只可把 Lease 放到协议要求的认证头中。

公共操作语义：

| 操作 | V1 语义 |
|---|---|
| `GetCapabilities(ProfileId)` | 返回本 Adapter 和该 profile 明确支持的 streaming、tool call、vision、usage、cancellation、output limit 等能力；未声明即不支持。 |
| `ValidateProfile(Profile)` | 验证协议、HTTPS endpoint、认证 scheme、模型和已声明参数；不访问用户私密正文。 |
| `StreamTurn(Profile, CredentialLease, Request, CancellationToken, EventSink)` | 发起一次 ProviderRequest，按单调序号递交规范化事件，并恰好产生一个终态事件。CredentialLease 由 Core 临时提供。 |
| `HealthCheck(Profile)` | 只验证受控连接状态；不得包含真实聊天正文、记忆或工具参数。 |

接口命名和同步/异步 C++ 类型由对应实现 TaskSpec 落定，数据与状态语义以本合同为准。

## Profile 与请求负载

`ModelProviderProfile` 至少包含 `ProviderProfileId`、`Protocol`、经 Core endpoint policy 批准的 HTTPS `Endpoint`、`ModelName`、`CredentialRef`、明确的 `AuthenticationScheme`、协议版本、明确的 `SamplingOptions`、`OutputLimit`、超时、retry budget、capability snapshot 和可选的 `CompatibilityProfileId`。V1 不接受任意 URL scheme、凭据 URL、自动跟随跨源 redirect 或未登记的供应商扩展。CompatibilityProfile 必须按 profile 注册并列出允许的额外字段，不能传任意 headers/body 字段。

`CanonicalModelRequest` 包含：

- `RequestId`、`AgentTurnId`、创建时间和可选的本地 `ConversationScopeRef`；这些是本地关联字段，默认不得序列化为远端身份资料。
- 经 Core/ContextSelector 选择的 `SystemInstructions`、`DeveloperInstructions`、有序 `Messages` 和 `ContentParts`。
- `ToolDefinitions`：只包含 Core 当前允许的业务动作 schema。Provider 和模型都不能据此授予权限。
- `ModelName`、`OutputLimit`、`SamplingOptions` 和 profile 中显式登记的协议选项。
- `CancellationToken` 和当前 provider capability snapshot；两者均为本地运行时数据，不写入 wire payload。

`Messages/ContentParts` 只含 Core/ContextSelector 明确选出的规范化文本或受控内容引用。V1 不传任意本机路径、原始平台对象或未登记的内容类型；若本 profile 没声明该 ContentPart 的支持映射，Adapter 必须以 `UnsupportedCapability` 终止请求，不得静默丢弃。

请求禁止包含原始 ChannelEnvelope、Channel/Account/Sender ID、未选择的历史聊天、密钥、Authorization header、未授权 MCP/Server Tool 定义或开发期 Task/Git/Build 信息。Provider 不得把外部 `tool_call` 当作授权凭证。

V1 `ContentParts` 必须类型化；文本始终可用。Image 输入仅在 Profile `Capabilities` 明确声明且 Core 提供获准内容引用时使用；Adapter 不得根据 URL 或路径自行下载/读取文件。Audio、video、任意文件和未知 ContentPart 在 V1 返回 `UnsupportedCapability`。

## CanonicalModelEvent V1.1

每个事件包含 `RequestId`、`AgentTurnId`、`SequenceNumber`、`EventType`，以及适用时的 `ItemId`、`ToolCallId` 和 payload。Adapter 创建本地 `SequenceNumber`：从 1 开始，每个已递交事件递增 1；同一响应中的文本项、拒绝项和工具调用保持稳定 ItemId。除协议映射特别声明外，协议私有字段不进入 Core/Agent 数据模型。

非终态事件：

| EventType | 必需语义 |
|---|---|
| `ResponseStarted` | 本 Provider 请求已收到流响应；不代表 AgentTurn 完成。 |
| `TextDelta` | 一个有序的可见文本增量，带稳定 `ItemId`。 |
| `RefusalDelta` | 模型拒绝内容的可见增量；拒绝不是 HTTP/传输失败。 |
| `ToolCallStarted` | 开始一项模型提出的 tool call，带稳定 `ToolCallId` 与名称。 |
| `ToolArgumentDelta` | 工具 JSON 参数增量；在完整参数确认前不可交 Core 执行。 |
| `ToolCallReady` | Provider 协议已结束该调用的参数块且 JSON 完整有效；仅代表候选动作，不代表 Core 授权。 |
| `UsageUpdated` | 可选 usage 快照。字段可为未知；缺失不等于零。 |

事件 payload V1.1：

| EventType | Payload |
|---|---|
| `ResponseStarted` | 可选 opaque `ProviderResponseId`；不含用户/渠道身份。 |
| `TextDelta` / `RefusalDelta` | 稳定 `ItemId`、增量 UTF-8 文本和可选有界分类；不可用作权限信号。 |
| `ToolCallStarted` | 稳定 `ItemId`、`ToolCallId`、`ToolName`。 |
| `ToolArgumentDelta` | `ItemId`、`ToolCallId`、原序参数 JSON 文本增量。 |
| `ToolCallReady` | `ItemId`、`ToolCallId`、`ToolName`、完整 JSON arguments。Core 仍须验证完整 schema 和 policy。 |
| `UsageUpdated` | `UsageSnapshot`：optional `InputTokens`、`OutputTokens`、`TotalTokens`、`CachedInputReadTokens`、`CachedInputWriteTokens`、`ReasoningOutputTokens` 和 `Completeness=Partial/Final/Unavailable`。 |
| ResponseCompleted | CompletionKind=FinalAnswer/ToolHandoff/Refusal，可选 stop sequence code、final UsageSnapshot 和可选类型化 RefusalStopDetails。RefusalStopDetails 只允许出现在 CompletionKind=Refusal。 |
| `ResponseOutputLimited` | `LimitKind=OutputTokens/ContextWindow`、final/partial UsageSnapshot 和可选已生成安全文本。 |
| ResponseFailed | FailureCode（仅允许 V1.1 failure-only 列表）、是否可安全 retry、脱敏说明；不含 provider-raw body。 |
| `ResponseCancelled` / `ResponseOutcomeUnknown` | 稳定 cancel/unknown reason code；不宣称远端副作用已停止或未发生。 |

RefusalStopDetails V1.1 是拒绝终态的类型化信息 metadata，不是可见文本或授权信号。该字段可独立于 RefusalDelta/TextDelta 表示，因此空拒绝文本仍可携带拒绝分类。

ResponseCompleted.RefusalStopDetails 是可选 payload；仅当 CompletionKind=Refusal 时允许出现。Anthropic stop_reason=refusal 时必须生成该结构；若 category 缺失、null 或不识别，规范化为 Unclassified。其他协议只有在各自映射明确规定时才提供该字段。

| RefusalStopDetails field | Type and rule |
|---|---|
| Type | Fixed literal Refusal. |
| Category | RefusalCategory; present only with CompletionKind=Refusal. |
| RefusalCategory values | Cyber, Bio, FrontierLlm, ReasoningExtraction, GeneralHarms, Unclassified. Unknown wire values map to Unclassified. |

Anthropic stop_details.explanation is unstable wire-only text and MUST NOT enter CanonicalModelEvent, prompts, logs, Memory, or Channel output. Arbitrary stop_details JSON is never passed through.

Canonical event 不包含任意 vendor JSON。协议专属字段只在本合同的映射表或 profile allowlist 中标准化；其他字段按 UnsupportedCapability/InvalidResponse 处理。

V1 不向 AgentRuntime 或 Channel 暴露原始 `ReasoningDelta`/thinking blocks。Profile 不请求隐藏推理内容；若协议随响应附带可选内部推理片段，Adapter 只能按本合同过滤且不得记录/持久化。若某响应没有剔除内部推理便无法正确解释，必须以 `UnsupportedCapability` 失败，不得把它作为文本发送。

每次 `StreamTurn` 恰好产生以下一个终态事件，之后不得再发事件：

| EventType | 含义 |
|---|---|
| `ResponseCompleted` | Provider 响应完整结束；可带 `CompletionKind = FinalAnswer / ToolHandoff / Refusal`。`ToolHandoff` 是 Provider 请求终态，但 AgentTurn 仍可继续。 |
| `ResponseOutputLimited` | 输出或上下文预算截断；不得当作正常最终答案。 |
| ResponseFailed | 请求明确失败。只携带 V1.1 failure-only FailureCode、Retryable 和脱敏说明。 |
| `ResponseCancelled` | 请求未发出，或 Provider 明确确认取消；不声称未知远端请求已停止。 |
| `ResponseOutcomeUnknown` | 请求已可能到达 Provider，但流在完整终态前丢失，或本地取消无法确认远端结果；禁止自动重发。 |

响应的正常结束、工具 handoff 和 AgentTurn 结束是不同概念。AgentRuntime 只有收到 `ResponseCompleted` 后才能处理该响应的已完整 `ToolCallReady`；仍须由 Core 对 ToolCallId、工具名、JSON schema、活动 AgentTurn、可信 Envelope、scope、授权和 capability 再验证。`ResponseCompleted` 不得直接结束带有待授权业务动作的 AgentTurn。

Canonical sequence 始终由 Adapter 生成。对于带上游 sequence 的协议（Responses），序号必须严格递增；完全相同的重复 wire event 可以忽略，缺号、乱序或相同序号但 payload 不同视为 `InvalidStream` 并以一个 `ResponseFailed` 终结。Chat Completions 与 Anthropic 按各自帧顺序重建，工具索引/Block index 映射到稳定 item/call ID。任何协议均不得合成缺失的文本、工具参数、usage 或成功终态。

## 错误、取消、重试和未知结果

V1.1 的 ResponseFailed.FailureCode 是封闭的公共 failure-only 枚举，表示请求明确失败。错误详情脱敏，不包含 key、完整私聊正文或完整敏感参数。

FailureCode V1.1 values:

| FailureCode | Meaning |
|---|---|
| InvalidProfile | Profile is invalid or unsupported by the adapter. |
| InvalidRequest | Canonical request is invalid before/during protocol mapping. |
| UnsupportedCapability | A requested, registered capability is unsupported. |
| AuthenticationFailure | Provider authentication was rejected or is unusable. |
| RateLimited | Provider rejected the request due to rate limits. |
| TransportFailure | A transport failure is confirmed and is not an unknown remote outcome. |
| InvalidStream | The received provider stream violates its protocol mapping. |
| ProviderRejected | Provider returned an explicit request rejection not represented by another code. |
| ContentFiltered | Provider completed with an explicit content-filter rejection. |


OutputLimitReached、Cancelled 和 OutcomeUnknown 不属于 FailureCode；分别且仅分别由 ResponseOutputLimited、ResponseCancelled 和 ResponseOutcomeUnknown 表示。
FailureCode 的终态归属互斥：

| Terminal event | Classification field |
|---|---|
| ResponseFailed | FailureCode only; excludes output-limit, cancellation, and unknown-outcome codes. |
| ResponseOutputLimited | LimitKind; never FailureCode. |
| ResponseCancelled | Cancellation reason code; never FailureCode. |
| ResponseOutcomeUnknown | Unknown-outcome reason code; never FailureCode. |

- Adapter 可按 profile 明列的 retry budget 重试明确未被远端接收的连接错误；HTTP 错误仅按协议明确标注的可重试类别处理。
- 只要已发出部分内容、Provider 已可能接受请求、已出现工具 call，或终态缺失，就不得自动重新提交相同请求；输出 `ResponseOutcomeUnknown` 或对应失败终态。AgentRuntime 不执行没有完整 Provider `ResponseCompleted` 的工具调用。
- 收到本地取消 token 后立即停止继续递交事件。若 Provider 取消得到确认，终态为 `ResponseCancelled`；已发出但无法确认时为 `ResponseOutcomeUnknown`。
- 错误、取消、截断和未知结果都只发生一个终态事件；未知结果不得伪装正常完成。

## 三协议规范化映射

| Protocol | Authentication | Request mapping | Streaming/tool and terminal mapping |
|---|---|---|---|
| OpenAI Responses | `Authorization: Bearer` with a Core-held profile key lease. | `instructions`、`input`、Core-approved `tools`、canonical OutputLimit -> `max_output_tokens` 和 profile allowlisted options；工具结果按原 `call_id` 序列化为 `function_call_output`。 | 按 event order 处理 output item、text/refusal delta、function-call argument delta/done；只在完整 arguments done 且 JSON 合法后发 `ToolCallReady`。校验上游 `sequence_number`。`response.completed` -> `ResponseCompleted`（含 ToolHandoff）；`response.incomplete`/`max_output_tokens` -> `ResponseOutputLimited`；`response.incomplete`/`content_filter`、未知 incomplete reason、`response.failed`、in-band error 或 HTTP error -> `ResponseFailed`。usage 仅记录协议提供值。 |
| OpenAI-compatible Chat Completions | `Authorization: Bearer` with a Core-held profile key lease. | V1 固定 `n=1`；canonical `system/developer/user/assistant` roles 映射到 `messages`，获准工具映射到 `tools`，OutputLimit 映射到 `max_completion_tokens`，仅发送 compatibility-profile 登记的额外字段。工具结果使用 `tool` role 且 `tool_call_id` 必须匹配相应 assistant `tool_call.id`。V1 不发已弃用 `max_tokens` 字段。 | 按 chunk 顺序处理 choice 0；`tool_calls` 分片依 `index` 组装，补齐初始 fragment 中可能缺失的 id/name/type，完整且 JSON 有效才发 `ToolCallReady`。`stop` -> Completed/FinalAnswer；`tool_calls` -> Completed/ToolHandoff；`length` -> OutputLimited；`content_filter` -> Failed/ContentFiltered；未知 finish reason -> Failed/InvalidStream。V1 不支持旧式 `function_call`。`include_usage` trailer 可为空 choices chunk且中断时可能缺失；缺失 usage 不补 0。 |
| Anthropic Messages | V1 profile 固定 AuthenticationScheme: XApiKeyHeader，只发 x-api-key（Core lease）；同时发 anthropic-version: 2026-10-21。V1 不用 Bearer fallback。 | canonical OutputLimit 映射到必需的 max_tokens；system 与 roles/messages/tools 映射到 Messages API。assistant tool_use 的结果只可回到紧接的下一条 user message；tool_result blocks 必须位于该 message content 最前，且 tool_use_id 一一匹配。 | message_start、content block start/delta/stop、message delta 和 message stop 按 Block index 重建稳定 ItemId。input_json_delta.partial_json 仅在对应 block stop 后解析；无效 JSON 不发 Ready。Server Tool/MCP/未知 content block -> Failed/UnsupportedCapability。end_turn/stop_sequence -> Completed/FinalAnswer；tool_use -> Completed/ToolHandoff；max_tokens/model_context_window_exceeded -> OutputLimited；refusal -> Completed/Refusal，并映射类型化 RefusalStopDetails（Type=Refusal）；category 缺失、null 或不识别时为 Unclassified。该 metadata 不要求存在 RefusalDelta；explanation 和其他未登记 stop_details 字段仅限 wire 层并丢弃。pause_turn -> Failed/UnsupportedCapability（本产品不续跑 server tools）；未知 stop_reason -> Failed/InvalidStream。message_delta.usage 是累计快照，后续已知字段替换而不求和；缺失值未知，0 才表示明确零。 |

#### Anthropic V1 profile 的参数约束

官方版本 `2026-10-21` 对 `temperature`、`top_p`、`top_k` 即使在默认值下也返回 400。V1 Adapter 必须从 wire request 中完全省略这三个字段；canonical sampling 若为未设置/协议默认值则直接省略，若请求非默认值则在发送前以 `UnsupportedCapability` 拒绝，不得改写成近似值。V1 `x-api-key` 方案被选作明确的 profile scheme；官方文档将其标为 legacy，但仍支持。工作区密钥策略由 Core profile/credential policy 决定，Provider 不保存明文密钥。

官方协议依据：

- OpenAI Responses create: https://developers.openai.com/api/reference/resources/responses/methods/create
- OpenAI Responses streaming events: https://developers.openai.com/api/reference/resources/responses/streaming-events
- OpenAI Chat Completions create: https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create
- OpenAI Chat Completions streaming events: https://developers.openai.com/api/reference/resources/chat/subresources/completions/streaming-events
- Anthropic API versioning: https://platform.claude.com/docs/en/api/versioning
- Anthropic migration to 2026-10-21: https://platform.claude.com/docs/en/api/migration-2026-10-21
- Anthropic authentication: https://platform.claude.com/docs/en/manage-claude/authentication
- Anthropic streaming: https://platform.claude.com/docs/en/build-with-claude/streaming
- Anthropic stop reasons: https://platform.claude.com/docs/en/build-with-claude/handling-stop-reasons
- Anthropic refusals and refusal stop_details categories: https://platform.claude.com/docs/en/build-with-claude/refusals-and-fallback
- Anthropic client tool call/result order: https://platform.claude.com/docs/en/agents-and-tools/tool-use/handle-tool-calls

不支持协议字段必须返回 `UnsupportedCapability` 或 `InvalidRequest`，不得静默丢弃或塞入公共 Core/Agent 数据模型。

## V1.1 离线合同夹具计划

- 三协议对同一 canonical request 的 request fixture 与事件 fixture；断言公共事件和 terminal 语义一致。
- Responses：正常文本、refusal、多个交错 function calls、response.completed、max_output_tokens incomplete、failed、HTTP/in-band error、sequence duplicate/gap。
- Chat Completions：`n=1`、角色和 tool_result round-trip、分片/交错多工具参数、finish_reason 矩阵、content_filter、include_usage 空 choices trailer、被截断时无 usage trailer、拒绝 function_call。
- Anthropic：版本/认证头、温度参数全部省略、非默认采样值在网络前拒绝、message/content block 生命周期、累计 usage、stop_reason 全矩阵、tool_use/tool_result 关联顺序、未知 block 拒绝、refusal StopDetails category 映射（含 null/unknown -> Unclassified）和 ResponseFailed failure-code 集合；空拒绝文本仍可带 RefusalStopDetails，explanation 不进入 expected canonical payload。
- 三 Adapter 与 FakeProvider 都验证同一 `RequestId`、本地单调 SequenceNumber、稳定 ItemId/ToolCallId、terminal exactly-once、取消和 outcome unknown；fixtures 不含真实密钥、用户私聊正文或网络录制。
- 这些 fixture 是实现任务的验收计划，本合同冻结任务不创建/运行测试，不替代独立 Provider live-smoke。


## V1.2 拒绝映射补充

本节是 V1.1 历史基线之上的 V1.2 规范补充。V1.2 profile 按 V1.1 全部公共请求、事件、错误、取消和未知结果规则执行，并采用本节对 Chat Completions 与 Anthropic refusal 的明确映射。未被本节明确修改的 V1.1 语义保持不变；本节不增加 CanonicalModelEvent 或 ModelProviderPort 的 C++ 类型。

### 共享拒绝终态

- `RefusalDelta` 仍是非终态、不可见于 Channel 的拒绝文本增量；它本身既不完成请求，也不授权或禁止任何 Core action。
- `ResponseCompleted` 的 `CompletionKind=Refusal` 表示 Provider 已明确完成一次拒绝响应，不是传输或 HTTP 失败。只有完整的协议终态才能产生它。
- `ResponseCompleted.RefusalStopDetails` 仍是可选、类型化 metadata，且只允许随 `CompletionKind=Refusal` 出现。若存在，`Type` 固定为 `Refusal`，`Category` 是封闭的 `RefusalCategory`：`Cyber`、`Bio`、`FrontierLlm`、`ReasoningExtraction`、`GeneralHarms` 或 `Unclassified`。该对象不是文本，也不是授权信号。
- 不得根据缺失的协议类别猜测原因或合成类别。协议没有提供拒绝类别时，可完成 `CompletionKind=Refusal`，但省略 `RefusalStopDetails`。协议明确要求拒绝详情的映射按下文执行。
- Anthropic explanation、推荐模型名、未经登记的 `stop_details` 字段及原始 JSON 都是 wire-only；不得把它们存入规范事件、Prompt、日志、Memory 或 Channel 输出。

### Chat Completions V1.2 refusal profile

V1.2 EmulativeDialog Chat Completions profile 延续 `n=1`，只处理 `choice.index=0`。按 chunk 读取 `choices[0].delta.refusal`：

| Wire 值 | 规范行为 |
|---|---|
| 字段缺失或 `null` | 不表示拒绝，不建立拒绝标记。 |
| 非空字符串 | 建立本请求的拒绝标记；把该非空文本映射为稳定 `ItemId` 上的 `RefusalDelta`。 |
| 空字符串 | 建立本请求的拒绝标记，但不发出空的 `RefusalDelta`。 |
| 非字符串且非 `null` | `ResponseFailed/InvalidStream`；不把值转换为字符串或透传。 |

`delta.refusal` 是拒绝内容，不是 `finish_reason`。即使已观察到拒绝字段，仍须等待终止 chunk；拒绝标记与干净的 `finish_reason=stop` 同时成立时，才产生一个 `ResponseCompleted(Refusal)`。Chat Completions 没有提供拒绝类别，因此不得为该终态构造 `RefusalStopDetails` 或 `Unclassified` 类别。

整个响应都必须符合单一内容类型：观察到拒绝后，任何 chunk 的非空 `delta.content` 或实际 `tool_calls` 都会与拒绝冲突；在拒绝之前已出现非空内容或工具调用、随后又出现拒绝，也同样冲突。冲突响应以一个 `ResponseFailed/InvalidStream` 终结，不产生 `ResponseCompleted(Refusal)`，也不得产生 `ToolCallReady`。这沿用既有终态与 final-only 交付语义；V1.2 不增加事件撤回或 partial-text rollback 机制。

| 最终状态 | V1.2 规范映射 |
|---|---|
| `finish_reason=stop`，未观察到拒绝 | 沿用 V1.1：`ResponseCompleted(FinalAnswer)`。 |
| `finish_reason=stop`，已观察到拒绝且没有混合内容/工具调用 | `ResponseCompleted(Refusal)`；不合成拒绝类别。 |
| `finish_reason=length` | 只映射 `ResponseOutputLimited`，不得完成为拒绝。 |
| `finish_reason=content_filter` | 只映射 `ResponseFailed/ContentFiltered`，不得完成为拒绝。 |
| `finish_reason=tool_calls`，无拒绝且工具调用完整有效 | 沿用 V1.1：`ResponseCompleted(ToolHandoff)`。若已观察到拒绝，则按上述冲突规则映射 `ResponseFailed/InvalidStream`。 |
| `finish_reason=function_call`、旧式 `delta.function_call` 或未知终止原因 | `ResponseFailed/InvalidStream`；不得降级成文本完成或工具成功。 |
| 流缺少完整终止原因 | 沿用 V1.1 的 EOF/未知终态规则；不得仅凭拒绝增量完成响应。 |

结构冲突校验先于成功完成判定。干净响应中的 `length` 只对应 `ResponseOutputLimited`，`content_filter` 只对应 `ResponseFailed/ContentFiltered`；`ResponseFailed` 不承载输出截断、取消或未知结果。每个请求仍恰好产生一个终态事件。

### Anthropic Messages V1.2 refusal profile

Anthropic `stop_reason=refusal` 是正常的 Provider 完成，不是 `ResponseFailed`。Adapter 必须等待 `message_stop` 才发出终态；拒绝的完成映射为一个 `ResponseCompleted(Refusal)`，并提供必需的 `RefusalStopDetails`：

1. V1.2 拒绝的 `message_delta` 必须携带 JSON object 类型的 `stop_details`，其 `type` 必须精确为 `refusal`。详情对象缺失、为 `null`、不是 object 或 `type` 不匹配时，以 `ResponseFailed/InvalidStream` 终止。
2. 对 `stop_details.category`，`cyber`、`bio`、`frontier_llm`、`reasoning_extraction`、`general_harms` 分别映射到 `Cyber`、`Bio`、`FrontierLlm`、`ReasoningExtraction`、`GeneralHarms`。缺失、`null` 或未知字符串映射为 `Unclassified`；其他 JSON 类型是无效协议值，映射到 `ResponseFailed/InvalidStream`。
3. `explanation` 只能是 string 或 `null`；其他类型按 `ResponseFailed/InvalidStream` 处理。无论内容如何，合法 `explanation` 都作为 wire-only 值丢弃，不解析、不作为文本发出，也不写入 Prompt、日志、Memory 或 Channel。`recommended_model` 是 fallback 专属 wire 提示；V1.2 不启用 fallback，不使用该值。除 `type`、`category`、`explanation` 和 `recommended_model` 外，`stop_details` 中的未知字段依照 V1 未知字段规则以 `ResponseFailed/UnsupportedCapability` 失败关闭，绝不透传原始 JSON。对非 refusal 的 `stop_reason`，不得把 `stop_details` 转成规范拒绝 metadata；非空的拒绝详情出现在其他 stop reason 上时按 `InvalidStream` 处理。

Anthropic 的 `message_delta` wire object 在 `delta.stop_reason` 之外可包含顶层 `stop_details`。只有在 `message_delta` 的 `delta.stop_reason=refusal`、详情对象有效并且随后收到 `message_stop` 后，才完成拒绝。Stream 在 `message_stop` 前中断时遵循 V1.1 终态/未知结果规则，不得提前生成 `ResponseCompleted`。已完成拒绝的详情仅描述 Provider 的拒绝分类，不构成工具调用、权限判断或出站动作。

### V1.2 官方协议依据

- OpenAI Chat Completions streaming schema：`delta.refusal` 为可选字符串或 `null`；`finish_reason` 与拒绝文本是不同字段。本合同中的混合值失败规则是 EmulativeDialog profile 的 fail-closed 约束。https://developers.openai.com/api/reference/resources/chat/subresources/completions/streaming-events
- Anthropic Messages API `stop_details` schema 与 API version：https://platform.claude.com/docs/en/api/messages/create
- Anthropic refusal 语义与官方类别 `cyber`、`bio`、`frontier_llm`、`reasoning_extraction`、`general_harms`，以及无类别时的 `null`：https://platform.claude.com/docs/en/build-with-claude/refusals-and-fallback

V1.2 follow-up fixtures 必须覆盖本节的 Chat refusal 空/非空/缺失/null、终止原因、混合内容/工具调用和非法类型，以及 Anthropic 有效/无效 `stop_details`、类别映射和 explanation 脱敏。本合同任务只修改本文件；fixtures、Adapter、Core policy 和 C++ Port 仍由各自任务负责。
