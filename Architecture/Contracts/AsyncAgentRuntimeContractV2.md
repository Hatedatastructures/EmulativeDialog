# Async Agent Runtime Contract V2

Status: Frozen candidate. This contract is additive to CoreAgentContract V1.1 (the CoreActionReceipt addendum), AsyncModelProviderContract V1, and the synchronous AgentRuntimePort. It supersedes the V1 async runtime behavior only for callers that opt into the V2 tool-loop port; the existing V1 RunTurnAsync API remains available and unchanged.

## Ownership and public boundary

Core remains the sole owner of trusted ChannelEnvelope validation, Principal and ConversationScope mapping, capability grants, action allowlists, action schemas, policy, and business-action execution. R5 definitions must declare `EffectClass::InProcessReadOnly`; Core rejects `ExternalWrite` and `ExternalIo` definitions before advertising or invoking them. An accepted handler may only compute from its in-memory input and trusted context. It cannot mutate persistent or cross-turn state or perform filesystem, database, network, provider, or channel I/O. Side-effecting actions require a separate OperationRecord and definite ChannelReceipt-backed task. The async runtime may present only the Core-provided canonical tool definitions to a provider. A provider FunctionCall is an untrusted proposal and is never authorization.

The additive `AsyncAgentToolRuntimePort::RunTurnWithToolsAsync` boundary receives the existing `AuthorizedTurnContext`, `AsyncModelProviderPort`, provider profile and credential lease, a Core-owned `AsyncBusinessActionDispatcherPort`, and a `std::stop_token`; it returns `boost::asio::awaitable<AgentRunResult>`. Existing `AsyncAgentRuntimePort::RunTurnAsync` and synchronous V1 entry points remain source-compatible. `CoreChatService::ProcessAsync` gains an overload accepting `AsyncAgentToolRuntimePort`; its new constructor overload accepts the Core-owned business action registry. It creates a per-turn dispatcher and awaits the V2 runtime path.

The Core dispatcher has two operations:

```cpp
std::vector<Providers::ToolDefinition> GetToolDefinitions(
    const AuthorizedTurnContext& Context) const;

boost::asio::awaitable<std::vector<AsyncBusinessActionReceipt>> DispatchAsync(
    const AuthorizedTurnContext& Context,
    const Providers::RequestId& Request,
    std::vector<Providers::FunctionCall> Calls,
    std::stop_token CancellationToken);
```

Each read-only result is a Core-created `CoreActionReceipt`, surfaced to the runtime as `AsyncBusinessActionReceipt`. It correlates the original `ToolCallId`, `RequestId`, `AgentTurnId`, and registered action name, and contains only a definite status (`Succeeded`, `Failed`, `Rejected`, or `Unsupported`) plus a stable sanitized result code. It contains no raw provider payload, credentials, chat body, channel response, or external-operation claim. It is not a `ChannelReceipt` and does not prove or authorize any external effect. An external write must still use an `OperationRecord` and a definite `ChannelReceipt` under CoreAgentContract.

The C++ signature is `boost::asio::awaitable<AgentRunResult> RunTurnWithToolsAsync(const AuthorizedTurnContext&, Providers::AsyncModelProviderPort&, const Providers::ModelProviderProfile&, const Providers::CredentialLease&, Core::AsyncBusinessActionDispatcherPort&, std::stop_token)` on `AsyncAgentToolRuntimePort`; the V2 overload does not change `AsyncAgentRuntimePort::RunTurnAsync`. `CoreChatService::ProcessAsync` gains an overload that accepts this V2 port and an async provider. Its V2 construction path holds the Core `BusinessActionRegistry`; per inbound message it builds one dispatcher bound to the resolved identity and turn, then awaits the V2 runtime before using the existing final Core/channel receipt path.

## Turn and request loop

The runtime builds the initial canonical transcript from the authorized user message. Every provider request uses a fresh unique `RequestId` and the same `AgentTurnId`. Tool definitions are taken only from the Core dispatcher. Provider events are accepted only when their request, turn, and sequence metadata match the current request.

`ToolCallReady` events are buffered. They are not dispatched unless the same request ends with exactly one valid `ResponseCompleted(ToolHandoff)` terminal and the usage budget permits dispatch. A final answer, refusal, failure, cancellation, output limit, malformed stream, or unknown provider outcome discards buffered proposals.

After a valid handoff, the runtime awaits the Core dispatcher. It appends one assistant message containing the canonical function calls and one `MessageRole::Tool` message per definite `CoreActionReceipt`, linked by `ToolCallResultId`. Only a correlated receipt and its stable sanitized code may enter that transcript. It then starts another provider request with a new RequestId. Rejected, unsupported, or definite failed receipts may be returned to the model as tool results so it can produce a user-facing response. The dispatcher never returns an unknown external-action outcome in R5 because external actions are ineligible. If Core cannot produce a definite read-only receipt, the turn stops without adding a tool result or continuing the provider loop.

A turn is bounded to at most four handoff cycles and eight proposed tool calls total. Reaching either limit returns a definite output-limit failure and dispatches no additional action. Requests execute serially; the runtime must not detach provider or action work.

## Cancellation and failures

Cancellation during a read-only handler returns Cancelled without manufacturing a receipt or continuing the provider loop. A handler exception returns a definite Failed `CoreActionReceipt` only when Core can establish that result safely; otherwise Core stops with no receipt and no provider continuation. R5 handlers are prohibited from starting external work, so action exceptions never produce an external `UnknownOutcome` receipt. Provider requests retain V1 behavior: after a request has been sent, an unknown provider outcome stops the turn as `AgentRunResult::UnknownOutcome` and is never retried to infer an outcome.

Provider profile, event correlation, usage, refusal, failure, output-limit, and cancellation mapping retain their V1 rules. `AgentRunResult::UnknownOutcome` is reserved for uncertain provider request outcomes in R5. Core read-only action errors map only to a definite failure receipt when established; otherwise the dispatcher stops without a receipt or continuation. All awaits use Boost.Asio; wrapping synchronous calls in a blocking wait is not conforming.

## Compatibility and exclusions

The V1 synchronous ports and the V1 async single-request port retain their signatures and behavior. The tool loop accepts only Core-registered in-process read-only business actions whose effect class is `InProcessReadOnly`. External writes, persistence, filesystem/database/network I/O, channel actions, Shell, host commands, arbitrary files, browser, and MCP tools are forbidden. This contract does not authorize real QQ or real provider traffic in its deterministic test slice.
