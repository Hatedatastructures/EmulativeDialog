# HTTP Transport 合同 V1

状态：Frozen（ED-0120/R5）。Worker06 对 PR #60 固定 Base `79f94a4fa9c9dd41a4ad8187e878683f9c9808c7` / Head `f4c3203061c9ff5680fbc407695ad31d8805453e` 独立批准；该 PR 已于 `2026-10-29T21:23:00Z` squash merge 为 `f4c21b67591548b33a09f9c3b03d4c03ce2e5f28`。本状态只确认文档合同冻结，不实现 WinHTTP 传输代码（ED-0121），也不定义任何 Provider wire schema。

## 职责与依赖方向

`HttpTransportPort` 是 Provider Adapter 与 Windows 网络层之间唯一的公共边界：

- Provider Adapter 负责构造 wire 请求字节、解析 SSE/JSON 流、把协议事件映射到 `ModelProviderPort`。
- `HttpTransportPort` 只负责一次受 Core endpoint grant 约束的 HTTPS 交换：连接、TLS、发送、分块读取、超时、取消、脱敏终态。
- Transport 不解析 Provider JSON，不做 Provider 级重试，不访问 Channel/Core/记忆/TUI，不持有任何凭据存储引用。
- 离线 fake 实现同一 Port，用于合同与 Adapter 测试；fake 不使用网络、不读取任何真实密钥。

任何一方都不得把 WinHTTP/SSPI 类型、完整 URL 字符串、原始 header 集合或原始响应 body 暴露到 Port 之外。

`HttpTransportPort` 构造时必须接收非临时的 `EndpointGrantResolver` 左值（包括 const 左值）并以只读引用持有；调用方必须保证 resolver 活得比 Port 更久。Base 与 WinHTTP 的临时对象重载显式删除，所有派生 Port 的构造接口也必须保留 lvalue-only 约束，不得通过 forwarding reference 接收临时对象。唯一 public `Stream` 是基类非虚守卫入口：恰好调用一次 `Validate(Grant, Destination)`，仅在 `Valid` 时分派到受保护的 `DoStream`；其他状态都在任何 sink 或传输操作前映射为 `EndpointNotGranted`。

## 请求模型

一次调用提交恰好一个不可变请求：

| 字段 | 语义 |
|---|---|
| `TransportRequestId` | Core/Adapter 生成的本地关联 ID；只用于本地日志、事件、telemetry、diagnostics 相关性，绝不放入远端请求的 header、query 或 body，也不以其他方式发送给远端。它不是授权引用，不得替代 `EndpointGrantId`。 |
| `EndpointGrantId` | Core endpoint policy 签发的 bearer capability。只在本地内存中的 `TransportRequest` 与 resolver 校验流程中使用；不得进入日志、事件、telemetry、diagnostics、持久化记录、命令输出或用户可见输出，也不得发送给远端。`Stream` 校验必须与此 grant 和请求 destination 一起完成。`Missing`、`Expired`、`DestinationMismatch` 全部只映射为 `EndpointNotGranted`，不向调用方区分原因。 |
| `TransportMethod` | V1 只有 `Post`；新增动词需要合同修订。 |
| `HttpsDestination` | 由 grant 解析出的 host（DNS 名或 IP 字面量，不含 scheme/userinfo）、port 与绝对 path+query。redirect 不改变该字段。 |
| `Headers` | 有界 header 列表。**协议/profile allowlist 由 Adapter 负责**：Transport 看不到 profile，无法也不得判断某个 provider 专属 header 是否被授权。Transport 只负责拒绝 CR/LF 注入、大小/条数超限与 hop-by-hop header；`Host` 与 `Content-Length` 属 Transport 所有，Adapter 不得设置，Transport 依据 `HttpsDestination` 与 `BodyUtf8` 生成并忽略传入值。敏感值不得进入终态、事件或日志。 |
| `BodyUtf8` | 已编码的 wire 请求体（如 JSON 或 SSE 请求）。受 `Limits.MaximumRequestBytes` 约束。 |
| `Limits` | 请求/响应/单块/header 字节上限与 header 条数上限。缺省值见 Port 类型；调用方可收紧，不可放宽到未登记上限。 |
| `Timeouts` | connect/send/response/idle 四类超时；均为绝对上限，不允许被重试预算放大。 |
| `Proxy` | `TransportProxyMode::System`（默认，使用 WinHTTP/系统代理配置）或 `Direct`（显式直连）。Profile 的代理选项只映射到这两个值；不存在按请求传入代理 URL、PAC 来源、SOCKS 或代理凭据的形状。 |

