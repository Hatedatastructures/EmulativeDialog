# 渠道 Adapter 合同 V1

状态：冻结候选（ED-0007@1；独立评审通过并合并后生效）。V1 冻结统一 ChannelAdapterPort、可信入站 Envelope、能力与回执语义，并为第一条测试切片定义 MockChannel；不实现任何真实平台接入。

## Port 和职责边界

`ChannelAdapterPort` 是 Core 与具体 Channel Adapter 之间的公共边界。一个具体 Adapter 只代表一个经确认的 IntegrationMode；其接口语义为：

| Operation | Contract |
|---|---|
| `GetCapabilities(AccountProfile)` | 返回该精确账号、IntegrationMode、认证/审核状态和时间窗口对应的 capability snapshot；没有证据的能力必须为 Unknown/Unsupported。 |
| `Start/Stop` | `Start(AccountProfile, CredentialLease)` 建立连接并返回脱敏状态；`Stop` 释放连接。重复 Start/Stop 有确定、幂等的状态结果；不能改变授权或 scope。 |
| `SubscribeEvents` | 只发布通过平台源验证的 `ChannelEnvelope` 与规范化 CanonicalMessage；不转发原始平台对象。 |
| `SendAction(CoreAuthorizedAction, CredentialLease)` | 执行 Core 已批准的一项动作并返回唯一、脱敏的 `ChannelReceipt`。 |
| `HealthSnapshot` | 返回脱敏的连接/认证/限流状态；不包含凭据、完整私聊正文或媒体内容。 |

Core 是产品主体映射、会话路由、MemoryScope、allowlist、审批、限频和副作用策略的唯一 owner。Adapter 只负责验证平台事件来源、规范化内容、声明本账号实际能力和执行已授权动作；不得自行扩大授权，也不得依靠模型 prompt 做身份判定。

## Trusted ChannelEnvelope V1

每个入站消息拆分为不可变的 Envelope 和不可信正文：

| Field | Semantics |
|---|---|
| `ChannelId`, `IntegrationMode`, `AccountId` | 已配置并经 Core 登记的渠道/接入模式/账号标识；均为不透明字符串。 |
| `ConversationId`, `ConversationKind` | 平台给出的精确 conversation 标识及私聊/群类型；不从正文推断。 |
| `SenderId` | Adapter 从已验证平台事件提取的 sender 标识；昵称或正文自称不能代替。 |
| `MessageId`, `PlatformEventId`, `Timestamp` | 原平台事件标识与时间；用于审计/排序，不作跨账号身份关联。 |
| `StableDeduplicationId` | 对同一平台事件稳定；Core 用它拒绝重放，Adapter 不得因重连为同一事件生成新值。 |

Adapter 只能在平台来源与当前 IntegrationMode 验证成功后递交 ChannelEnvelope。Core 仍须验证 `ChannelId/AccountId` 已登记、Sender/conversation 路由一致并通过去重，再解析产品 Principal。Provider、AgentRuntime、TUI 和消息正文不得构造可信 Envelope。

与 Envelope 配对的 `CanonicalMessage` 才包含 `MessageParts`、`ReplyToId`、`MentionedIds` 和允许的 `PlatformMetadata`。这些内容均是不可信输入，只保留经允许列表规范化的文本/媒体引用；不把完整原始平台对象、未许可引用、认证信息或个人资料自动加入 ProviderRequest。

`ChannelEnvelope` 不包含 `MemoryWriteConsent` 字段；Adapter 只验证并提供来源元数据，不得推断或代报用户记忆同意。Core 在校验 Envelope、映射当前 Sender 到 Principal 后，才可把当前 `CanonicalMessage` 中明确的用户记忆控制意图识别为同意、确认或撤销。Provider/AgentRuntime 输出、历史授权和正文中的身份/权限声明均不是当前用户同意。

## CapabilitySet 和动作授权

CapabilitySet 必须关联具体 AccountId、IntegrationMode、SnapshotVersion/时间和取证状态。至少可分别声明 `ReceiveText`、`ReceiveMedia`、`ReceiveSticker`、`SendText`、`SendRichText`、`SendMedia`、`SendSticker`、`AddReaction`、`Poke`、`LookupDisplayName` 与 `SendProactiveMessage`。能力未知或未证实等同于不支持。

V1 `ChannelAction` 至少包含 `ActionId`、`OperationId`、`IdempotencyKey`、`TargetScope`、ActionKind、允许的正文/媒体引用和 Core 签发的 `AuthorizationRef`。Adapter 必须校验：

1. `AuthorizationRef` 来自当前 Core 注册、与当前 AgentTurn/Operation/TargetScope 匹配且未过期；
2. ActionKind 在该账号 CapabilitySet 中受支持；
3. Adapter 已验证的账号/会话与 TargetScope 匹配；
4. 频率、窗口、条数和平台审核限制满足当前 profile。

