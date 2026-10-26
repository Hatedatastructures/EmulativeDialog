# 模型 Provider 合同 V1

状态：草案。三协议的公共语义必须先由合同负责人核定，再允许 Provider Agent 并行实现。

## 公共职责

Provider Adapter 是纯 C++ 边界组件。它接收 CanonicalModelRequest，通过配置的 HTTPS 远端端点请求模型，并把响应转换为 CanonicalModelEvent。它不直接访问 Channel、数据库、TUI、DPAPI 或 Core 授权策略。

建议内部接口概念：

- GetCapabilities：返回流式输出、视觉、工具调用、取消、token 用量和输出限制等能力。
- ValidateProfile：校验 endpoint、模型、认证方式和协议参数。
- StreamTurn：接收 ModelRequest、CancellationToken 和 EventSink；返回有序事件与终态结果。
- Cancel：请求停止当前 Provider 操作。
- HealthCheck：只做受控的连接可用性检查，不发送用户私密内容。

具体类名与方法签名在 Contract Freeze 任务中落定。

## 公共请求与事件

CanonicalModelRequest 至少包括：

- RequestId、ConversationScope、TurnId、CreatedAt。
- SystemInstructions、Messages、ContentParts、ToolDefinitions。
- ModelName、OutputLimit、SamplingOptions、ProviderOptions。
- CancellationToken 和当前 profile 的能力集合。

CanonicalModelEvent 具有单调递增的 SequenceNumber、RequestId、TurnId 和 EventType。最小事件类型：

- TurnStarted、TextDelta、ReasoningDelta（仅经批准）、ToolCallStarted、
  ToolArgumentDelta、ToolCallReady、UsageUpdated。
- TurnCompleted、TurnFailed、TurnCancelled、OutputLimitReached。
- ProviderError，其中包含稳定 ErrorCode、可重试标志和脱敏的说明。

事件顺序必须稳定。每个文本片段及工具调用使用稳定的 ItemId。终止事件只出现一次。取消、错误和 token limit 不得伪装成正常完成。

## 三协议映射

| 协议 | 请求/流 | 认证 | 映射要求 |
|---|---|---|---|
| OpenAI Responses | Responses request 与 Responses SSE event | 仅使用当前 profile 的受保护 key | instructions/input/tools/output_limit、text/refusal/function_call、usage 和 error |
| OpenAI-compatible Chat Completions | messages/tools 与 Chat completion SSE chunks | 当前 profile 的 Bearer key | role/content、stream deltas、tool_calls、finish_reason、usage；供应商 token 字段按明确的兼容配置映射 |
| Anthropic Messages | messages/tools 与 Anthropic SSE blocks | x-api-key 和版本头，密钥由 Provider 层注入 | system、user/assistant、text、tool_use/tool_result、stop_reason、usage |

协议字段不能直接渗入 Core/Agent 数据模型。Response、Chat 和 Anthropic 每个映射函数对不支持字段返回 UnsupportedCapability 或 InvalidRequest，不静默丢弃。

## 安全与可靠性

- TLS 证书验证启用；endpoint 只能是经 Core endpoint policy 审核的远端地址。
- 任何含凭据的请求默认拒绝跨源 HTTP redirect。
- API key 不能进入 URL、日志、命令行、消息正文、错误详情或事件流。
- SSE 解析器处理 UTF-8 增量、分片边界、重复/乱序事件、空帧、超时、断连和服务商错误。
- 重试必须按协议和副作用状态分类；已发送 turn 的结果不确定时标记 UnknownOutcome，不自动重复生成副作用。
- Provider 的 max output、timeout、retry 和 capability 参数显式记录在 profile；不依赖模糊的默认行为。

## 验收要求

- 每个协议有不访问网络的请求映射 fixture 和 SSE event fixture。
- 三个 Adapter 对相同 CanonicalModelRequest 产生同一类 CanonicalModelEvent。
- 非法身份、未声明工具、格式错误、token limit、超时、取消和断线都有确定的 ErrorCode。
- Provider live smoke 是独立的发布验证任务，不可由 fixture 或编译成功替代。
