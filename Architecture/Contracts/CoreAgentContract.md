# Core 与 Agent 合同 v1

状态：草案。该合同用于划分 Core policy 与 C++ Agent Runtime，不依赖 Codex 私有结构。

## Core 的所有权

Core 唯一拥有：

- Account/Conversation/Sender 的可信映射。
- CredentialStore 和 profile 加载。
- Tool/Action Allowlist、限频、一次性授权和审批。
- ConversationScope、MemoryScope 和数据库事务。
- OperationRecord、幂等键、未知结果状态和生命周期。
- Channel/Provider Adapter 的注册与能力快照。

Core 不把策略责任委托给 Agent 的 System Prompt。

## Agent Runtime 的职责

Agent Runtime 执行单个 ChatTurn：

1. 校验传入 CanonicalMessage 及其可信 Envelope。
2. 请求 Core 解析 ConversationScope、能力和允许的 memory records。
3. 通过 ContextSelector 生成受 token/output limit 控制的 ModelRequest。
4. 订阅 Provider stream，将 CanonicalModelEvent 映射为 AgentEvent。
5. 如出现工具调用，验证 schema、能力、作用域和策略，再向 Core 申请执行。
6. 根据 ChannelCapabilities 形成 ChannelAction；由 Core/Adapter 执行发送并取得 ChannelReceipt。
7. 保存 turn 终态和按 policy 允许的 memory update。

Provider 返回的 function call 只是模型请求，不是授权凭证。Core 必须重新检查工具名、参数、活动 TurnId、可信 Envelope 和当前策略。

## AgentTurn 状态机

每个 turn 至少经历：

- Accepted
- ContextSelected
- Generating
- AwaitingAuthorizedAction（可选）
- Sending
- Completed / Failed / Cancelled / UnknownOutcome / Unsupported

状态转换记录 TurnId、OperationId、ProviderProfileId 和 ChannelReceiptId。模型流中断、外部消息发送超时、Core 重启都必须有恢复策略。

## Core RPC 与本机接口

- 本机服务通过版本化 JSON-RPC over Windows named pipe 暴露。
- TUI 只调用 Core API；Provider/Channel credential 不下发到 renderer。
- 本地 RPC 增加方法须更新 CoreAgentContract、调用方和版本策略。
- 不在 TUI、Agent、Channel Adapter 内重复实现 DPAPI、用户 scope 或权限判断。
- 任何远端 Provider 的 HTTP/WSS 请求都不是本机 IPC，不得改成隐藏的 loopback 服务。