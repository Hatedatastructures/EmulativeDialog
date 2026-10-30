# Async Agent Runtime Contract V1

Status: Proposed. This contract becomes frozen only when the independently reviewed ED-0149/R2 product PR is merged. It is additive to CoreAgentContract V1 and the existing synchronous AgentRuntimePort.

## Purpose and ownership

AsyncAgentRuntimePort is the coroutine form of the Core-to-AgentRuntime boundary. It consumes the same Core-authorized turn context and canonical V1 provider types, and returns the existing AgentRunResult through a C++20 Boost.Asio awaitable. It does not replace AgentRuntimePort::RunTurn or change its behavior.

Core remains the sole owner of trusted ChannelEnvelope validation, principal and conversation mapping, deduplication, policy, memory scope, action authorization, operation records, and outbound channel effects. The async runtime accepts an already-authorized context and cannot promote model-supplied identity or tool data into authorization.

## Call lifetime and execution

The caller keeps AuthorizedTurnContext, AsyncModelProviderPort, ModelProviderProfile, and CredentialLease alive and unmodified until RunTurnAsync completes. The coroutine must not retain their references after completion or detach work from the awaited operation.

RunTurnAsync is cancellation-aware through the supplied std::stop_token and awaits the provider operation directly. Wrapping synchronous RunTurn or StreamTurn in a blocking wait is not an implementation of this contract. Provider nonterminal events are consumed serially on the provider awaitable's associated executor; the runtime must not publish callbacks after its awaitable completes.

## Result mapping

The runtime returns one AgentRunResult:

- ResponseCompleted maps to ResponseReady with the original CompletionKind and accumulated canonical answer text.
- ResponseCancelled maps to Cancelled only when no unresolved remote or external side effect remains.
- ResponseOutcomeUnknown maps to UnknownOutcome and must not trigger a blind retry.
- ResponseFailed maps through the existing MapProviderFailureCode mapping.
- ResponseOutputLimited maps to Failed(OutputLimitReached).
- A provider tool handoff remains a proposal for Core policy; the runtime cannot execute it or send it to a channel.

This contract does not add a second Core authorization path, tool executor, provider retry policy, or channel integration. The later product behavior task implements the coroutine turn loop and Core/Channel composition against this boundary.

## Failure and compatibility

Expected provider outcomes are represented using existing canonical terminal and AgentRunResult types. The synchronous V1 interfaces and existing callers remain source-compatible. New callers opt in by depending on AsyncAgentRuntimePort and co_awaiting RunTurnAsync.