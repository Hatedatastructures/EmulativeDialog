# ADR-0001：全 C++ 产品运行时与自研 TUI

状态：方向已由产品 owner 明确；工具链、Boost/TLS 和平台接入模式仍待 P0 任务锁定。

## 背景

EmulativeDialog 要作为面向 QQ/微信的聊天专用 Agent。用户要求产品运行时、Provider、Channel、Memory、TUI 和底层自有模块全部用 C++，并要求项目自有目录、文件和 C++ 标识符遵循大驼峰。Agent 必须支持 OpenAI Responses、OpenAI-compatible Chat Completions 和 Anthropic Messages。

## 决定

- 发布运行时只包含 C++ 产品模块；不带入 Rust Codex app-server、Go TUI、Node/Electron UI 或 Python Agent。
- 将 Codex 在本产品中的 Agent 运行能力改为 C++ 聊天专用实现。
- 不以复制 Codex 整个 Rust 源仓库的文件结构或逐行翻译为成功标准；成功标准是聊天产品合同和经过确认的能力。
- Codex Agent 核心改写的具体纳入/排除范围与完成判定由 Architecture/Decisions/Adr0002CodexAgentCppRewrite.md 固定。
- Responses、Chat Completions、Anthropic 各自是独立 Provider Adapter，共享 CanonicalModelRequest / CanonicalModelEvent。
- QQ 与微信各自实现 C++ Channel Adapter；平台能力按账号类型、授权和当前官方 API 记录。
- TUI 的 screen buffer、layout、input、renderer 和 widgets 均自研 C++，不依赖第三方 TUI 库。
- Boost.Asio/Beast/JSON 可在通过依赖审查后作为 C++ 网络基础设施；Core policy 和协议语义由项目代码实现。
- 开发期可继续用 AI 编程 Agent 写和审查 C++；开发工具不是发布运行时的一部分。

## API 等价范围

TUI 开工前必须锁定具体 TypeScript 参照库和版本，并对其批准范围内的公开 API 建立 100% 的 C++ 映射、参数语义和行为对照；所有未批准差异为零。C++ API 的调用语法可以符合 C++ 习惯，但不能减少组件能力或改变可观察行为。TypeScript JSX 的字面语法不能直接成为原生 C++ 语法；若用户要求相同 JSX 源语法，必须通过 ChangeRequest 重新决定是否改变全 C++ 运行时边界。详细门槛见 Architecture/TuiApi.md。

## 后果

- Provider、Channel、Memory、TUI 的公共 C++ contract 必须早于并行实现冻结。
- 大型 Codex 功能清单必须先与聊天产品 scope 对照，明确每项复刻、重设计、移除或暂缓。
- 任何 QQ/微信模式和聊天动作必须先核实官方/授权能力；Unsupported 是合法结果。
- C++ TUI 需要自行承担终端兼容、Unicode display width、输入解析、resize、selection 和异常退出恢复。
- 依赖、行为和质量通过 Windows 构建、合同 fixtures、TaskReport 和 PR 证据维护。
