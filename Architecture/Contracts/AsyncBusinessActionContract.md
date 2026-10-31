# Async Business Action Contract V1

Status: Frozen candidate. This contract defines the Core-owned asynchronous business-action registry and dispatcher used by Async Agent Runtime Contract V2.

## Registration

A business action is available to the model only when Core explicitly registers it. Each asynchronous definition contains a PascalCase action name, a canonical provider `ToolDefinition` whose name equals the registered action name, an object JSON Schema, required top-level argument fields, required capability names, a channel/conversation scope requirement, and an asynchronous handler. Existing synchronous registry handlers remain available to synchronous callers and are not invoked by the asynchronous path.

The supported action schema subset is a JSON object schema with declared top-level properties of primitive JSON types, a `required` list, and an explicit `additionalProperties` policy. Unsupported schema keywords or malformed schemas fail registration. The same schema is sent to the provider and used by Core to validate the completed arguments; required fields alone do not substitute for type or additional-property validation.

An async handler receives a Core-created `BusinessActionInvocation` containing the registered action name, provider `ToolCallId`, parsed canonical arguments, trusted Principal, trusted ConversationScope, and active AgentTurnId. It also receives the current `std::stop_token` and returns `boost::asio::awaitable<BusinessActionExecutionResult>`. Handlers may return only a stable sanitized result code with status Succeeded, Failed, or UnknownOutcome; raw channel payloads, credentials, user message text, and provider wire objects are not receipt data.

The additive registry fields are a canonical `Providers::ToolDefinition ModelTool` and `using AsyncHandler = std::function<boost::asio::awaitable<BusinessActionExecutionResult>(const BusinessActionInvocation&, std::stop_token)>`; the existing synchronous `Handler` field remains available and unchanged for V1. The Core V2 dispatcher implements `GetToolDefinitions(const AuthorizedTurnContext&) const` and `DispatchAsync(const AuthorizedTurnContext&, const Providers::RequestId&, std::vector<Providers::FunctionCall>, std::stop_token) -> awaitable<std::vector<AsyncBusinessActionReceipt>>`.

## Eligibility and authorization

`IdentityBinding` is the trusted source for its `GrantedCapabilities`; `IdentityPolicy::Resolve` carries those grants into `ResolvedIdentity` and the Core-created `AuthorizedTurnContext`. The grant set is not read from ChannelEnvelope text, model output, or provider arguments. The model does not receive the grant names.

The Core dispatcher builds model-visible definitions from the registry using the exact authorized context. It offers an action only when its registered scope matches and every required capability is present in the Core-resolved grant set. The provider cannot add or modify definitions, capabilities, Principal, scope, or turn identity.

After a correlated ToolHandoff terminal, Core validates each proposal against the registered action name and JSON object schema, verifies required fields, request and turn identity, scope and capability grants, and rejects duplicate or previously dispatched ToolCallIds. Core must not call a handler for any rejected proposal. A ToolCallId is marked before dispatch so reentrant delivery cannot execute it twice.

Each dispatched handler produces one sanitized `AsyncBusinessActionReceipt` associated with the original ToolCallId and action name. Definite success, failure, and rejection may be returned to the model as a tool-role result. If cancellation or an exception occurs after side effects may have begun without a definite result, the receipt is UnknownOutcome; the turn stops and that receipt is never presented as success. Unknown outcomes are not automatically replayed.

## Lifetime and scope

Registry, trusted context, provider lease, dispatcher, and handler dependencies remain alive until their awaited operations complete. The dispatcher does not detach work. Action execution is limited to explicitly registered chat business actions; no generic Shell, host command, arbitrary file, general network, browser, or MCP capability is introduced.