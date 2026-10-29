# Core Endpoint Grant Contract V1

Status: This ED-0120/R5 disclosure-policy revision remains a freeze candidate until its own exact fixed-head independent approval and merge. PR #46 at fixed Base `b037f2b8dfce76f504213b4f6e46d1faf15424ed` / Head `3a9f00aff097c4a23a655eaea1093c35096c67b0` (merged as `5364581858c8299884e87e00730748b86fb43346` after internal Worker04/Worker06 approvals) is prior ED-0147/R9 authority-implementation/baseline evidence, not approval of this R5 text; this does not change ED-0147 task state or the P3 gate.

## Scope and ownership

`Core::EndpointGrantAuthority` is the Core-owned authority that turns one
trusted, immutable provider profile snapshot into short-lived references to
the profile's single canonical HTTPS destination. Core is the only owner of
profile authorization, grant issuance, revocation, and the grant registry.
Provider adapters and network transport receive a grant reference; neither may
choose or expand its destination.

This contract implements the authority and its offline behavior only. It does
not define or claim an existing production Provider/WinHTTP composition call
site, provider configuration source, real credential issuance, or live network
request.

## Snapshot and endpoint rules

Construction copies only each profile's `ProviderProfileId` and validated
`HttpsDestination`. It does not retain model names, credential references,
provider options, or the caller's profile objects. The complete snapshot is
rejected if any ID is empty or duplicated, or if any endpoint is invalid; no
partially accepted authority is exposed.

Accepted endpoints use HTTPS and ASCII URI syntax. User information, fragments,
control/space characters, raw non-ASCII, backslashes, malformed percent
escapes, malformed or ambiguous IP literals, invalid DNS labels, invalid
ports, and path dot segments are rejected. DNS names are lowercased and a
single terminal dot is removed. IPv4/IPv6 literals are normalized; IPv6 is
stored without URI brackets in `HttpsDestination::Host`. An absent port means
443. An absent path means `/`; a query-only endpoint becomes `/?query`. The
path and query remain bound as one absolute path string; percent escapes are
validated, escaped unreserved bytes are decoded, and escaped path separators
are rejected to avoid destination ambiguity.

`IssueForProfile` takes only a `ProviderProfileId`. There is no overload that
accepts an arbitrary URL or `HttpsDestination`. The returned destination is
copied from the validated snapshot. Updating a profile requires revoking this
authority and constructing a new one from a new trusted snapshot.

## Grant lifecycle

Windows issuance uses `BCryptGenRandom` with
`BCRYPT_USE_SYSTEM_PREFERRED_RNG` for exactly 32 bytes. The bearer reference is
encoded as 64 lowercase hexadecimal characters. There is no fallback source.
The registry stores the profile identity, exact canonical host/port/path,
issuance time, and expiry time. Expiry is five minutes after issuance using
`std::chrono::steady_clock`; a grant is expired when `now >= expiry`.

An entropy failure creates no registry entry. A token collision returns a
failure and leaves the existing entry unchanged. Unknown profile IDs cannot
issue a grant. `RevokeProfile` marks the profile revoked, invalidates all of
its grants, and prevents further issuance from that authority. Revoked and
expired grant IDs remain tombstoned so an old ID is never rebound. A separate
authority starts with an independent registry and does not recognize old IDs.

`EndpointGrantResolver::Validate` returns `Valid` only if the ID exists, its
profile is not revoked, its five-minute lifetime has not elapsed, and the
requested `HttpsDestination` exactly equals the stored binding. Unknown and
revoked grants return `Missing`; expired grants return `Expired`; any host,
port, or path mismatch returns `DestinationMismatch`. Exceptions fail closed
as `Missing`. The guarded `HttpTransportPort::Stream` maps every non-`Valid`
result to `EndpointNotGranted` before calling `DoStream` or either response
sink.

## Concurrency and disclosure

All profile revocation, grant insertion, and validation use one synchronized
registry. Concurrent issuance never overwrites an existing ID. Grant IDs and
profile endpoints are not written by this module to logs, events, telemetry,
diagnostics, persistence, command output, or user-visible output. The authority
has no logging, event, telemetry, or persistence dependency.

`EndpointGrantId` is a bearer capability. It may be held only in the local
in-memory authorization/resolver flow; Core, Transport, adapters, and callers
must never disclose it in logs, structured events, telemetry, diagnostics,
persistent records, command output, user-visible output, exception text, or
crash reports, and must never send it to the remote endpoint. `TransportRequestId`
is a separate local correlation identifier for transport diagnostics and events;
it must not be placed in remote headers, query parameters, or body content, and
must never substitute for the grant during authorization. Local correlation uses
`TransportRequestId` rather than `EndpointGrantId`.

ADR-0003 and HttpTransportContract V1 use this same no-disclosure rule. The
former diagnostic allowance for recording grant IDs is superseded by the
ED-0120/R5 correction; there is no transport-layer exception. This documentation
revision does not alter the V1 API or wire behavior.

## Verification boundary

The contract fixture is offline. It exercises snapshot validation,
canonicalization, grant issuance and unknown IDs, exact destination binding,
expiry, revocation, entropy failure, collision handling, concurrent access,
and the guarded transport call order. Tests do not use network access or real
credentials. This contract is V1; changing destination fields, status mapping,
token format, lifetime, or revocation semantics requires a reviewed contract
change.
