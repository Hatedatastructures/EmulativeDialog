# EmulativeDialog

EmulativeDialog 的首要目标是用 C++ 重实现 Codex Agent 核心，并将它做成全 C++ 聊天专用 Agent。当前阶段先接入经授权的 QQ；微信延期。系统把消息转换成统一模型，选择相关上下文与记忆，调用三种模型 Provider，再通过来源渠道返回文本、媒体或受支持的社交动作。

## 项目目标解释

本项目要把聊天 Agent 需要的 Codex Agent 核心行为用 C++ 重新实现，包括回合编排、会话/上下文、工具调用生命周期、记忆、三种 Provider、流式处理、取消、错误与恢复。它不运行或包装 Rust Codex。Codex CLI/IDE 界面、代码编辑、Shell/主机命令拦截、主机状态查询和任意文件工具明确不属于当前产品范围；如需加入，必须走 ChangeRequest。

开发阶段可以由 Codex 或其他开发 Agent 生成、审阅 C++ 代码；该开发协调器不属于 EmulativeDialog 的运行时依赖。

## 技术方向

- 产品运行时：C++20 起步，Windows x64 原生构建。
- TUI：自研 C++ 屏幕缓冲、布局、交互组件和终端适配层；不依赖 FTXUI、Ink、OpenTUI 或其他 TUI 框架。
- Provider：C++ 实现 OpenAI Responses、OpenAI-compatible Chat Completions、Anthropic Messages 三个协议适配器。
- Channel：当前先实现经授权的 QQ C++ Adapter；微信延期，未来若重启则作为独立 Adapter 评估。
- 网络：优先评估 Boost.Asio、Boost.Beast、Boost.JSON；外部依赖必须在 Standards/DependencyPolicy.md 注册并锁定版本。
- 平台安全：Win32 DPAPI、named pipe、Console/VT、线程/IOCP 等通过 C++ RAII 封装。
- 构建元文件仍使用 CMake、YAML/JSON 等工具要求的格式；“全 C++”指产品运行逻辑和发行运行时。
- 为保证长周期重写不偏移，项目自有开发期 AgentWorkflowController 也使用 C++；Codex 开发宿主只提供模型/子代理执行能力，不作为 EmulativeDialog 运行时依赖。

## 项目接口原则

- Core 是唯一授权和状态边界。
- Provider 使用统一的 CanonicalModelRequest / ModelEvent 合同。
- Channel 使用统一的 ChannelEnvelope / ChannelAction 合同。
- TUI 仅调用 Core RPC，不直接连接模型或 QQ/微信。
- 私有协议、Hook 和 UI 自动化不是默认兼容路径；每种渠道连接方式须先通过授权与能力审查。

## 文档入口

- Goals/ProductGoals.md：聊天产品的范围、目标和非目标。
- Architecture/SystemArchitecture.md：模块职责、状态流和规范边界。
- Architecture/Decisions/Adr0002CodexAgentCppRewrite.md：C++ 重实现 Codex 聊天 Agent 核心的主目标、纳入/排除范围和完成标准。
- Architecture/TuiApi.md：自研 C++ TUI 的公开行为合同。
- Standards/CppEngineeringStandard.md：大驼峰、C++ 规则及工具检查。
- Standards/DependencyPolicy.md：Boost 和其他依赖准入规则。
- Standards/SecurityAndPrivacy.md：凭据、聊天身份、记忆及平台动作边界。
- Workflow/AgentExecutionStandard.md：AI Agent 的任务、纠错、上下文和审计规则。
- Workflow/TaskLifecycle.md：分支、PR、评审、合并及任务状态机。
- Workflow/TaskIndex.yaml：按阶段分片的任务与事件路由索引；不要整份计划预载。
- Workflow/AgentPool.yaml：最多八个稳定逻辑工作槽的注册表；槽位只有分配真实 Ready 任务后才启动。
- Workflow/EnforcementMatrix.yaml：每条硬规则对应的自动门、独立签字门和当前落地状态。
- Architecture/ModuleMap.yaml：模块 owner/写集分片索引；按当前任务只载入一个模块域。
- Workflow/Plans/P0.yaml：当前范围与证据门的候选任务；未获分配前均为 Proposed。
- Research/Platforms/PlatformCapabilityMatrix.md：QQ/微信平台支持证据和待确认能力。
