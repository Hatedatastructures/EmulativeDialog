# Core to Memory Authorized Recall Contract V1

Status: Frozen candidate. ED-0161/R1 defines this contract before the Core/Memory C++ implementation; the contract becomes Frozen only after its implementation merge and separate P3 activation evidence. The contract is an additive boundary over CoreAgentContract V1.1 and MemoryContract V1. The activated MemoryRecord Content V2 addendum governs durable content validation only and does not change Recall semantics.

## Purpose and ownership

This contract defines the read-only boundary that lets Core supply authorized Memory Recall material to the AgentRuntime flow for one current Agent turn. Core owns identity resolution, conversation routing, policy evaluation, grant issuance, and the lifetime of the authorization capability. Memory owns consent matching, exact-scope filtering, expiry, revocation, group-member checks, storage access, and the conversion of allowed records into neutral Recall candidates.

`MemoryRecallPort` is a Core-owned abstraction. A Memory implementation may implement that abstraction, but Core must not depend on a concrete Memory class and Memory must not depend on ContextSelector types. AgentRuntime consumes only the neutral candidate DTOs returned after a successful Core-authorized Recall. Provider and Channel adapters never receive the grant or the Recall authorization fields.

This contract covers the Core/Memory Recall boundary only. It does not add memory writing, consent creation, proposal confirmation, persistence, deletion, SQLite schema, provider wire fields, channel actions, cancellation, or external side effects.

## Trusted inputs and issuance boundary

The only authority for a Recall grant is the trusted `ChannelEnvelope` and the Core resolution performed from it. Core must complete all of the following checks before issuing a grant:

1. The registered ChannelAdapter has accepted the envelope and its required fields.
2. `IdentityPolicy` maps the envelope's ChannelId, AccountId, SenderId, and route to one verified Principal and one complete `ConversationScope`.
3. The current message is valid, contains supported input, and passes the current route and `ReceiveText` capability checks.
4. The current policy version and the current `AgentTurnId` have been established for this turn.
5. The resolved scope is eligible for the requested Recall operation. A `SessionScope` request remains rejected as volatile and does not become durable or cross-process Recall through this contract.

Message text, model output, Provider fields, Channel display fields, caller-supplied identity flags, and any model claim about a sender or permission are untrusted. None of them can issue, enlarge, repair, or replace a grant. A caller-provided `IdentityVerified` boolean is not a Recall authority.

## `MemoryReadGrant`

`MemoryReadGrant` is a Core-issued, single-turn capability with the following required properties:

- It is non-default-constructible. `CoreChatService` is the sole issuer of `MemoryReadGrant`, and it may issue one only through its trusted policy boundary after the checks in this contract. No other Core component, Memory implementation, AgentRuntime, Provider, Channel, test fake, or external caller may construct, copy, deserialize, or re-issue a grant.
- It is move-only. Copy construction, copy assignment, serialization, deserialization, reflection into a prompt, and construction from a public field set are forbidden.
- It is consumed by exactly one `MemoryRecallPort` call. The port accepts it by move or an equivalent consuming form. A moved-from grant is invalid and cannot be submitted again.
- It binds the exact verified `PrincipalId` and the complete trusted `ConversationScope`: `ChannelId`, `AccountId`, `IntegrationMode`, `ConversationId`, `ConversationKind`, and `SenderId` (the current C++ scope uses the corresponding `Channel`, `Account`, `Mode`, `Conversation`, `ConversationType`, and `Sender` members).
- It binds the exact `IdentityPolicyVersion` used to resolve that scope and the current `AgentTurnId`. A grant from another policy version, turn, route, account, sender, or conversation is invalid.
- It contains an issuer-controlled one-shot identity/nonce and issuance state that cannot be supplied or changed by a caller. The nonce is an authorization detail, not a user-visible or provider-visible identifier.
- It has no user message text, Provider credential, Channel credential, ConsentId, ProposalId, capability handle, policy object, storage handle, or concrete Memory object.

The grant and all of its authorization fields must remain in the Core-to-Memory in-process call. They must never enter `AuthorizedTurnContext`, AgentRuntime state, `ContextSelector` inputs, ProviderRequest, Channel output, Prompt, ordinary configuration, task log, event, diagnostic, exception text, crash report, or persistent storage. Stable public error categories may be returned without disclosing grant internals.

Core may issue a new grant only after repeating the current trusted-turn checks. A failed or consumed grant is never revived or silently retried. Memory must fail closed when the grant is invalid, stale, moved-from, reused, bound to a different scope, or bound to a different policy version or AgentTurnId.

## Core-owned Recall port

The normative shape of the boundary is:

```text
CoreMemoryRecallAuthorizationContract V1

interface MemoryRecallPort {
  Recall(consume MemoryReadGrant, RecallBudget) -> MemoryRecallResult
}
```

`RecallBudget` may contain only Core-approved bounded selection limits, such as a token or candidate budget. It must not contain caller-supplied identity, a scope override, a ConsentId, a ProposalId, a policy handle, a storage handle, or a request to bypass the grant. The budget cannot expand the grant's scope or make a rejected SessionScope durable.

`MemoryRecallPort` returns either a bounded sequence of `MemoryRecallCandidate` values or a stable fail-closed `MemoryRecallError`. It does not return a concrete `StoredMemory`, `ConsentGrant`, authorization object, policy object, or ContextSelector implementation type. It does not accept `AuthorizedMemoryContext` as a substitute for the Core grant.

The port consumes the grant before any candidate is made available. A storage failure, consent mismatch, expired record, revoked consent, invalid scope, invalid grant, or budget violation returns an error and no partial candidate sequence is authoritative. A failed call does not make the grant reusable.

