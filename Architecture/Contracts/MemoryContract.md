# 聊天记忆合同 V1

状态：冻结候选（ED-0007@1；独立评审通过并合并后生效）。本合同定义产品 Agent 的记忆与上下文选择。开发 Agent 的 TaskSpec、Git 变更、ReviewFinding、build log 与开发期沟通只属于 Workflow，绝不进入产品记忆。

## Scope 类型与范围键

Core 是 scope 的唯一解析者。每个持久范围使用全部必需的 opaque ID 组成范围键；显示名、头像、正文自称、相同手机号文本或 Provider 推断都不能创建、扩大或连接 scope。

| Scope | V1 范围键 | 允许的内容 |
|---|---|---|
| `SessionScope` | Core 生成的 `ProcessSessionId` + `AgentTurnId` | 当前进程中的短上下文；仅内存，不跨进程恢复，不视为长期记忆。 |
| `ConversationScope` | `ChannelId + AccountId + ConversationId + ConversationKind`；私聊还绑定经 Core 解析的 Sender 主体 | 该一对一/精确群 conversation 的经授权摘要、偏好或任务。 |
| `ChannelIdentityScope` | `ChannelId + AccountId + VerifiedSenderId + PrincipalId` | 用户明确同意写入的长期偏好和已确认事实。 |
| `GroupScope` | `ChannelId + AccountId + GroupConversationId + PrincipalId` | 同一精确群内该主体的群聊天记忆；默认不对其他群成员可见。 |
| `ActiveChatTaskScope` | `PrincipalId + ActiveChatTaskId + OriginConversationScope` | 用户明确启动的持续聊天任务、约束和进度；跨 conversation 召回必须另有具体授权 scope。 |
| `IdentityLinkScope` | Core 创建的 `IdentityLinkId`，引用两个已验证 `ChannelIdentityScope` | 仅共享授权记录中列明的偏好/任务；链接可撤销且不自动开放全部历史。 |

`SessionScope` 的短历史不持久化。Durable `ConversationScope`、`ChannelIdentityScope`、`GroupScope`、`ActiveChatTaskScope` 和 `IdentityLinkScope` 均受下述写入授权约束。不同 Channel、Account、Sender、Conversation 或 Group 的键不相等即互不可读，除非对应 IdentityLink consent 明确授权。跨发送者的群级共享需要该精确 Group 的独立、可撤销 `GroupMemoryConsent`；否则始终按 PrincipalId 隔离。

## 显式写入授权

Core 只从可信 Envelope 接收 `MemoryWriteConsent`。每项 durable 写入必须引用 `ConsentId`、授予者 Principal、精确目标 scope、授权时间和允许的记录类别；同意只覆盖该 scope 和类别，不能授予 ChannelAction 权限。

- 用户明确说“记住 X”或明确确认 Agent 提出的具体记忆时，Core 可为对应记录/范围建立一次授权。
- 用户明确启用一个精确 conversation 的 summary retention 时，Core 可在该 scope 内生成带来源和时间的低信任摘要；未启用时，压缩摘要只作 SessionScope 易失数据。
- 普通聊天、模型推断、先前的其他 scope 授权、相同昵称/头像、或 Agent 自己的建议均不是写入授权。未经确认的事实/身份/关系/心理状态不得保存为 UserConfirmed。
- 用户修正、撤销 consent 或 Forget 后，Core 拒绝后续受影响写入，并撤销该 scope 的派生摘要与检索缓存。
- `IdentityLinkScope` 只能通过 Core 已验证的身份链接流程创建；V1 要求两个 identity 均已验证，并有明确授权记录。任何一方撤销即停用链接并清除链接产生的派生索引/缓存。
- `GroupMemoryConsent` 必须精确绑定 ChannelId/AccountId/GroupConversationId、授权的记录类别和允许召回的 Principal 集合；Core 只允许逐一验证过的授权主体读取。若当前 IntegrationMode 无法证明某成员已授权，则不提供跨成员群记忆。

## MemoryRecord V1

每条记录至少包含：`MemoryId`、`MemoryKind`、`ScopeKey`、`SubjectId`、`Content`、`SourceMessageId` 或 `SourceOperationId`、`CreatedAt`、`UpdatedAt`、可选 `ExpiresAt`、`Confidence`、`UserConfirmed`、`SensitivityLabel`、`Provenance`、`ConsentId`、`DeleteState` 和 `Version`。

