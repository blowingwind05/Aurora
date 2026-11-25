# Embedded Sunshine pairing host

Add `native/pairing/host` after the pairing, Tailscale, and Sunshine lifecycle
targets, and link `Aurora::EmbeddedPairingHost`. The target uses the repository's
typed pairing RPCs and bundled Sunshine lifecycle API.

## Parent interface

Construct `EmbeddedPairingHost(tailnet, lifecycle, currentContext)` on the
event-loop thread shared by `EmbeddedStatusClient` and `LifecycleController`.
The parent owns enrollment, user intent, and the verified account. Its
`currentContext` callback supplies an `EmbeddedHostContext` with

- `EnrollmentResult::Approved` and the existing embedded `ApprovalScope` for
  the current bundled Sunshine admin port, certificate hash, owner, and local node
- The verified tsnet `AccountIdentity`, local IPv4 address, and current helper
  RPC generation
- A stable parent account epoch and the authenticated Sunshine launch session
  from `LifecycleController::State::settingsSession`

Call `start()` explicitly for an approved context. Read `phase()` and
`error()` on `stateChanged()`. Call `contextChanged()` synchronously on
provider, enrollment, account epoch, or local address changes. Return an
empty context during logout or provider transition. The host monitors the
lifecycle directly and closes its listener and lease on a change in authority.
Same-account status refreshes advance the RPC generation, including for
`requestPairSource`, while keeping the listener and its account epoch.
The host checks the enrollment against the ready lifecycle's session, admin
port, and certificate hash. It passes the per-attempt thread-safe guard to
`LifecycleController::approvePairing` for the check before HTTP transmission.

## Protocol

The host checks pinned pending lookup before registering its ephemeral IPv4
loopback listener. It accepts at most four in-flight bridge connections, each
with a fresh challenge. The bridge proof and exact request frame must
validate before a fresh status lookup identifies one same-account peer.
One pairing at a time receives an offer. Name and source must match exactly
one pending row with a valid source port. The host rechecks the peer, re-reads
the selected row over pinned Sunshine HTTPS, and requests tsnet's live source
proof for the exact peer and port. Only then does it construct the typed
Sunshine approval. Disconnection, unexpected socket data, expiry, or
revocation cancels the approval guard. An approval already transmitted can
take effect before cancellation is observed.

## Offline verification

From the repository root with the typed API changes applied:

```sh
cmake -S desktop/app/src/pairing/host -B build/pair-host -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build/pair-host --config Release
ctest --test-dir build/pair-host -C Release --output-on-failure
```

On Windows, put the Qt kit's `bin` directory and compatible TLS libraries on
`PATH`. Tests use a synthetic JSON-lines helper, ephemeral loopback sockets,
and the lifecycle worker with a local TLS fixture. They cover exact peer and
pending-row matching, lease survival during status refresh, serialization,
and revocation during TLS before approval bytes are sent.