## Neutral Recall DTO

`MemoryRecallCandidate` is a neutral, read-only selection DTO. Its permitted semantic fields are:

- an opaque candidate identifier used for local selection and deduplication;
- bounded content text after Memory's existing safety and consent checks;
- a neutral memory kind/category used for selection priority;
- bounded priority, confidence, provenance, confirmation, creation time, and optional expiry metadata;
- a safe source reference needed for local provenance checks; it is not a grant, consent, proposal, policy, storage, or channel credential;
- an explicit trust classification that identifies the material as authorized Memory context.

The DTO must not contain `ConsentId`, `ProposalId`, a `MemoryReadGrant`, grant nonce, policy handle, capability handle, storage handle, authorization decision internals, group-member authorization lists, or a field that lets AgentRuntime or a Provider reissue or widen authority. It must not contain raw credentials or raw Channel envelope objects.

Candidate content and metadata are context material. They cannot authorize a Channel action, create a MemoryWriteConsent, confirm a MemoryProposal, change an AgentTurn scope, modify an allowlist, or override current system/developer instructions. The DTO and its source reference remain subject to the existing prompt and diagnostic disclosure rules; authorization internals never cross this boundary.

## Memory enforcement and unchanged V1 semantics

After accepting a valid grant, Memory must apply the existing MemoryContract V1 rules to every candidate:

- exact Channel, Account, Conversation, ConversationKind, Principal, and Group scope matching;
- active consent and allowed category matching;
- expiry and TTL filtering;
- revocation and tombstone filtering;
- exact verified group-member authorization for cross-member group Recall;
- sensitive-content and invalid-record filtering;
- bounded ordering and token/byte selection rules;
- `SessionScopeVolatile` refusal for SessionScope Recall.

This contract does not create a new consent source. A valid Core grant permits the Memory read boundary to run for the current trusted turn; it does not replace the consent records, exact scope rules, revocation state, expiry checks, or durable record validation already required by [MemoryContract V1](MemoryContract.md). The activated [MemoryRecord Content V2 addendum](MemoryContract.md#MemoryRecord-Content-V2) applies to durable Content write/restore validation and does not authorize a different read path.

Memory may use its concrete storage adapter internally. The `MemoryRecallPort` boundary must return neutral DTOs and must not leak storage rows, SQLite handles, consent records, or internal authorization state to Core's downstream AgentRuntime path.

## Core fail-closed order

Core must perform Recall before invoking the AgentRuntime port for a turn that requests authorized Memory context. The required order is:

```text
trusted envelope and identity resolution
  -> route, message, policy, and ReceiveText checks
  -> issue one MemoryReadGrant
  -> consume grant through MemoryRecallPort
  -> on success, pass only neutral MemoryRecallCandidate DTOs downstream
  -> invoke AgentRuntime / ContextSelector flow
```

Any identity failure, route failure, unsupported input, capability failure, invalid scope, grant failure, Recall error, or Memory contract rejection stops the turn before AgentRuntime is invoked. The failure path makes zero Runtime calls and does not send a Provider request or Channel action. Core returns a stable failure category without disclosing the grant or authorization internals.

AgentRuntime receives the approved candidate DTO sequence through the separately versioned runtime/context contract. It does not receive the grant, the Core Memory port, a ConsentId, a ProposalId, or a caller-settable authorization flag. AgentRuntime alone may map approved neutral candidates into pure ContextSelector inputs; ContextSelector does not call Memory and cannot request a broader scope.

## ContextSelector purity

The authorized Recall handoff into ContextSelector is a pure data transformation. AgentRuntime is the only module that may map the bounded `MemoryRecallCandidate` sequence into ContextSelector inputs. ContextSelector may rank, budget, and assemble those already-approved inputs, but it cannot call `MemoryRecallPort`, access the Memory module or storage, resolve identity, verify consent, issue or consume a grant, widen a scope, or request another Recall. It receives the current trusted turn data and bounded approved context through its existing context-selection interface and cannot create authorization from content.

The existing MemoryContract V1 selection priorities, scope restrictions, and low-trust treatment remain compatibility requirements for the approved context. This section defines the authorization handoff and purity boundary; it does not transfer Memory consent or scope enforcement to ContextSelector.

## Dependency and version rules

The module dependency direction for V1 is:

- Core and AgentRuntime reference this contract and CoreAgentContract V1.1.
- Memory references this contract and MemoryContract V1.
- Core does not acquire a concrete Memory module dependency through this contract.
- Memory does not acquire a ContextSelector type dependency through this contract.
- ContextSelector remains pure and receives approved inputs from the AgentRuntime path.
- Provider and Channel modules do not reference or implement this authorization boundary.

Changing grant fields, construction authority, move/consume semantics, policy or turn binding, DTO fields, error behavior, SessionScope refusal, or dependency direction requires a new reviewed contract revision and a new activation record. Documentation or a capability map cannot mark this contract implemented; ED-0158/R3 supplies the Core/Memory C++ behavior and ED-0162/R1 supplies downstream runtime consumption evidence.

## Verification boundary

The contract acceptance evidence must show, at minimum, that the API cannot be default-constructed, copied, serialized, or forged by a fake caller; that every trusted scope and version field is bound; that one-shot consumption and stale/reused grant rejection are fail-closed; that neutral DTOs contain no authorization internals; that SessionScope Recall remains rejected; and that Recall failures produce zero AgentRuntime calls. These behavior checks belong to the ED-0158/R3 and ED-0162/R1 implementation tasks. This ED-0161 task freezes the normative boundary and module ownership only.