请求不得包含完整 URL、Authorization 明文以外的任意凭据引用、ChannelEnvelope、用户会话身份、开发任务/仓库信息或任意文件路径。

## 流式结果

调用方提供两个可选 sink：

1. `ResponseHeadSink`：在第一个 body 块之前最多调用一次，携带 allowlisted 元数据 `StatusCode`、`ContentType`、`RetryAfter`。原始 header 不暴露。
2. `ResponseChunkSink`：按到达顺序递交有界字节块。块内容只在回调期间有效；调用方如需保留必须自行复制。

回调抛出异常属于调用方合同违约：Transport 必须以 `Failed{InternalError}` 终结且不得继续交付。Transport 自身绝不把实现异常泄漏到调用方。

每次调用恰好返回一个 `TransportTerminal`，之后不得再触发回调：

| 终态 | 含义 |
|---|---|
| `CompletedTransport{StatusCode, ReceivedBodyBytes}` | HTTP 交换完整结束。仅代表字节完整，不代表 Provider 语义成功；Provider 仍解析 SSE 并产出自己的终态。 |
| `FailedTransport{Code}` | 确定性失败，且 Transport 能确认请求结果（未发出或明确失败）。`Code` 为封闭枚举（见下）。 |
| `RedirectDeniedTransport{StatusCode, RedactedLocationHost}` | 收到 3xx；不跟随。只记录状态码和脱敏 Location host。 |
| `TimedOutTransport{Phase}` | 在 connect/send/response/idle 阶段超时。确定性超时，和 unknown outcome 区分。 |
| `CancelledTransport{RequestMayHaveReachedEndpoint}` | 取消。写出前为 false（确认未发送）；写出后为 true（不宣称远端停止，禁止自动重发）。 |
| `UnknownOutcomeTransport{Reason, RequestMayHaveReachedEndpoint=true}` | 请求可能已到达远端，但流在完整终态前中断。禁止盲目重试。 |

### 失败码 V1

| `TransportFailureCode` | 含义 |
|---|---|
| `InvalidRequest` | 请求结构、header、limit 或 timeouts 非法。 |
| `EndpointNotGranted` | 无有效 grant、grant 过期或与 destination 不一致。 |
| `DnsFailure` | 名称解析失败（确定未建立连接）。 |
| `ConnectFailure` | TCP 连接失败（确定未收到响应）。 |
| `TlsHandshakeFailure` | 证书链、主机名、协议或加密参数校验失败；fail-closed。 |
| `ProxyAuthenticationRequired` | 代理要求认证或认证被拒；不含代理凭据。 |
| `RequestWriteFailure` | 请求发送阶段失败，且能确认远端未得到完整请求。 |
| `ResponseReadFailure` | 读取阶段失败且可确认远端未产生可解释结果（否则必须 `UnknownOutcome`）。 |
| `ResponseLimitExceeded` | 超过响应字节上限。 |
| `HeaderLimitExceeded` | 超过 header 字节/条数上限。 |
| `InvalidHttpResponse` | 响应不符合 HTTP/1.1 或 TLS 之上的预期形态。 |
| `InternalError` | 实现内部错误。 |

失败与未知的判定顺序：若实现无法排除远端已经收到请求并可能产生副作用/计费，必须返回 `UnknownOutcome`，不得返回 `Failed`。若 cancel 发生在请求写出后，必须返回 `Cancelled{true}`，不得声称停止。

## 重定向、代理与安全边界

