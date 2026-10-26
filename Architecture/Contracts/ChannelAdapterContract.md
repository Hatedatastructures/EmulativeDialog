# 渠道适配器合同 V1

状态：草案。QQ 和微信的具体账号/授权类型仍须先在 Research/Platforms 中核实。

## 适配器职责

每个 Adapter 只负责一种已确认的渠道集成模式。它完成平台 SDK/HTTP/WebSocket 接入、平台事件解析、源身份验证、消息正规化和已授权的出站动作。Agent 不直接调用 QQ/微信 SDK 或平台 socket。

概念接口：

- GetCapabilities：读取当前账号、认证、平台权限和审核状态对应的能力。
- Start / Stop：建立和释放连接；重复启停具有明确结果。
- SubscribeEvents：递交 InboundEvent，不传递未核验的用户身份声明。
- SendAction：执行 Core 已授权的 ChannelAction 并返回 ChannelReceipt。
- HealthSnapshot：提供脱敏的连接/认证/限流状态。

## 统一入站事件

InboundEvent 至少具有：

- ChannelId、IntegrationMode、AccountId、ConversationId、ConversationKind。
- SenderId、MessageId、PlatformEventId、Timestamp。
- MessageParts、ReplyToId、MentionedIds、PlatformMetadata。
- StableDeduplicationId 和可信度/来源标记。

所有 ID 按不透明字符串处理。账号、群、发送者、角色不能从正文或模型输出推断。

## 统一出站动作

ChannelAction 可表达：

- SendText、SendRichText、SendMedia。
- SendSticker、AddReaction、Poke 或平台特定社交动作。
- EditMessage、DeleteMessage 等后续候选能力。

每个动作有 ActionId、IdempotencyKey、TargetScope、正文/媒体引用和策略上下文。Adapter 必须用 CapabilitySet 判断动作是否支持；不支持时返回 UnsupportedCapability，权限或频率不满足时返回 Forbidden 或 RateLimited。

ChannelReceipt 包含 ActionId、平台回执 ID、时间、最终状态和脱敏的错误类别。无回执或网络超时的结果为 UnknownOutcome，禁止 Agent 声称成功。

## 账号类型与能力矩阵

- QQ Bot API、QQ 个人客户端、本地测试 Mock 是不同 IntegrationMode。
- 微信公众号、企业微信/微信客服、小程序、个人微信是不同 IntegrationMode。
- 启动时的能力集合按账号主体和平台审核状态生成；不把一个模式的权限复用到另一个模式。
- DisplayName、昵称、贴纸、表情、拍一拍、主动消息、群全量消息和媒体必须逐项取证。
- 有时间窗口、条数上限、intent 或审核要求的平台动作，由 Adapter/Core 明确执行限制。
- 若平台只开放“收到消息后回复”，不能把它包装成任意主动消息功能。

## 隐私和内容边界

- 用户显示名称、头像、群资料、引用消息和媒体都属于个人数据。
- Channel Adapter 不把完整平台原始对象传入模型；先映射成允许的 CanonicalMessage。
- Channel token 只存在受保护凭据边界或必要的进程内存。
- 调试记录只保留事件 ID、能力结果和脱敏错误，不记录完整私聊正文。
- 用户明确要求删除或解绑时，返回哪些消息、记忆和摘要会删除，哪些仍由平台持有。
