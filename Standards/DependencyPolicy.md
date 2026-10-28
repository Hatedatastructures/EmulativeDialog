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
| Boost | 1.92.0 / `afdfa32505af73e3d208144b3f623f0096cb62b6` (`boostorg/boost`) | Boost Software License 1.0；重分发时按上游许可证保留版权和许可文本。 | Pin owner：BuildAndDependencyOwner。`EmulativeDialogBoostJson` 是当前构建基线；Provider adapters 为后续消费者，包括 NetworkPlatform、ProviderOpenAIResponses、ProviderOpenAIChatCompletions、ProviderAnthropicMessages。 | Asio 用于异步网络/I/O，Beast 用于 HTTP/WebSocket 构件，JSON 用于 Provider wire 编解码。Boost.Process 目前仅为候选库，不由本基线启用。ED-0131 在 Windows MinGW 下编译并运行 Asio C++20 协程探针；ED-0143 编译并运行 Boost.JSON 解析/序列化探针；Beast 和 Process 尚未由探针编译。 | `BoostAsioCoroutineProbe` 使用 header-only Asio 和进程内 `io_context`；Windows target 链接系统 `ws2_32` 以满足 Asio 的 Winsock 初始化引用。`EmulativeDialogBoostJson` 是单一实现静态库：仅 `CMake/BoostJsonImplementation.cpp` 包含 `<boost/json/src.hpp>`，Provider 与 `BoostJsonProbe` 只包含 `<boost/json.hpp>` 并链接该库，避免多翻译单元 ODR 冲突。两者均不新增第三方运行时 DLL；其他 Boost 库的链接模式须由对应消费者登记。 | BuildAndDependencyOwner 维护根 pin。升级需 ChangeRequest、上游差异/安全审查和 Windows 消费者验证。无消费者且所有调用方已迁移后，移除固定源包解析及本登记。 |
| yaml-cpp | 0.9.0 / `56e3bb550c91fd7005566f19c079cb7a503223cf` (`jbeder/yaml-cpp`) | MIT；重分发时在副本或重要部分中保留版权和许可声明。 | Pin owner：BuildAndDependencyOwner。预期消费者：AgentWorkflowController。 | 用于 C++ 开发期工具读取/校验 YAML 任务和计划元数据；不进入聊天产品运行时。上游提供 CMake 构建并包含 Windows/MSVC 修复记录；本任务的 MinGW 构建仅覆盖 BuildProbe，尚未编译 yaml-cpp。 | 本任务不下载、不链接 yaml-cpp。消费者接入时须明确静态/动态链接模式、是否有 DLL，以及其随工具分发的方式。 | BuildAndDependencyOwner 维护根 pin。升级需 ChangeRequest、上游差异/安全审查和 Windows 消费者验证。Controller 不再依赖 YAML 后移除 FetchContent 声明及本登记。 |
| SQLite | 3.50.1 / `sqlite-amalgamation-3500100.zip`，SHA256 `41716B44AC8777188C4C3F1F370F01C9CB9E3B6428EB5C981D086C35DE2D9D3F`（`https://www.sqlite.org/2025/`） | Public Domain。SQLite 源码由作者放弃版权，可自由复制、修改、发布。 | Pin owner：BuildAndDependencyOwner。预期消费者：Storage（ED-0123）、聊天 Memory（ED-0113）。 | 官方 amalgamation 中的 `sqlite3.c` 由本仓库 CMake 编译成静态库 `EmulativeDialogSQLite`；不链接系统 `sqlite3`，不引入 DLL。Windows x64 MinGW 下由 `SQLiteBuildProbe` 验证 `sqlite3_libversion_number() > 0`。 | 静态链接；编译宏 `SQLITE_THREADSAFE=1`、`SQLITE_OMIT_LOAD_EXTENSION`、`SQLITE_DQS=0`、`SQLITE_DEFAULT_MEMSTATUS=0`；Windows 额外链接 `ws2_32`。探针不得导入 `sqlite3`/`libgcc`/`libstdc++`/`libwinpthread` 运行时 DLL（静态链接构建）。 | BuildAndDependencyOwner 维护根 pin。升级 = 替换 `EmulativeDialogSqliteVersion`、URL 与 SHA256，并重跑 `SQLiteBuildProbe` 与存储测试。移除 = 删除 FetchContent 声明、`EmulativeDialogSQLite` 目标与登记，确认无消费者后再删代码。离线构建可用 `-DFETCHCONTENT_SOURCE_DIR_SQLITE=<已校验解压目录>` 注入同版本源。 |

