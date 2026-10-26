# C++ 工程规范

本规范应用于 EmulativeDialog 自有 C++ 源码、测试、内部接口和产品运行时。Boost、Windows API、C ABI、外部协议及自动生成文件使用各自规定的名称和边界。

## 语言与平台基线

- C++20 是初始最低标准；提高标准版本须有 ADR、Windows 工具链矩阵和构建证据。
- 初始交付目标是 Windows x64 原生环境。WSL 不作为实现或验收环境。
- 所有资源使用 RAII 管理：Windows HANDLE、console mode、文件、锁、异步操作和临时资源。
- 禁止未说明所有权的裸 owning pointer、无界 detached thread、隐式生命周期延长和跨模块可变全局状态。
- 异步函数必须有取消、超时、资源所有者、结束状态和失败语义。
- Provider、Channel、TUI 和 Storage 通过窄接口通信，公共合同不泄漏 Boost 实现类型。

## 大驼峰定义

“大驼峰”在本项目中指 PascalCase：每个英文单词首字母大写、词间不使用分隔符。项目自有标识符全部按此执行：

| 类别 | 规则 | 示例 |
|---|---|---|
| 文件夹和文件基名 | PascalCase | Source/Providers/OpenAIResponses |
| namespace 片段 | PascalCase | EmulativeDialog::Agent |
| class/struct/enum/concept/alias | PascalCase | ProviderProfile |
| 函数/方法 | PascalCase | BuildCanonicalRequest |
| enum 值 | PascalCase | StreamCompleted |
| 成员/局部变量/参数/模板参数 | PascalCase | ConversationId |
| 常量/constexpr 变量 | PascalCase | MaximumFrameBytes |
| CMake target | PascalCase | EmulativeDialogCore |

私有成员默认也使用 PascalCase，不加 m_、s_ 或尾随下划线。为避免同作用域冲突，字段使用语义名，访问方法使用动词，例如字段 ConversationState、方法 GetConversationState 和 SetConversationState。若该约定造成真实冲突，提交 RuleDeviation，不得私自混用多套风格。

## 必需名称例外

以下为固定工具/API名称，不属于违规：

- AGENTS.md 是 Codex 自动发现项目指引时要求的文件名。
- CMakeLists.txt、CMakePresets.json、.gitignore、.clang-format、.clang-tidy 使用工具要求的固定名称。
- C/C++ 宏使用 UPPER_SNAKE_CASE。
- Windows SDK、Boost、标准库、第三方 API、C ABI 和自动生成文件保留其上游名字。
- QQ/微信、Responses、Chat Completions、Anthropic 等外部协议字段保持精确 wire spelling。
- 存储格式和跨版本 JSON 字段不能只为代码命名风格而重命名；在序列化边界转换。

项目路径名检查器只检查自有 Source、Tests、Standards、Architecture 和 Workflow 内容；检查器使用明确例外表，不使用宽泛忽略规则。

## 代码结构与边界

- 一个文件聚焦一个主要类型或一组强相关实现；文件名使用 PascalCase。
- Header 声明能力，CPP 实现细节；公共 Header 不依赖偶然传递 Include。
- 使用值类型表达小消息/合同；长生命周期资源使用唯一、明确的 RAII 所有权。
- 错误通过统一 ErrorCode/Result 体系返回，不丢弃 Provider、Channel、Storage 或 OS 错误。
- 外部输入在协议边界校验长度、编码、枚举、ID 和允许字段。
- 禁止记录密钥、完整访问令牌、未脱敏聊天正文和未经授权的身份信息。
- API Provider 只负责网络映射；Core 决定授权、重试安全和记忆范围。
- TUI 只调用 Core contract；不得直连数据库、Provider、QQ/微信或 DPAPI。

## 公共 API 简洁、安全与可演进性

跨模块接口和模块公开 API 都必须先有具体消费者场景，再冻结合同；不得为了“以后可能用到”暴露尚无调用方的抽象层、扩展点或配置项。一个业务动作默认只提供一种规范调用方式；确有不同语义时使用不同命名和类型，不用多个重载/布尔开关隐藏行为。

每项公开操作必须说明：

- 用途、输入约束、输出/回执、失败状态和重试语义。
- 资源所有权、线程/执行器约束、取消和超时行为。
- 身份来源、授权 owner、幂等键和可能的外部副作用。
- 稳定性级别、合同版本、兼容规则和迁移责任。

设计审查必须能从一个最小调用示例看出正常路径、拒绝路径、失败路径和资源释放。禁止仅返回 bool 或空字符串来表示多种失败；使用窄域类型与统一 Result/ErrorCode 表达结果。不可用未校验的字符串、整数或可随意组合的标志位代表账号身份、能力、权限、状态机阶段或敏感操作。

公共 API 不泄漏 Boost、Win32、数据库驱动或 Provider wire 类型；外部协议字段只存在于序列化边界。禁止 void*、std::any、无约束字典或全局回调作为跨模块逃生口。输入长度、编码、枚举、标识符、资源上限和状态转换必须在信任边界校验。

任何公开 API 变化都要更新对应合同、调用方映射、契约夹具和变更说明。破坏性变更先升合同版本并提供迁移/回滚方案；不能在模块内部“顺手修”共享 API。API Review 至少检查：是否能删除多余入口、是否有隐式权限/副作用、是否泄漏实现、失败状态是否完整、是否能被受限测试替换。

模块化不等于把每个函数拆成一个类或文件。拆分依据是独立职责、独立所有权、稳定合同和可并行验证；强耦合的小实现留在同一模块，避免产生只有转发逻辑的空壳层。

## Boost 和依赖

- Boost.Asio：网络、异步 I/O、定时器和执行上下文。
- Boost.Beast：建立于 Asio 上的 HTTP/WebSocket 协议构件。
- Boost.JSON：JSON 解析、序列化和增量解析。
- Boost.Process：仅在产品确实需要受管子进程时评估；任何进程都必须有清晰的停止与退出归属。
- Boost 不替代 Provider 语义、Agent 编排、TUI、DPAPI、持久化 schema 或授权 policy。
- 每个第三方依赖在 Standards/DependencyPolicy.md 登记版本、用途、License、Windows 支持、传递运行库、升级责任和移除计划。
- 外部 TLS 库、数据库驱动和 Unicode/grapheme 实现均须经过单独依赖审查。

## 静态检查

本节定义目标检查门；当前是否已接入由 Workflow/EnforcementMatrix.yaml 的 Status 字段为准。规范写出检查命令不等于 CI 已执行或已通过。

- clang-format 是唯一格式化器，配置提交在 .clang-format。
- clang-tidy readability-identifier-naming 逐类执行 PascalCase。
- 文件夹/文件命名由路径检查脚本核对，并允许固定工具名例外。
- 项目自有代码的新 warning 默认视为任务未完成；第三方 warning 单独隔离记录。
- CI 至少执行 CMake 配置、目标构建、clang-tidy 命名规则和格式检查。每个小任务只验证受影响目标，阶段合并后再执行相应的集成验证。
- Clang-Tidy 支持对类、方法、成员、参数和变量分别配置命名规则：
  https://clang.llvm.org/extra/clang-tidy/checks/readability/identifier-naming.html
