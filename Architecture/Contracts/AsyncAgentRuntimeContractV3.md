# Async Agent Runtime Contract V3 — Per-Request Stream Resource Bounds

Status: Proposed for fixed-head review. This exact revision becomes Frozen only after independent review approval and merge; before that merge it is a proposal, not a product runtime guarantee.

## Purpose and compatibility

V3 adds bounded retained-state accounting and overflow behavior to the asynchronous AgentRuntime request lifecycle designated by ED-0157/R1. It is additive to the frozen async AgentRuntime behavior and does not replace or change the synchronous `AgentRuntimePort`, `AsyncAgentRuntimePort`, `AsyncModelProviderPort`, `ModelProviderContract`, or `CoreAgentContract` APIs. Existing V1 callers remain source-compatible.

The bounds apply to one AgentRuntime-owned provider request: one invocation of the asynchronous provider streaming operation for one `RequestId`. Counters begin at zero before that operation can deliver an event and are reset only when a later provider request is explicitly started by the existing AgentRuntime/Core turn flow. A new request does not reset the existing per-turn tool limits.

V3 bounds data retained or accumulated by AgentRuntime after it receives canonical provider events. They do not bound raw HTTP/SSE/JSON wire buffers or allocations made inside a provider adapter before canonical event delivery. Provider adapters remain responsible for their own wire parsing bounds. A callback's already-materialized canonical input is transient borrowed input; the runtime must not copy or retain it before the V3 admission checks pass.

## Per-request limits

All KiB values below mean 1,024 bytes. Limits are inclusive: a request may retain exactly the stated maximum; the next item that would exceed any maximum latches the local resource stop.

| Counter | Maximum | What is charged |
|---|---:|---|
| Canonical events | 1,024 events | Each nonterminal `EventSink` callback and the single terminal returned by the provider awaitable, each counted once. The returned terminal is included even though it is not sent through the sink. |
| Dynamic string payload | 256 KiB | The sum of `std::string::size()` for every dynamically sized string field that AgentRuntime retains from canonical event metadata and payloads, plus nested JSON string keys and values. |
| Text/refusal/safe-generated text | 64 KiB | The combined UTF-8 byte length appended to the request's text/refusal accumulator from `TextDelta`, `RefusalDelta`, and safe generated text carried by `ResponseOutputLimited`. |
| Complete tool arguments | 64 KiB | The sum, across all complete `ToolCallReady` argument values in this provider request, of each value's compact canonical JSON serialization in UTF-8 bytes. This is aggregate, not a per-call allowance. |
| Pending tool calls | 8 distinct calls | Calls first observed as `ToolCallStarted` and still incomplete, plus complete `ToolCallReady` calls not yet dispatched to Core or discarded. |

### String and byte accounting

`std::string::size()` is the byte count for string fields. Canonical string data is UTF-8; counting uses its stored payload octets without Unicode normalization, code-point counting, or allocator-capacity accounting. Countable strings include, where present, request/turn/response/item/tool identifiers, tool names, text and refusal deltas, tool argument deltas, stable reason/error/stop codes, sanitized descriptions retained in canonical state, safe generated text, JSON object member names, and recursively nested JSON string values. Numeric, boolean, null, enum, pointer, allocator, and container bookkeeping bytes are not dynamic-string payload.

Every retained string field is charged once for each retained representation. If the same logical bytes are separately retained in a delta string and in a parsed JSON value, both representations are charged. If a payload is referenced or observed without being copied or appended to AgentRuntime-owned request state, it is not charged as retained state. It must still pass all applicable size checks before any copy, append, or other retention.

For complete tool arguments, the byte count is the UTF-8 output length of the deterministic compact canonical JSON serializer used by the AgentRuntime/Core validation path: arrays preserve order, object members use that serializer's deterministic canonical ordering, and insignificant whitespace is omitted. The source argument-delta bytes are charged separately by the dynamic-string counter. The implementation must determine the canonical output length with a bounded counting pass (or an equivalent serializer that stops at the remaining allowance); it must not first allocate an unbounded duplicate serialization merely to discover that the limit was exceeded. Canonical JSON string keys/values also contribute to the dynamic-string counter when retained as parsed argument data.

Text/refusal accounting charges bytes when newly appended to the request accumulator; a later view or terminal snapshot of already accumulated bytes does not charge them again. Tool argument strings are not text/refusal output, but remain subject to both the dynamic-string and complete-tool-argument counters. A text or argument value is never truncated to fit a limit.

## Admission before retention and overflow-safe counters

For each incoming canonical event and for the returned terminal, the runtime computes all applicable counter deltas before retaining that event, appending its text, parsing/copying its argument strings into retained state, or exposing a tool handoff. It checks every relevant limit as one admission decision. No part of an event is committed if any applicable limit would be exceeded.

