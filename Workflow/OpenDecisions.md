# 待确认决策

这些问题只在依赖它们的实现任务之前需要确认。Agent 可继续处理不依赖这些决定的文档、合同草案和 Fake fixtures，不可自行假定后声称平台功能已支持。

| DecisionId | 问题 | 当前工作假设 | 受影响模块 |
|---|---|---|---|
| OD-0001 | Codex Agent 核心重写使用哪个官方仓库、提交和许可证基线？ | 重写范围已由 ADR-0002 固定为聊天专用 Agent 核心的 C++ 重实现；CLI/IDE UI、Shell/主机命令拦截、主机状态查询和任意文件工具排除 | CodexCapabilityMap、AgentRuntime、各 C++ 模块 |
| OD-0002 | QQ 接入模式是官方 QQ Bot、个人 QQ 客户端或其他经过授权的方式？ | 每种模式单独 adapter，不假定个人 QQ 可用 | QQ Adapter、身份模型、消息能力 |
| OD-0003 | 未来恢复微信工作时选择哪种账号类型？ | 当前阶段 Deferred，不调研、不实现、不纳入验收；以后另开任务选择公众号、WeCom/客服、小程序或其他授权接口 | 后续 WeChat Adapter；不阻塞当前 Codex/QQ 工作 |
| OD-0004 | 哪些消息和社交动作必须支持？ | 文本先行；昵称、图片、语音、表情包、反应、拍一拍和主动消息逐项验证 | CanonicalMessage、ChannelAction、TUI |
| OD-0005 | TUI 对标哪一个 TypeScript 库及精确版本？“API 一样”是否要求字面 JSX 源语法？ | 必须先锁定单一参照版本；批准范围内 API 映射覆盖率 100%、未批准行为差异为零。C++ 调用语法可以不同，但不得减少能力；字面 JSX 语法与原生全 C++ 存在运行时边界冲突 | TUI API、布局/输入/渲染、差异测试 |
| OD-0006 | Windows terminal 支持范围是 Windows Terminal、经典 Console、ConPTY，还是仅指定 shell？ | Windows Terminal 为首个验收目标，其他逐项扩展 | TerminalSession、InputRouter |
| OD-0007 | Boost 和 TLS 固定版本是什么？选 OpenSSL 还是 Windows Schannel？ | **已决定（2026-10-28，ED-0120/R2）**：Boost 1.92.0 固定 commit `afdfa32505af73e3d208144b3f623f0096cb62b6`（ED-0131）；HTTP 使用 WinHTTP + 系统 Schannel 信任根，证书失败 fail-closed，默认拒绝重定向，系统代理或显式直连，取消/超时/unknown-outcome 语义见 ADR-0003 与 HttpTransportContract V1。 | HTTP、WebSocket、安装包 |
| OD-0008 | QQ/微信是否允许保存原始聊天正文，保存期限与删除语义是什么？ | 默认保存必要的操作/作用域元数据和精选 memory，不无限期存全文 | Storage、Memory、隐私设置 |
| OD-0009 | 产品如何公开 AI 身份和用户选择的 persona？ | 自然聊天风格可配置；不冒充真实个人，不隐藏自动化属性 | SystemPolicy、TUI |
| OD-0010 | 提醒/主动发送功能是否属于 v1？平台主动消息限制是什么？ | 只有平台能力和用户授权都明确后才打开 | Scheduler、ChannelAdapter、RateLimit |

决策格式：现状、选项、证据、影响面、回退方式、owner、日期和状态。决定完成后新增 ADR，不删除旧决定历史。
