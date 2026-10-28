# ADR-0003：Windows 原生 HTTPS Provider Transport 采用 WinHTTP + 系统 Schannel 信任

状态：Accepted（2026-10-28，ED-0120/R2；独立评审通过后随 HttpTransportContract V1 一起生效）。

## 背景

三种 Provider 协议（OpenAI Responses、OpenAI-compatible Chat Completions、Anthropic Messages）都需要同一条出站 HTTPS 流式通道。OD-0007 要求在此之前固定 Boost 与 TLS 选择。Boost.Asio 1.92.0 已由 ED-0131 固定为构建基线；本 ADR 解决 TLS 与 HTTP 栈选择，并冻结三种 Provider 共用的 C++ `HttpTransportPort` 语义。

产品运行时是 Windows 原生 C++20，不打包 Rust/Node/Python 运行时。聊天 Agent 只允许 Core 登记过的 Provider HTTPS endpoint，不能变成通用网络工具。

## 决策

1. **HTTP 栈使用 WinHTTP**（`winhttp.dll`，Windows 系统组件）。它提供异步/同步 HTTP/1.1 客户端、系统代理发现、连接复用与流式读取，且不引入额外运行库。
2. **TLS 使用 WinHTTP 内置 Schannel**。产品不直接调用 Schannel SSPI 接口，也不自带 OpenSSL/BoringSSL。根证书使用 Windows 系统信任根；不接受应用注入的自定义根，也不提供 `--insecure`/跳过校验开关。
3. **证书失败一律 fail-closed**。`ERROR_WINHTTP_SECURE_FAILURE` 等安全错误必须映射为 `TlsHandshakeFailure` 终态；不得降级重试、不得换协议、不得把错误响应正文当成功数据。
4. **代理策略**：默认使用系统配置（WinHTTP 默认代理设置或当前用户 AutoProxy），支持 profile 显式声明“直连，不使用代理”。Port 只以 `TransportProxyMode::System`（默认）与 `TransportProxyMode::Direct` 表达，profile 选项只能映射到这两个值；不支持按请求传入任意代理 URL、SOCKS 或用户脚本来源；代理认证失败映射为 `ProxyAuthenticationRequired`，且不在错误里回显代理凭据。
5. **超时策略**：连接、发送、响应与空闲四类超时分别配置并映射到 WinHTTP 的 resolve/connect/send/receive 超时；超时是确定性 `TimedOut` 终态，不同于 `UnknownOutcome`。
6. **重定向默认拒绝**：使用 `WINHTTP_OPTION_REDIRECT_POLICY_NEVER` 等价策略。3xx 响应返回 `RedirectDenied`，只携带状态码和脱敏 Location host，不跟随到任何新 host，也不把 Location 原样透出。ED-0121 实现时不得通过 `WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS/DISALLOW_HTTPS_TO_HTTP` 放宽。
7. **取消语义**：请求字节写出前的取消是 `Cancelled{RequestMayHaveReachedEndpoint=false}`；写出后或流已开始后的取消为 `Cancelled{RequestMayHaveReachedEndpoint=true}`，禁止声称远端已停止，禁止自动重发。
8. **未知结果**：请求已发出但流在终态前中断，返回 `UnknownOutcome`；Core 侧不得盲目重试，Provider 重试只能发生在明确分类为可重试的终态上。
9. **凭据与日志**：Authorization 等敏感头只存在于请求对象与内存中。Transport 终态类型只含状态码、脱敏 host、字节数与错误码；诊断日志只记录请求 ID、endpoint grant ID、状态码、错误码、耗时，绝不记录 header、body 或完整 URL。
10. **endpoint 来源**：Transport 不接受任意 URL 字符串。请求必须引用 Core endpoint policy 签发的 `EndpointGrantId` 与已解析的 `HttpsDestination`（scheme 固定 HTTPS，含 host/port/path）。HttpTransport 在构造时注入 Core 提供的只读 `EndpointGrantResolver`，`Stream` 必须在 DNS/connect 前调用 `Validate`，并把 `Missing`/`Expired`/`DestinationMismatch` 统一映射为 `EndpointNotGranted`；ED-0121 实现还必须拒绝未登记端口、含 userinfo 的 host、CR/LF 注入 header、hop-by-hop 或 `Proxy-Authorization` header 与超限请求。`Host` 与 `Content-Length` 由 Transport 依据 `HttpsDestination` 和 `BodyUtf8` 生成，Adapter 传入值不得生效。

## 备选方案

| 方案 | 结论 | 理由 |
|---|---|---|
| Boost.Beast + OpenSSL | 不采用 | 需要额外构建与分发 OpenSSL，证书/根管理与系统策略分离；与“Windows 原生、最小运行时依赖”冲突。 |
| Boost.Beast + Schannel 手写 TLS 流 | 不采用 | 需要自研 SSPI/Schannel 流封装和证书错误分类，复杂度和安全面都高于 WinHTTP。 |
| libcurl | 不采用 | 引入第三方二进制与其 TLS 后端选择，加宽依赖和供应链面。 |
| WinHTTP + 系统 Schannel（本决策） | 采用 | 系统组件、系统信任根、内置代理与超时控制，错误码可映射为本合同的封闭失败码。 |

## 后果

- Provider Adapter 只依赖 `HttpTransportPort`，不接触 WinHTTP 类型；离线测试用脚本化 fake 替换实现，不需要网络。
- WinHTTP 是 Windows 专属；Linux/macOS 平台适配留待独立任务，不阻塞当前产品范围。
- 系统根证书、企业代理与 TLS 检查策略由 Windows 管理；产品不提供绕过手段。企业 MITM 证书须由系统管理员正常安装，产品不感知。
- `TransportRequest` 携带 Core 提供的 endpoint grant 与已解析目标；SSRF 防护不能只靠启动时 DNS 检查。ED-0121 必须把 DNS/代理解析纳入每次连接的授权目标校验，并在重定向、代理和 DNS 变化时保持 fail-closed。
- 429/503 的 `Retry-After` 由 transport 以 allowlisted 元数据传输；是否重试由 Core/ProviderOutcomePolicy（ED-0111）决定，transport 不自行重试。

## 证据来源

- WinHTTP 会话与代理概览：<https://learn.microsoft.com/windows/win32/winhttp/winhttp-sessions-overview>
- `WinHttpSetTimeouts`：<https://learn.microsoft.com/windows/win32/api/winhttp/nf-winhttp-winhttpsettimeouts>
- 会话选项 `WINHTTP_OPTION_REDIRECT_POLICY`：<https://learn.microsoft.com/windows/win32/api/winhttp/nf-winhttp-winhttpsetsessionoption>
- 选项常量（含 `WINHTTP_OPTION_SECURITY_FLAGS`）：<https://learn.microsoft.com/windows/win32/winhttp/option-flags>
- WinHTTP 错误消息（含 `ERROR_WINHTTP_SECURE_FAILURE`）：<https://learn.microsoft.com/windows/win32/winhttp/error-messages>
- 流式读取 `WinHttpQueryDataAvailable`/`WinHttpReadData`：<https://learn.microsoft.com/windows/win32/api/winhttp/nf-winhttp-winhttpquerydataavailable>
- 当前用户代理配置 `WinHttpGetIEProxyConfigForCurrentUser`：<https://learn.microsoft.com/windows/win32/api/winhttp/nf-winhttp-winhttpgetieproxyconfigforcurrentuser>
- Schannel 安全支持提供程序：<https://learn.microsoft.com/windows/win32/secauthn/schannel-security-support-provider>
