# 依赖准入策略

## 产品源代码范围

发布运行时的自有模块全部是 C++。项目允许操作系统 API、C ABI 系统库和经审查的第三方依赖。构建、文档、CI 和索引使用工具要求的元文件格式；它们不属于产品 Agent 运行时。

“所有代码都用 C++”的验收范围应写成“所有发布运行逻辑使用 C++”。强迫 CMake、JSON、许可证文本和官方 API 字段也变成 C++ 没有实际意义，并会破坏工具互操作。

## Boost 基线

| Boost 库 | 预期用途 | 限制 |
|---|---|---|
| Boost.Asio | C++ 异步网络、I/O、timer/executor | 网络 policy 和业务重试由产品模块控制 |
| Boost.Beast | Asio 上的 HTTP/1 与 WebSocket 构件 | Provider request/response 语义仍由协议 Adapter 实现 |
| Boost.JSON | JSON 解析/序列化和增量解析 | 对外 JSON 字段名保留对应协议格式 |
| Boost.Process | 需要受管子进程时使用 | 子进程所有者、stdout/stderr、退出和取消须定义 |

Boost 的官方资料把 Asio 归为网络/低层 I/O，把 Beast 描述为 Asio 上的 HTTP/WebSocket 构件，把 Boost.JSON 描述为 JSON 操作库：
https://docs.cppalliance.org/user-guide/task-networking.html
https://docs.cppalliance.org/user-guide/library-table.html

## TLS、存储和终端

- Boost.Beast 不是完整 TLS trust policy。P0 必须决定 Windows 使用 OpenSSL 还是 Schannel，并记录 CA、证书错误、proxy 和 redirect 处理。
- SQLite、Win32 DPAPI、Windows named pipe 和 Console API 通过小型 PascalCase wrapper 接入。
- 产品 TUI 的 screen buffer、layout、widget、input router 由项目自研 C++ 模块，不引入第三方 TUI renderer。
- 对 Unicode grapheme/display width，需要决定使用审查过的 C++ Unicode 库、系统 API 或维护项目内受限表。不能用 UTF-8 字节数冒充终端显示宽度。

## 依赖登记表

每个非标准依赖在纳入前记录：

- DependencyId、精确版本或 commit、下载来源。
- OwnerModule 和用途。
- License、再分发要求以及动态/静态链接状态。
- Windows x64 编译证据和运行时 DLL/资产。
- 传递依赖、编译宏、TLS/平台约束。
- 安全更新责任人和版本升级路径。
- 替代/删除方式。

只在指定版本的配置和 build manifest 中声明依赖。Provider task 不得直接更新根 Boost 版本；根依赖由 DependencyOwner 负责，相关 API 变更要走 ChangeRequest。

## P1 构建基线依赖

根 CMake 版本和提交固定在 `CMake/DependencyPins.cmake`。FetchContent 只声明不可变提交；它不会下载或构建这些依赖，直到某个消费者显式调用 `FetchContent_MakeAvailable`。Boost.Asio 构建门只填充冻结 commit 下所需子模块，不构建 Boost 自身。`BuildProbe` 不使用第三方依赖。

| DependencyId | 精确版本/提交 | License 与分发要求 | Pin owner / Consumer | 用途与 Windows 状态 | 链接与运行时 | 升级与移除 |
|---|---|---|---|---|---|---|
| Boost | 1.92.0 / `afdfa32505af73e3d208144b3f623f0096cb62b6` (`boostorg/boost`) | Boost Software License 1.0；重分发时按上游许可证保留版权和许可文本。 | Pin owner：BuildAndDependencyOwner。当前消费者：RootBuildAndDependencies；后续消费者：NetworkPlatform、ProviderOpenAIResponses、ProviderOpenAIChatCompletions、ProviderAnthropicMessages。 | Asio 用于异步网络/I/O，Beast 用于 HTTP/WebSocket 构件，JSON 用于 Provider wire 编解码。Boost.Process 目前仅为候选库，不由本基线启用。ED-0131 使用 Windows MinGW 编译并运行 Asio C++20 协程探针；Beast、JSON 和 Process 尚未由该探针编译。 | `BoostAsioCoroutineProbe` 使用 header-only Asio 和进程内 `io_context`；Windows target 链接系统 `ws2_32` 以满足 Asio 的 Winsock 初始化引用。不新增第三方运行时 DLL。其他 Boost 库的链接模式须由对应消费者登记。 | BuildAndDependencyOwner 维护根 pin。升级需 ChangeRequest、上游差异/安全审查和 Windows 消费者验证。无消费者且所有调用方已迁移后，移除固定源包解析及本登记。 |
| yaml-cpp | 0.9.0 / `56e3bb550c91fd7005566f19c079cb7a503223cf` (`jbeder/yaml-cpp`) | MIT；重分发时在副本或重要部分中保留版权和许可声明。 | Pin owner：BuildAndDependencyOwner。预期消费者：AgentWorkflowController。 | 用于 C++ 开发期工具读取/校验 YAML 任务和计划元数据；不进入聊天产品运行时。上游提供 CMake 构建并包含 Windows/MSVC 修复记录；本任务的 MinGW 构建仅覆盖 BuildProbe，尚未编译 yaml-cpp。 | 本任务不下载、不链接 yaml-cpp。消费者接入时须明确静态/动态链接模式、是否有 DLL，以及其随工具分发的方式。 | BuildAndDependencyOwner 维护根 pin。升级需 ChangeRequest、上游差异/安全审查和 Windows 消费者验证。Controller 不再依赖 YAML 后移除 FetchContent 声明及本登记。 |

`BoostAsioCoroutineProbe` 是依赖构建门：它从官方 Boost superproject 的固定 commit `afdfa32505af73e3d208144b3f623f0096cb62b6` 检出 Asio 所需子模块，再由这些固定子模块头文件编译并验证 `awaitable`、`co_spawn`、`io_context` 和 executor 行为。CMake 在读取头文件前执行 `git rev-parse HEAD`，任何 checkout commit 漂移都会使配置失败。`EmulativeDialogBoostAsio` 仅供后续 C++ consumer 复用固定头和 C++20 要求。探针不读取系统 MSYS2 Boost，也不打开 socket、不查询 DNS、不建立 TLS，不使用凭据或外部账号。

该目标不引入传递运行库或 DLL。yaml-cpp 的生产链接和打包仍由 WorkflowController consumer 验证；Beast、JSON 和 Boost.Process 在各自消费者接入前不能从 pin 的存在推断已经编译或验收。

固定来源与许可证证据：

- Boost：[1.92.0 发布说明](https://www.boost.org/releases/1.92.0/)、[固定 superproject 提交](https://github.com/boostorg/boost/commit/afdfa32505af73e3d208144b3f623f0096cb62b6)、[该提交的 Boost Software License 1.0](https://github.com/boostorg/boost/blob/afdfa32505af73e3d208144b3f623f0096cb62b6/LICENSE_1_0.txt)。
- yaml-cpp：[0.9.0 发布记录](https://github.com/jbeder/yaml-cpp/releases/tag/yaml-cpp-0.9.0)、[固定提交](https://github.com/jbeder/yaml-cpp/commit/56e3bb550c91fd7005566f19c079cb7a503223cf)、[该提交的 MIT License](https://github.com/jbeder/yaml-cpp/blob/56e3bb550c91fd7005566f19c079cb7a503223cf/LICENSE)。