- 重定向：默认拒绝（`RedirectDenied`），绝不自动跟随。ED-0121 不得通过会话选项放宽。
- 代理：默认系统代理配置；profile 可显式选择直连。禁止按请求传入任意代理 URL、PAC 脚本来源、SOCKS 或认证凭据。
- TLS：系统信任根与系统 Schannel 策略；不提供跳过校验、自定义根、弱协议降级。企业 MITM 由系统管理员正常安装的根证书支持。
- DNS/SSRF：Core grant 绑定的 host 是唯一允许目标。实现必须在每次连接（含复用到新连接）时确保实际连接目标与 grant 一致；启动时 DNS 检查不能作为完整 SSRF 防护。
- header 责任划分：Adapter 负责只写入本协议/profile allowlist 允许的 header；Transport 看不到 profile，只负责拒绝名称或值中的 CR/LF、超过字节/条数上限，以及禁用 hop-by-hop header（`Connection`、`Keep-Alive`、`Proxy-Connection`、`Proxy-Authorization`、`TE`、`Trailer`、`Transfer-Encoding`、`Upgrade`，大小写不敏感）并返回 `InvalidRequest`/`HeaderLimitExceeded`。`Host` 与 `Content-Length` 是 framing header：Adapter 不得设置，Transport 必须依据 `HttpsDestination.Host`（含已登记端口）与 `BodyUtf8.size()` 自行生成，并忽略任何 Adapter 传入值；Port 以 `IsAdapterForbiddenHeader` 暴露这一 Adapter 禁用集合。
- 压缩：V1 不支持压缩。Transport 不设置 `Accept-Encoding`、不协商压缩、不自行解压，响应 body 字节原样交给 Adapter；任何非 `identity` 的 `Content-Encoding`（例如被中间层强制 gzip）都返回 `InvalidHttpResponse`，不得把压缩字节当 SSE 交付。
- `Retry-After`：Transport 把 delta-seconds 与 HTTP-date 两种合法形式统一归一化为相对当前响应的秒数；值非法/不可解析时置空，超过实现登记的合理上限时截断到该上限。归一化结果只是 allowlisted 元数据，是否重试由 Core/ProviderOutcomePolicy 决定。
- 脱敏：终态、事件、日志、telemetry、diagnostics、持久化记录、命令输出、用户可见输出与任务报告不得包含 `EndpointGrantId`、Authorization、API key、cookie、完整 URL、请求/响应正文或 Location 完整值。本地需要关联单次传输时使用 `TransportRequestId`；它不得被序列化进发送给 Provider 的任何内容。

## 健康检查

`Health(stop_token)` 返回 `Ready`/`Degraded`/`Unavailable`，不得发起需要真实聊天正文、模型输出或凭据的探测请求，也不得据此推断 Provider 可用性。

## 取消与停机

- `std::stop_token` 是唯一取消信号；Transport 不得自建全局取消开关。
- 预取消（调用前已请求停止）必须直接返回 `Cancelled{false}`，并确保零字节写出。
- 进程退出或连接断开发生在请求写出后，必须落为 `UnknownOutcome` 或 `Cancelled{true}`，由 Core 保留 OperationRecord，禁止自动重发。

## 离线 fake 与合同测试

`Tests/Contract/Platform/Network/HttpTransportPortTests.cpp` 中的脚本化 fake 证明：

1. Port 可由离线实现替换，不访问网络、不读取真实凭据。
2. 正常流：head 回调一次、两个 body 块、`Completed` 终态、字节数守恒。
3. TLS 失败 fail-closed 为 `TlsHandshakeFailure`。
4. 3xx 返回 `RedirectDenied`，只暴露脱敏 host。
5. 超时为 `TimedOut{Response}`，与 `UnknownOutcome` 明确区分。
6. 取消分两相：写出前 `Cancelled{false}`，mid-stream `Cancelled{true}`。
7. header CR/LF 注入被拒为 `InvalidRequest`；响应超限为 `ResponseLimitExceeded`。
8. 编译期断言公共类型不含 `Url`/`AuthorizationHeader`/`RawBody` 字段，证明 API 形状本身不开放任意 URL、原始凭据或原始响应体。
9. 代理模式：缺省为 `System`；显式 `Direct` 原样传递到实现；Port 无法表达代理 URL、PAC 来源或代理凭据。
10. grant 校验：`Missing`、`Expired`、`DestinationMismatch` 均终止为 `EndpointNotGranted` 且 head sink 与 chunk sink 零调用；`Valid` 才继续。
11. hop-by-hop header（如 `Connection`、`Transfer-Encoding`、`Proxy-Authorization`）被拒为 `InvalidRequest`，普通 header 可通过；allowlist 本身由 Adapter 负责。
13. framing header：`IsAdapterForbiddenHeader("Host")` 与 `("Content-Length")` 为真；Adapter 传入伪造值时 Transport 仍以 destination host 与实际 body 长度生成有效 framing。
12. 预取消（调用前已请求停止）返回 `Cancelled{false}` 且零字节；body sink 抛异常只会得到 `Failed{InternalError}`，不得变成成功或泄漏异常。

## 版本与兼容

- 本文件为 V1。新增终态、失败码、请求字段或动词必须走 ChangeRequest 并升级合同版本。
- ED-0121 实现 WinHTTP 传输时不得改变 V1 类型或语义；只实现该 Port。
- Provider Adapter（ED-0104/ED-0105/ED-0106）只依赖本 Port；不得直接调用 WinHTTP 或引入自有 TLS。