`BoostAsioCoroutineProbe` 是依赖构建门：它从官方 Boost superproject 的固定 commit `afdfa32505af73e3d208144b3f623f0096cb62b6` 检出 Asio 所需子模块，再由这些固定子模块头文件编译并验证 `awaitable`、`co_spawn`、`io_context` 和 executor 行为。空缓存下的 configure 只登记 pinned ExternalProject，不假设源码已存在；缓存已存在时 configure 会提前暴露 checkout 漂移。构建阶段 `EmulativeDialogBoostAsioVerify` target 依赖 `EmulativeDialogBoostAsioSource`，在填充后执行 `git rev-parse HEAD` 与子模块/许可证完整性校验，探针又依赖该 verify target，因此任何 commit 漂移或缺失子模块都会在编译前使构建失败。`EmulativeDialogBoostAsio` 仅供后续 C++ consumer 复用固定头和 C++20 要求。探针不读取系统 MSYS2 Boost，也不打开 socket、不查询 DNS、不建立 TLS，不使用凭据或外部账号。

`BoostJsonProbe` 是 Provider 解析构建门：它复用同一固定 superproject commit `afdfa32505af73e3d208144b3f623f0096cb62b6` 的 checkout，补齐 `libs/json`、`libs/container`、`libs/describe` 与既有公共子模块。填充和校验是两个阶段：`EmulativeDialogBoostAsioSource` 的 populate step 负责按 superproject gitlink 初始化缺失子模块；`EmulativeDialogBoostJsonVerify` 只读校验 commit、13 个子模块目录、`boost/json.hpp`、`boost/version.hpp` 与 `LICENSE_1_0.txt`，不得联网或修改源码树。缓存已存在时由 `EmulativeDialogBoostJsonPopulate` 完成同样的惰性填充。配置 `-DEMULATIVE_DIALOG_OFFLINE=ON` 时，缺少 pinned checkout 会在 configure 失败，缺少子模块会在 populate 阶段失败且不会执行网络更新。Boost.JSON 1.92.0 在本项目采用单一编译实现库：仅 `CMake/BoostJsonImplementation.cpp` 一个翻译单元包含 `<boost/json/src.hpp>` 并由 `EmulativeDialogBoostJson` STATIC target 编译；`EmulativeDialogBoostJson` 是当前构建基线，Provider adapters 是后续消费者；它们与 `BoostJsonProbe` 都只包含 `<boost/json.hpp>` 并链接同一静态库，避免多个 TU 重复实例化造成 ODR 冲突。STATIC target 不携带 `winpthread` 链接项；MinGW 的最终 executable/test 使用 `-no-pthread` 关闭 GCC 隐式动态 `-lpthread`，并把 `EmulativeDialogBoostJsonWinPthreadLinkOption` 的单条 `--push-state/-Bstatic/-lwinpthread/-Bdynamic/--pop-state` 组合放在静态库之后的最终链接项中。`Dependency.BoostJsonWinPthreadLink` 直接断言 `BoostJsonProbe` 的真实链接行同时包含这两个选项，同时 configure 断言静态 target 的 `INTERFACE_LINK_LIBRARIES` 和 `INTERFACE_LINK_OPTIONS` 不含 winpthread 或链接状态。探针导入表仅保留既有 `libgcc_s_seh-1.dll`/`libstdc++-6.dll` 与系统 DLL。探针覆盖对象、数组、字符串、数字、序列化/反序列化往返以及非法输入 fail-closed；不打开网络、不读取凭据、不访问文件。

该目标不引入传递运行库或 DLL。Boost.JSON 编译为本仓库静态实现库，不产生独立 Boost DLL；yaml-cpp 的生产链接和打包仍由 WorkflowController consumer 验证；Beast 和 Boost.Process 在各自消费者接入前不能从 pin 的存在推断已经编译或验收。

固定来源与许可证证据：

- Boost：[1.92.0 发布说明](https://www.boost.org/releases/1.92.0/)、[固定 superproject 提交](https://github.com/boostorg/boost/commit/afdfa32505af73e3d208144b3f623f0096cb62b6)、[该提交的 Boost Software License 1.0](https://github.com/boostorg/boost/blob/afdfa32505af73e3d208144b3f623f0096cb62b6/LICENSE_1_0.txt)。
- yaml-cpp：[0.9.0 发布记录](https://github.com/jbeder/yaml-cpp/releases/tag/yaml-cpp-0.9.0)、[固定提交](https://github.com/jbeder/yaml-cpp/commit/56e3bb550c91fd7005566f19c079cb7a503223cf)、[该提交的 MIT License](https://github.com/jbeder/yaml-cpp/blob/56e3bb550c91fd7005566f19c079cb7a503223cf/LICENSE)。
- SQLite：[3.50.1 amalgamation](https://www.sqlite.org/2025/sqlite-amalgamation-3500100.zip)，SHA256 见登记表；[SQLite 版权声明](https://www.sqlite.org/copyright.html)。归档在 FetchContent populate 时按 SHA256 校验，解压后仅 `sqlite3.c`/`sqlite3.h` 进入静态目标。
