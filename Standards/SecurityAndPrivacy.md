# 安全、隐私与平台边界

## 身份与授权

- Channel Adapter 提供来自平台的可信 Account、Conversation、Sender、Message、时间和事件 ID。
- 用户正文、引用文本、图片 OCR、语音转写、昵称、记忆、模型输出和工具结果均为不可信输入。
- Core 是唯一 side-effect policy owner。模型给出的身份、账号、群号、权限和角色文本不能获得授权。
- 每个外部写操作先建立 OperationRecord 和幂等键，再向 Provider 或 Channel 发送。
- 结果不确定时标记 UnknownOutcome，不盲目重试，等待人工或平台回执确认。

## Agent 工具

- 默认工具目录为空。
- 新增工具只能是具体、有限、可审计的聊天业务动作。
- Shell、任意代码执行、宿主状态查询、任意文件系统、任意网络、私有会话扫描和通用 MCP 均不默认开放给聊天模型。
- ToolCall 必须映射到活动 Core-owned AgentTurn、ConversationScope、ChannelCapabilities 和本轮发送者策略。
- 表情、贴纸、反应、拍一拍、昵称查询或主动发送均需平台能力证明和明确限频。无能力或无授权时返回 UnsupportedCapability。
- Agent 的自然聊天风格不能要求它假称为真人、冒充某个真实个人、编造已读状态或谎称动作成功。

## 产品记忆

- 产品聊天记忆与开发任务轨迹分开存储、分开检索、分开保留。
- 记忆按 Channel/Account/Identity/ConversationScope 隔离；跨 QQ/微信身份关联需明确授权和可撤销 IdentityLink。
- 记忆检索返回来源、作用域、创建时间、过期时间、确认状态。记忆永远不能授予操作权限。
- 用户明确要求记住的偏好、事实、任务约束和进度可以存储；自动总结内容须标记为推断和低优先级。
- 原始聊天全文不默认无限期保存。Retention、导出、忘记和过期规则须明确可验证。
- 删除身份链接时，Core 必须处理由该链接创建的缓存、摘要和派生索引。

## 凭据与日志

- Provider keys 和 Channel credentials 存入 Windows 用户保护凭据存储，引用 ID 与秘密分离。
- 密钥不能出现在配置样例、命令行、普通环境文件、Prompt、诊断日志或任务报告中。
- 诊断记录优先保存操作 ID、错误码、时间、profile 名和脱敏状态，不保存完整聊天正文。
- 设置 key、替换 key、撤销 key、导出记忆和清除记忆都是审计操作。

## 平台集成边界

- 每个 QQ/微信 Adapter 必须在 Research/Platforms 中标记连接形态、账号主体、接收范围、发送窗口、审核/速率限制和能力证据。
- Official Bot、公众号、企业微信/客服、小程序和个人客户端是不同的账号及 API 模型，不能混成一个 adapter。
- 不得因为另一平台支持 stickers/poke/reactions 就推断当前平台支持。
- 第三方 Hook、UI 自动化、私有协议或依赖 SnowLuma/OneBot 的替代路径不得静默启用；需要独立评审和显式决策。
- 若平台官方接口有消息时间窗口、权限审核、群成员限制或频次上限，Adapter 必须将它们转换成能力和错误，不得绕过。
- 终端或named pipe 的用户身份/ACL检查不替代频道用户身份授权。

## 本机通信和终端

- Core 与 TUI 使用受当前 Windows 用户 ACL 限制的 named pipe。
- 不开 localhost/loopback TCP 监听。模型 Provider 的远端 HTTPS/WSS 是独立出站访问。
- 若 HTTP 连接层没有固定 DNS 解析地址，启动时 DNS 检查不能宣传为完整 SSRF 防护。
- TUI 退出、异常和取消路径必须恢复 Console/VT 模式、光标和输入状态。