V1 不把 Provider 流式 `TextDelta` 直接作为出站动作。SendText 仅在 AgentRuntime 得到完整 Provider `ResponseCompleted` 且 Core 对最终内容/目标 scope 授权后创建；不完整或失败的 Provider stream 不产生“发送中”的成功声明。

`CoreAuthorizedAction` 是 Core 包装后的 ChannelAction + AuthorizationRef + 当前 policy/capability version；只有 Core 可构造/刷新 AuthorizationRef。它不是 Bearer/Channel credential，也不应暴露给模型。

Adapter 的这些检查是对 Core 授权的执行约束，不能代替 Core policy。工具模型输出、Channel id 文本或 Adapter capability 不能自行创建 AuthorizationRef。Unsupported、Forbidden 或 RateLimited 必须在能确认未发出副作用时返回相应状态；无法确认是否已送达时只能返回 UnknownOutcome。

## ChannelReceipt 与副作用生命周期

每次 `SendAction` 返回且只返回一个 `ChannelReceipt`，包含 `ActionId`、`OperationId`、可选的脱敏 PlatformReceiptId、发生时间、Status 和稳定 ErrorCode。Status 为：

- `Succeeded`：平台确认动作完成；仅此状态允许 Core/Agent 声称该动作成功。
- `Rejected`：平台明确拒绝，且 Adapter 能确认该动作未生效。
- `UnsupportedCapability`：账号能力未声明/不支持，且确认未发出动作。
- `Forbidden`：Core authorization 无效或平台权限不足，且确认未生效。
- `RateLimited`：已确定动作未生效并有可选的重试时间；必须受 Core retry policy 控制。
- `Failed`：确定性失败，Adapter 能确认动作未生效。
- `UnknownOutcome`：动作已开始发送或平台结果可能产生，但没有确定回执/查询结果。

若 `SendAction` 在 dispatch 后超时、连接断开、进程退出或取消，而平台不能确认是否生效，必须返回 `UnknownOutcome`。Core 把 OperationRecord 保持为 UnknownOutcome；Agent 不得伪造成功或盲目重发。只有平台幂等查询确认未发生，或用户重新授权一个新操作，才能再次发送。

## MockChannel V1

`MockChannel` 是 `Tests/Contract/Channels/Contracts/` 中的合同 test double，使用 `IntegrationMode=Mock`，不登录、不访问网络、不持有真实凭据，也不进入产品发行包。它实现同一 Port，可配置可信测试身份、CapabilitySet 和确定回执；不能绕过 Core 提供可信 Envelope。

最低场景：

1. 合法 Mock envelope 通过 Core identity/scope policy；获准 `SendText` 返回与 ActionId/OperationId 对应的 `Succeeded` receipt。
2. CapabilitySet 未声明的动作返回 `UnsupportedCapability`，没有发送或成功记录。
3. 错误 scope/AuthorizationRef 返回 `Forbidden`，无副作用。
4. 模拟 dispatch 后无回执时返回 `UnknownOutcome`；同一 OperationId 不自动重发。
5. 重放同一 `StableDeduplicationId` 不产生第二个 AgentTurn/动作。
6. 错误 receipt 不含原始正文、密钥或完整媒体路径。

## 平台范围

- `Mock` 是测试 IntegrationMode，不代表真实平台能力。
- 当前产品渠道是 QQ；具体账号模式、授权方式、收发窗口和社交动作能力由 ED-0003/ED-0004 以官方证据确认。V1 不在本合同中宣称 QQ Bot 或个人 QQ 的具体 capability。
- `ChannelWeChat` 保持 Deferred；本合同不调查或实现任何微信 IntegrationMode，也不把其能力作为当前发布依赖。
- 任何表情、贴纸、反应、拍一拍、昵称查询、媒体或主动消息必须先有该 IntegrationMode 的能力证据和 Core policy；否则返回 UnsupportedCapability。

## 隐私和日志

- Channel credentials 只通过 Core 受保护凭据引用和 `CredentialLease` 交给 Adapter；若维持已启动连接确需令牌，可只保存在 Adapter 当前进程的 active session 内存，并在 Stop/logout/profile change 时清除。不得写入普通配置、命令行、ProviderRequest、ChannelEnvelope 日志或事件记录。
- 调试/事件只记录 MessageId/OperationId、CapabilitySet 版本、状态码、时间和脱敏错误，不记录完整私聊正文、媒体或密钥。
- 删除/忘记只声明 EmulativeDialog 可删除的数据范围；不声称已删除平台自身保存的内容。
