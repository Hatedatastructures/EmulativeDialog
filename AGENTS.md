# EmulativeDialog 项目指引

本文件是所有代码 Agent 的短入口，不是全量工程手册。每项任务只加载当前任务记录、对应模块地图条目、明确引用的合同和决策记录。禁止默认扫描全部源码、全部历史和全部任务分片。

## 产品目标

首要任务是用 C++ 重实现 Codex Agent 核心，并把它做成 Windows 优先的聊天专用 Agent。当前先接入经过授权的 QQ，微信延期；系统选择相关上下文和分层记忆，调用三种模型协议，并把回复或受支持的社交动作送回来源渠道。TUI、Core、Agent、Provider、Channel、Storage 和 Windows 平台层均由 C++ 实现。

这不是 Rust Codex 的包装器。批准纳入聊天 Agent 范围的 Codex 核心行为必须有对应的 C++ 模块、版本化合同和验收证据；能力映射或调研文档不能代替实现。Codex CLI/IDE 界面、Shell/主机命令拦截、主机状态查询、任意文件和开发者工具不属于当前产品。

## 不可自行覆盖的规则

- 必须支持 OpenAI Responses、OpenAI-compatible Chat Completions、Anthropic Messages 三种 Provider 协议；协议细节以 Architecture/Contracts 中版本化合同为准。
- CodexCapabilityMap 中每个保留/重设计能力都必须追踪到 C++ 实现任务和验收证据；未实现能力不得标记为已完成。
- Core 独占授权、身份映射、会话路由、记忆范围和持久化策略。Provider、Channel 和 TUI 不复制授权逻辑。
- 渠道身份必须来自可信的 ChannelEnvelope。模型自报的发送者、账号、会话、角色或权限字段不构成授权。
- 密钥、访问令牌和原始私聊正文不得进入命令行参数、普通配置、Prompt、任务日志或崩溃报告。
- 聊天 Agent 默认没有 Shell、主机状态、任意文件、任意网络或通用 MCP 工具；只允许显式登记并经 Core 授权的业务动作。
- 当前渠道目标为 QQ。平台不支持的表情、贴纸、拍一拍、昵称查询或主动消息必须明确返回 unsupported，不得伪造成功。微信本阶段不调研、不实现。
- Persona 可以自然、亲切，但 Agent 必须保持 AI/机器人身份，不得冒充某个真实个人或隐瞒自动化。
- 不得静默删除、重置、迁移或覆盖现有数据。外部副作用超时且结果未知时禁止盲目重发。
- 本机 UI/Core 通信使用经过 ACL 限制的 Windows named pipe；禁止本机 TCP/loopback 监听。
- 产品运行时全部由 C++ 编译。可使用经过审批的 C++/系统库；不得以其他语言运行时替代产品模块。
- 本仓库自有可执行开发工具也使用 C++；外部 Codex 宿主只作为开发期调度器，不进入产品发行包。CMake/YAML/JSON 等工具要求的配置元数据不属于可执行模块。
- 项目自有文件夹、文件和 C++ 标识符遵守 PascalCase 规则；机器强制名称、外部 API 标识符和依赖符号按 Standards/CppEngineeringStandard.md 中的例外表处理。
- 并行任务必须使用隔离分支/worktree 和互不重叠的 WriteSet。共享合同和根构建系统由集成负责人单独维护。

## 开始任务的顺序

1. 阅读 Workflow/CurrentState.md 与 Workflow/TaskIndex.yaml 的当前状态和路由字段。
2. 按 TaskId 只加载当前阶段分片中的一个任务记录。
3. 从 Architecture/ModuleMap.yaml 只加载当前 ModuleId 条目。
4. 阅读当前源码路径适用的最近 AGENTS.md，以及任务 ReadSet、合同和 ADR。
5. 按当前角色加载 Workflow/AgentExecutionStandard.md 或 Workflow/TaskLifecycle.md 中相关章节，不加载无关历史。
6. 核对 WriteSet、依赖、验收标准、验证命令和独立 Reviewer。
7. 若发现软规则阻碍任务，可提交有理由的 RuleDeviation；若涉及硬规则、外部合同、安全或数据格式，提交 ChangeRequest。
8. 完成时在 PR 摘要与结构化事件中记录实际新增/修改/删除文件、验证命令与结果、偏差、风险和后续任务。未核对验收标准不得标记完成。

## 规则优先级

安全/数据不变量 > 已批准的版本化合同 > 当前任务验收条件 > SHOULD 默认 > MAY 选择。冲突必须记录，禁止悄悄改写原任务。