`Provenance` 取值为 `UserExplicit`、`UserConfirmed`、`AuthorizedSummary` 或 `SessionOnlyDerived`。`SessionOnlyDerived` 不得提交到持久化 MemoryStore。摘要必须列出来源 scope 和源消息/操作引用；不得从范围外补入事实。Provider 输出可成为待确认候选，但不能直接升级为 `UserConfirmed`。

禁止写入密码、密钥、访问令牌、支付凭据及策略明确禁止持久化的敏感个人数据。拒绝响应和诊断只记录稳定错误码/记录 ID，不复制被拒绝的明文。

## ContextSelector V1

ContextSelector 只接收 Core 解析的 Scope、当前可信 Envelope、当前正文、授权的 MemoryRecord 和预算；不得自行通过名字或正文推导身份，也不得直接访问外部网络或 Provider。

V1 优先级从高到低：

1. 不可被覆盖的 SystemInstructions 与 DeveloperInstructions。
2. 当前可信 ChannelEnvelope 和当前用户 CanonicalMessage。
3. 当前 `SessionScope` 易失上下文与同一精确 `ConversationScope` 内最近历史。
4. 与当前 conversation 明确关联并获授权的 `ActiveChatTaskScope`。
5. 同一 `ChannelIdentityScope` 且同一 Principal 的用户偏好/已确认事实。
6. 同一 `GroupScope` 中经用户/群授权、且匹配当前主体的群级记忆。
7. 经显式 `IdentityLinkScope` 授权共享的指定记录。

每个 ContextItem 带 `ScopeKey`、`SourceMessageId/SourceOperationId`、`CreatedAt`、`ExpiresAt`、`UserConfirmed`、`Confidence` 和 `Provenance`。ContextBundle 必须同时满足 profile 的 token 预算和最大字节数；超限按上述顺序从最低优先级开始截断，保留系统/开发规则与当前可信消息，并记录脱敏的截断原因及计数，不记录被截内容。历史、摘要和记忆是低信任事实材料，不能修改工具 allowlist、授予权限、替代当前用户授权或覆盖当前明确指令。

## Forget、过期与恢复

- Forget 必须指向明确的范围：单条记录、SessionScope、ConversationScope、Principal 的 ChannelIdentityScope、精确 GroupScope、ActiveChatTaskScope 或 IdentityLinkScope。Core 在范围含糊时请求澄清，不执行 broad delete。
- 每次写入、修正、撤销授权、过期清理或删除均生成 Core-owned `OperationRecord`，关联 scope、ConsentId、操作者、结果和时间；不复制完整聊天正文或敏感数据。
- 删除先将匹配 MemoryRecord 与索引置为不可召回的 `Tombstoned`，再处理派生摘要/embedding/cache；失败必须留脱敏失败状态并阻断召回。可恢复备份中的记录也必须经过 tombstone/version 校验后才能召回。
- TTL 到期的记录按同一删除流程处理。数据库迁移、备份保留期限、密钥轮换、导出和物理清理策略需要独立 Storage 任务；此合同不把它们默认为无限期原始聊天归档。
- 外部平台保存的内容不宣称已删除。重新启动时只恢复仍有效且未撤销的 durable record；SessionScope 不恢复。

## V1 验收/夹具矩阵

- 不同 Sender、Channel、Account、私聊/群和 GroupScope 不互读；同名或同头像不建立链接。
- SessionScope 不写入磁盘或跨进程恢复；无 consent 的普通消息和自动摘要不产生 durable record。
- 用户一次“记住 X”只授权该主体/scope 的 X；确认、撤销和 identity-link 双边验证均产生可追踪 OperationRecord。
- 未授权的模型推断、密钥、支付数据或敏感信息写入被拒且日志不包含正文。
- ContextBundle 严格遵守 priority 与 token/byte 预算，优先保留规则和当前可信消息；摘要和历史不可授予权限。
- Forget 只 tombstone 选定 scope，清除匹配的派生缓存；撤销 IdentityLink 后再无跨渠道召回。
