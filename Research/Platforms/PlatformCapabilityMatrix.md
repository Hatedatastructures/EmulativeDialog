# QQ 与微信平台能力矩阵

最后核对：2026-10-26。平台 API 和权限可能变化。此表是实施前的证据索引，不是对任何个人账号权限的承诺。

状态定义：

- Verified：有当前官方资料且适配器测试证明目标账号可以使用。
- Documented：有官方文档，但目标账号审核/运行尚未验证。
- Unknown：证据不足，不能在产品功能列表中承诺。
- Unsupported：当前项目明确不实现。
- RequiresDecision：产品必须先选择账号类型或授权范围。

| Channel/模式 | 收取 | 回复/发送 | 昵称/身份 | Sticker/Reaction/Poke | 状态 |
|---|---|---|---|---|---|
| QQ 官方 Bot | 按事件订阅、intent、账号审核和场景权限 | 按消息场景的回复窗口、消息额度和平台审核；主动消息政策要逐项核对 | 以官方 API 实际提供字段为准，不推断传统 QQ 号 | 每项单独核实；默认 Unknown | Documented / 需测试 |
| QQ 个人客户端 | 尚未选定支持路径 | 尚未确认授权 API | 未核验 | 未核验 | RequiresDecision |
| 微信公众号 | 取决于账号类型、关注/会话状态和接口权限 | 取决于被动回复、客服消息及平台时限 | 按接口返回字段处理 | 未核验 | RequiresDecision |
| 企业微信/微信客服 | 取决于企业主体、客服账号和权限 | 通过对应客服 API 与接待状态管理 | ExternalUserID/客服账号与个人微信身份不同 | 未核验 | RequiresDecision |
| 微信小程序 | 取决于具体小程序场景与用户授权 | 只能使用该场景允许的消息接口 | 不视为通用微信聊天联系人 | 未核验 | RequiresDecision |
| 个人微信客户端 | 尚未核验官方受支持访问路径 | 尚未核验 | 未核验 | 未核验 | Unsupported until decision |

## QQ 官方 Bot 当前资料提示

腾讯维护的 QQ Bot send 文档当前写有：通用主动推送能力自 2025-04-21 起不再提供，并列出单聊/群聊被动回复窗口和回复次数限制。该页面同时包含不同场景的限制说明，因此实现前必须按目标事件和账号类型重读最新页面，不得只据此总结成“所有主动消息均可用”或“所有主动消息均不可用”。

来源：
https://github.com/tencent-connect/bot-docs/blob/main/docs/develop/api-v2/server-inter/message/send-receive/send.md

## WeChat 接入决策门

“微信”必须选定账号面：公众号、企业微信/微信客服、小程序或个人客户端。每种模式各有身份、授权、消息入口、回复窗口、媒体能力和审计要求。未选定前不实现统一 WeChatAdapter，不从第三方桌面 Hook 项目推断官方能力。

## 功能准入表

每个可见的聊天动作必须有一条 capability 记录：

- CapabilityId、ChannelMode、AccountScope、Direction。
- API 文档地址、文档版本/最后核对时间、审核依赖。
- 输入 schema、用户授权、目标和限频。
- 失败码、幂等规则、UnknownOutcome 处理。
- 适配器 contract fixture 和目标账号验证证据。
- 需不需要用户可见确认，是否允许自动触发。

没有上述证据的能力保持 Unknown，不显示给 Agent。