Counter arithmetic must not wrap. For byte counters, with `used <= maximum`, an increment `amount` is admissible only when `amount <= maximum - used`; only then may the runtime add it. Event and pending-call counters use the same remaining-capacity rule and are never incremented past their maximum. If the representation or input length cannot be measured without overflow, the runtime treats it as a limit breach and fails closed.

The event counter is charged before event retention. A nonterminal event that would be event 1,024 may be admitted when it is within all other limits; a subsequent returned terminal would then exceed the 1,024-event maximum and latch the stop. The terminal counts exactly once, as the result of the awaited provider operation. The provider contract forbids emitting the terminal through `EventSink` as well.

For tool calls, a call becomes pending at its first valid `ToolCallStarted`. Its stable `ToolCallId` must be unique within the request; duplicate or out-of-order lifecycle events remain subject to the existing provider stream validation and must not be merged into an existing call. `ToolArgumentDelta` does not release the pending slot. `ToolCallReady` marks that same call complete but it remains pending until the existing Core handoff dispatches it or the request is discarded. An incomplete call is never promoted to a complete call for purposes of fitting the budget.

## Resource-limit stop lifecycle

When an event, terminal, text append, complete-argument value, or new pending call would cross any limit, the runtime atomically latches the first local resource-limit condition before retaining the offending payload. It requests stop on the runtime-owned `std::stop_source` whose token was forwarded to the provider operation. The runtime composes the caller's cancellation request into this source without blocking or detaching work.

After the latch:

1. No bytes, event, or tool-call state from the offending item or later callbacks are accumulated. Later callbacks return promptly and are ignored for retention.
2. The runtime does not publish the partial answer, dispatch any complete or incomplete tool call, create a `CoreActionReceipt`, start another provider request, retry/replay the request, or continue the turn.
3. The runtime still awaits the original provider awaitable to completion. It keeps all borrowed request/profile/credential/event-sink inputs alive until that awaitable completes and event delivery has stopped.
4. No blocking wait, detached coroutine, or lifetime extension beyond the awaited operation is permitted.

The returned terminal is inspected only as needed to determine whether the provider outcome is definitive or unknown. Once a limit is latched, it is not retained as response content. If the awaited operation escapes with an exception and dispatch may have started, the result is uncertain and follows the `UnknownOutcome` precedence below.

## Terminal and exception precedence

`ResponseOutcomeUnknown` and an escaped provider-call exception after the request may have been dispatched are outcomes of uncertain remote work. They map to `AgentRunResult::UnknownOutcome`, even if caller cancellation or a local resource limit was also observed. Unknown work is never retried, replayed, dispatched as a tool handoff, or continued.

If a local resource limit was latched and the provider then returns any definitive terminal (including a successful completion, output-limited result, known cancellation, or explicit failure), AgentRuntime discards that terminal and returns the existing `AgentRunResult::Failed(OutputLimitReached)`. This local limit result takes precedence over ordinary cancellation or definitive provider failure; only an uncertain provider outcome takes precedence over the limit. No partial accumulated text or ready tool call is returned with `OutputLimitReached`.

If no local resource limit was latched, V2 terminal mapping and cancellation semantics remain unchanged: the runtime returns the existing result for the definitive terminal, maps an uncertain result to `UnknownOutcome`, and never treats `ResponseOutputLimited` as a complete successful answer. Provider exceptions known to have occurred before any dispatch may be mapped only through the existing safe failure/cancellation rules; an exception without proof that dispatch did not start is uncertain.

## Existing turn and authorization bounds remain in force

V3 does not grant permission to execute model-proposed tools and does not change the Core-owned identity, route, deduplication, schema, capability, scope, receipt, or policy checks. A complete `ToolCallReady` is still only a provider proposal. Existing Core authorization and canonical terminal requirements continue to govern any handoff.

The existing per-turn limit of at most four dispatched tool cycles and at most eight dispatched calls remains unchanged. V3's maximum of eight pending calls applies only within one provider request and cannot increase the per-turn dispatched-call allowance. V3 adds no output-limit continuation or automatic retry. A resource-limit stop ends this request's AgentRuntime continuation path as described above.

## Explicit non-goals

This contract does not add protocol wire-size limits, HTTP/network behavior, provider adapters, new canonical event or C++ API types, a tool executor, external side effects, channel delivery, memory/persistence, or changes to synchronous AgentRuntime/Provider APIs. It does not promise that a provider stops remote work merely because the local stop token was requested; uncertain remote outcomes remain `UnknownOutcome` and are never replayed.
