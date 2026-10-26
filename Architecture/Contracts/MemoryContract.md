# 聊天记忆合同 V1

本合同定义产品 Agent 的聊天记忆。开发 Agent 的任务卡、Git 变更、审查和 build log 属于 Workflow，永远不进入产品记忆。

## 记忆范围

- ChannelIdentityScope：一个渠道、账号和可信发送者的长期用户偏好。
- ConversationScope：一个精确的私聊或点对点 conversation。
- GroupScope：一个精确群/频道；平台账号和群身份必须都匹配。
- ActiveChatTaskScope：用户在聊天里显式表达的持续任务、约束和进度，可在用户明确授权的关联会话中继续。
- IdentityLinkScope：用户明确把两个渠道身份关联后，才允许共享指定的偏好或任务记录；身份链接可撤销，撤销须清理派生缓存。

昵称、头像、相同显示名、文本中自称同一人、相同手机号的未经验证文本，都不能自动创建 IdentityLinkScope。

## 记忆记录模型

每个 MemoryRecord 至少具有：

- MemoryId、MemoryKind、Scope、SubjectId。
- Content、SourceMessageId 或 SourceOperationId。
- CreatedAt、UpdatedAt、ExpiresAt（可选）。
- Confidence、UserConfirmed、SensitivityLabel。
- Provenance、DeleteState、Version。

记录内容仅能是经过 policy 过滤的事实、偏好、摘要或活跃聊天任务。密钥、密码、访问令牌、支付信息和不允许保存的敏感个人信息必须拒绝写入。

## 写入规则

- 用户明确要求记住、修正或忘记时可以写入相应作用域。
- 用户明确提供并确认的偏好可按用户记忆写入。
- 模型自行推断的身份、人格事实、关系或心理状态不能静默升级为已确认记忆。
- 摘要必须引用其来源 scope 和可追溯消息/操作；摘要不得携带来源 scope 之外的事实。
- 每次写入、覆盖、删除或身份链接都产生一条审计 OperationRecord。审计记录不得复制明文密钥或整段原始聊天。

## ContextSelector 规则

ContextSelector 返回有界 ContextBundle，顺序为：

1. 系统与开发规则（不可被记忆覆盖）。
2. 当前可信消息 Envelope 和当前用户正文。
3. 当前 ConversationScope 内最近的上下文。
4. 与当前任务明确关联的 ActiveChatTaskScope。
5. 当前用户在相同 ChannelIdentityScope 内的偏好。
6. 同一 GroupScope 内用户明确授权的群记忆。

每个召回项带 Scope、Source、时间、确认状态和置信度。超出 token/字节预算时按优先级截断，并记录截断摘要；不得混入另一个群、另一个用户或未链接渠道的记忆。

摘要和历史上下文是低优先级不可信资料，不能授予权限、修改工具 allowlist 或覆盖当前用户明确指令。当前消息与历史要求冲突时，Agent 必须遵循系统/开发策略和当前授权流程。

## 持久化、保留和删除

- Core/Memory 是记忆的唯一持久化 owner；Provider 不能读写记忆数据库。
- 默认只持久化选定记忆与必要操作元数据，不无限期保存完整原始消息正文。
- 如果某渠道为了 resume 必须保留原始事件引用，保留策略、TTL、静态加密、导出和删除能力必须单独设计。
- Forget 请求必须明确删除范围：单条记忆、用户 scope、conversation scope、group scope 或 IdentityLink。
- 删除至少 tombstone 记忆索引和派生摘要；外部平台保存的内容不宣称可删除。
- 数据库迁移、加密密钥替换、备份恢复和 TTL 清理都作为独立任务验收。

## 验收矩阵

- 不同发送者的 UserScope 不互相读取。
- 不同群和私聊互相隔离。
- 同名 QQ/微信身份不自动合并。
- 用户撤销 IdentityLink 后不能再召回链接前的跨渠道项。
- 普通消息不能自动创建长期偏好。
- Forget 操作删除匹配 scope 并留脱敏审计记录。
- ContextBundle 达到预算时遵守优先级，保留当前消息与系统规则。
