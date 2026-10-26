# 系统架构

## 运行时目标

发布的软件是一个采用 C++20 或更新标准构建的 Windows x64 程序。产品不会启动或嵌入 Rust 版 Codex、Node.js、Go、Python、Web UI 或其他 Agent 运行时。构建元数据和文档可以使用工具要求的格式。Boost 及其他经过审查的 C++ 库按 Standards/CppEngineeringStandard.md 管理。

本项目的主工程任务是用 C++ 重实现 ADR-0002 纳入范围的 Codex Agent 核心，并为聊天场景重新设计。Provider 适配器、记忆组件或 TUI 单独完成，都不等于 Codex Agent 核心重写完成。

应用提供 CLI 入口，用于启动 Agent 服务和自研 TUI。服务负责会话状态、记忆、策略、Provider 选择、渠道适配器、持久化和本机 IPC。TUI 是服务的客户端，不单独实现一套授权逻辑或会话状态。

## 模块边界

    QQ 渠道适配器 ----------┐
    微信渠道适配器 ---------+--> Core 入站 --> 会话路由 --> Agent 运行时
                          |                               |                    |
                          |                               v                    v
                          |                         上下文选择器       Provider 注册表
                          |                               |          /       |       \
                          |                               v       Responses  Chat    Anthropic
                          |                         聊天记忆
                          |                                                    |
                          └<---- 出站动作路由 <--- Agent 回合 <------┘

    C++ TUI --> Windows 命名管道 --> Core RPC

### 命令行与应用宿主

应用宿主解析 CLI 命令、加载配置、启动和停止服务，并进入 TUI 模式。交互运行时由宿主负责协调关闭流程和 Windows 控制台会话。

### Core 核心

Core 独占授权、持久化操作、会话路由、Provider 配置档选择、记忆作用域、渠道身份规范化和配置变更。所有副作用都必须经过 Core 策略检查。模块通过有类型的结果返回数据，不直接写入其他模块的存储。

### Agent 运行时

Agent 运行时以一条可信且已规范化的入站消息为输入，执行一个回合。它组合指令和限定作用域的上下文，选择指定 Provider，处理流式事件，只调用已登记的业务工具，并生成规范化的出站回复或动作。它不负责原始平台套接字、凭据、终端绘制或数据库句柄。

### Provider 适配器

每个 Provider 适配器都实现同一份 C++ Provider 合同，同时将各自的网络协议模型封装在模块内部：

- OpenAI Responses
- OpenAI-compatible Chat Completions
- Anthropic Messages

适配器负责请求序列化、Provider 认证头、SSE 解析、错误规范化、用量映射、输出限制、重试分类和取消处理。它们向 Agent 运行时返回统一格式的事件流。Provider 配置档不能改变 Core 授权规则。

### 渠道适配器

QQ 和微信使用彼此独立的渠道适配器。每个适配器都声明接收、发送、媒体、贴纸、反应、已读或显示信息及社交动作等能力。Core 在执行动作前会检查能力集合和当前账号策略。一个渠道的功能或身份语义不得泄漏到另一个渠道的实现中。

在产品明确微信账号面之前，不得启用微信适配器。账号面可以是公众号、企业微信/微信客服、小程序或其他明确获准的接口。QQ 官方 Bot API 和 QQ 个人客户端接入属于不同账号面，必须分别审查能力和授权。

### 聊天记忆与上下文选择

产品记忆与开发工作流账本分开管理。记忆按可信的渠道、账号、身份和会话信息划分作用域。上下文选择器只为当前回合读取相关且有数量上限的记录；不得把整个数据库或所有渠道的历史记录塞进 Prompt。

Memory record categories are:

- 用户偏好：用户明确表达且希望长期生效的交互偏好。
- 已确认事实：用户明确要求 Agent 记住的信息。
- 会话摘要：限定在单一会话作用域内、用于保持连续性的摘要。
- 活跃聊天任务：用户尚未完成的聊天请求、约束和进度；归属原始身份及明确关联的会话。

检索结果必须包含来源、作用域、时间戳和置信度。记忆内容属于不可信输入，不能授予工具权限，也不能覆盖更新的消息。跨渠道身份关联必须取得明确同意。

### 持久化

持久化通过 C++ 接口实现，每类数据只有一个所有者。初版应分开存储配置、凭据引用、会话元数据、操作/发件箱和聊天记忆记录。Provider 凭据使用 Windows 凭据保护边界，不保存在普通 JSON 设置中。数据结构迁移必须显式排序；在可行时支持回退，并在破坏性变更前保留备份。

### TUI 终端界面

TUI 与服务共用版本化本机 RPC 合同。TUI 显示会话视图和服务状态，不直接调用 Provider 或渠道 API。TUI 引擎由项目使用 C++ 自行实现，并提供简洁的组件 API；具体行为见 Architecture/TuiApi.md。

## 统一 C++ 合同类型

以下概念类型由 Architecture/Contracts 定义，内部名称使用 PascalCase：

- ChannelEnvelope: verified source, account, conversation, sender, timestamp,
  message ID, and platform capability data.
- CanonicalMessage: normalized text and supported media/reply parts; raw
  channel-specific structures remain inside the adapter.
- ConversationScope: channel/account/identity/conversation scope for memory
  and history.
- AgentTurn: one request, selected context, provider profile, stream state,
  cancellation state, operations, and terminal outcome.
- ModelRequest and ModelEvent: provider-neutral request and event sequence.
- ToolCommand and ToolResult: registered business action, canonical args,
  policy decision, idempotency key, and result.
- ChannelAction and ChannelReceipt: requested outbound effect and
  adapter-confirmed outcome.
- OperationRecord: durable state for any external side effect, including
  succeeded, failed, unsupported, and unknown outcomes.
- CapabilitySet: explicit supported actions and message formats for a
  provider, channel, account, and runtime profile.

外部 JSON 名称和协议字段保留其规定的网络拼写形式。名称转换只在合同边界的序列化器中进行。

## 数据与事件流

1. 渠道适配器接收平台事件，并使用所选接入方式的正式认证机制验证来源身份。
2. Core 将事件规范化为 ChannelEnvelope 和 CanonicalMessage，拒绝稳定事件 ID 的重复项，并记录入站操作。
3. Core 应用账号、发送者、群组和动作策略，然后确定 ConversationScope 并选择上下文与记忆。
4. Agent 运行时构造统一 ModelRequest，并通过所选 Provider 适配器发送请求。
5. 每个 Provider 事件都带有稳定的事件/回合关联信息，并转换为统一 ModelEvent。错误和 token 上限终止必须明确表示。
6. 每个工具或动作请求都要经过当前渠道能力和 Core 策略检查，并在执行前记录操作。
7. 每个出站 ChannelAction 只发送一次。投递结果不确定时将操作标记为未知，不盲目重试。
8. 按保留策略持久化最终回复和获准的记忆更新；TUI 只接收安全的状态视图。

## API 形态与等价性决策

TUI 必须先选定具体 TypeScript 参照库和版本，再对其批准范围内的公开 API、参数含义、事件语义、生命周期和可观察渲染结果建立一对一 C++ 映射；目标范围的覆盖率为 100%，未批准差异为零。符合 C++ 的调用语法可以不同，但不能因此减少参照库能力。TypeScript JSX 源语法不能直接成为原生 C++ 语法；如果要求字面相同的 JSX 语法，需要另行决定是否改变全 C++ 运行时边界。完整门槛见 Architecture/TuiApi.md。
