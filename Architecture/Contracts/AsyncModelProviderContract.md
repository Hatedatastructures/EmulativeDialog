# Async Model Provider Contract V1

Status: Proposed. This contract becomes frozen only when the independently reviewed ED-0149/R2 product PR is merged. It adds awaitable operations alongside ModelProviderContract V1.2 and does not modify or replace the synchronous ModelProviderPort.

## Purpose and boundary

AsyncModelProviderPort is the asynchronous provider seam used by AgentRuntime. It reuses the canonical profile, request, event, terminal, error, capability, and CredentialLease types from ModelProviderContract V1.2. Provider implementations remain protocol adapters; they do not own identity, authorization, channel effects, memory, or tool execution.

This contract only defines the asynchronous API and its compile-contract fake. It adds no network transport, endpoint selection, production adapter, retry loop, QQ integration, or product turn behavior.

## Operations

Each operation is a C++20 Boost.Asio awaitable:

- GetCapabilitiesAsync returns ProviderResult<ProviderCapabilities>.
- ValidateProfileAsync returns ProviderResult<std::monostate>.
- StreamTurnAsync returns exactly one CanonicalTerminalEvent and receives the existing EventSink for nonterminal events.
- HealthCheckAsync returns ProviderResult<ProviderHealthStatus>.

The signatures use the existing V1 ProviderProfileId, ModelProviderProfile, CredentialLease, CanonicalModelRequest, CanonicalNonTerminalEvent, CanonicalTerminalEvent, ProviderResult, and std::stop_token. The synchronous methods remain available and unchanged.

## Borrowed lifetimes

The caller keeps every referenced ProfileId, ModelProviderProfile, CredentialLease, CanonicalModelRequest, and EventSink alive and unmodified until the returned awaitable completes. The provider must not retain pointers, references, string views, callbacks, the lease, or request data after completion. Implementations must not detach work whose lifetime outlives the awaitable.

A CredentialLease remains valid only during that awaited call. Implementations must not copy its authentication secret into persistent state, logs, diagnostics, events, or provider-independent data.

## Executor and event delivery

The awaitable's associated executor is the event-delivery executor. A provider obtains it from its coroutine context and invokes EventSink only on that executor. Delivery is serial: callbacks for one request never overlap or reenter one another. The provider may suspend between callbacks, but it must resume delivery on the same associated executor.

Each nonterminal event carries the current RequestId and AgentTurnId and has a SequenceNumber starting at 1 that increases by exactly one for each emitted event. No event callback may occur after StreamTurnAsync completes. The sink accepts only nonterminal variants; the single terminal is returned from the awaitable and must not also be sent through the sink.

## Cancellation and terminal result

StreamTurnAsync observes the supplied std::stop_token. If cancellation is known to occur before the remote request could have been sent, it returns ResponseCancelled. If the request may have reached the remote service and the provider cannot determine a definitive terminal outcome, it returns ResponseOutcomeUnknown. If a definitive provider terminal was received, that terminal is returned even if cancellation is requested afterward. An unknown outcome must never be converted to success or blindly replayed.

Expected provider failures use the existing canonical terminal or ProviderError types. A caller must await completion before destroying borrowed inputs or assuming that event delivery has stopped.

## Compile-contract fixture

The ED-0149 compile-contract test supplies an in-memory awaitable fake. It exercises all four operations, nonterminal event order and sequence, terminal return, cancellation before dispatch, unknown outcome after simulated dispatch, and an awaitable consumer. It performs no socket, HTTP, DNS, external-account, or channel operation.