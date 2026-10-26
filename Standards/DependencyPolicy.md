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