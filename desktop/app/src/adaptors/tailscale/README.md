# Native Tailscale helper

AuroraNativeTailscale links Qt Core and Network and uses the bundled AuroraTailnet helper. The coordinator owns one EmbeddedStatusClient on its event-loop thread. Pass the Windows package root or the macOS bundle's Contents/MacOS directory to bundledExecutable, and pass the existing absolute, per-user Aurora/Tailnet state directory to the constructor.

The typed methods share one child process. Status and mutations use one in-flight slot. Up to eight presence requests can overlap each other and that slot. A method that returns false or zero has queued nothing. Accepted requests complete asynchronously and echo the supplied generation. Presence, stream opens and pairing requests also return the request ID used in their completion signals.

| Request and signal | Successful Go result | Coordinator use |
| --- | --- | --- |
| requestStatus / statusFinished | Status object | Use the snapshot for display and account identity. |
| requestLogin / loginFinished | An object containing only the boolean pending | When true, the helper has queued or started authorization. When false, the node is already running. Poll status for a vetted login URL. |
| requestLogout / logoutFinished | Empty object | Refresh status and resolve signed-out presentation from that result. |
| requestHost / hostFinished | Boolean enabled echo | Enable or disable the existing Sunshine listener. |
| requestPublish / publishFinished | Boolean enabled echo | Publish a kind-2 presence template for the supplied account or disable publication. |
| requestPresence / presenceFinished | Base64 kind-2 packet | Probe a verified peer using a kind-1 request. The reply carries the same nonce. |
| requestOpen / openFinished | Lease ID and relay base port | Open the bundled Sunshine port on a verified IPv4 peer. Use the returned loopback endpoint. |
| requestClose with StreamLease / closeFinished | Empty object | Release a stream lease through its original helper. The completion echoes the open request ID. |
| requestPairHost / pairHostFinished | An object containing only leaseId | Register a pairing listener for the verified local account and caller-owned loopback port. |
| requestPairOpen / pairOpenFinished | An object containing leaseId and localPort | Open the pairing dial listener for the verified peer and use its IPv4 loopback endpoint. |
| requestPairSource / pairSourceFinished | An object containing only the boolean verified | Ask Go to confirm the pending source port against the local account and peer. |
| requestClose with PairingLease / pairCloseFinished | Empty object | Release the auxiliary lease through its original helper. The completion echoes the allocation request ID. |

The child uses the existing `--state-dir` argument and string request IDs. The transport deadline is 12 seconds including startup. Auxiliary closes have a 3-second deadline including time in the close queue. Presence reports timeout after 900 ms and retains sent requests until their replies are drained or the transport deadline expires. Status replies are limited to 4 MiB and other replies to 4 KiB. A hard timeout, malformed frame, broken pipe, or failed lease close invalidates the transport and stops the helper. Mutations are sent once. After an ambiguous failure, refresh status before retrying. Stderr and helper error text stay out of public results. The helper can start or resume its node when launched. Disable requests use an already-running helper.

Only status can expose a login URL. The legacy HTTPS host and path check applies during NeedsLogin. The helper client leaves browser launch and user confirmation to its caller.

The snapshot keeps Self.UserID as a positive signed 64-bit value. Use its decimal string form when forwarding the ID to QML. The decoder treats Self.ID and CurrentTailnet.StableID as opaque identifiers. User display data is selected by that exact owner. Peer rows use the stable ID field in each Peer entry. The projection omits tagged, shared, other-owner, duplicate-ID and unusable-address records. A connected backend can lack a verified account identity or local address. The helper's account and peer checks remain authoritative for pairing and streaming.

Pass the generation from the latest authenticated status completion to enable hosting or publication, probe a peer, or open a stream. Publication checks the owner and both stable IDs against that status. The wire owner is a decimal signed 64-bit JSON number even on Qt 5. Presence and open require the stable peer ID and an address from that same status. Open always targets port 48989, and the Go RPC rechecks the peer at the selected address. The coordinator owns context revalidation and can discard stale completions. It also owns discovery packet creation and payload interpretation.

Keep the returned shared `StreamLease` for as long as the stream uses its loopback address and port. Use and release the lease on the client's event-loop thread. Calling `close()` or dropping the last reference schedules a close. `isValid()` tracks local ownership and helper lifetime. `cancelOpen(id)` cancels a queued open before send or lets the client consume the allocation reply and schedule its close, with a `Cancelled` completion. Transport loss invalidates all owned leases and emits `leasesInvalidated()`. The coordinator should stop using the corresponding endpoints.

Pairing uses the authenticated status generation and a verified IPv4 tailnet address. Host and source requests take the exact local account identity. Dial and source requests take the stable peer ID and that peer's IPv4 address. Ports must be valid nonzero TCP ports. The host-port exclusion matches Go's exclusion of port 48990. Host and dial requests require a 32-byte key and allow up to 16 auxiliary leases alongside the stream leases. Go remains responsible for live account, peer and connection-source verification.

Keep a shared PairingLease on the client's event-loop thread. Pair-host has no returned local port. Pair-open reports a local port for IPv4 loopback. Explicit close or destruction schedules {"leaseId": ...} and emits pairCloseFinished only after a correlated acknowledgement or a transport error. Generation, local account, or peer changes retire affected pairing leases. cancelPairing(id) removes an unsent request or drains a sent reply. Canceled allocations are closed automatically and canceled source requests never expose a positive verification. The coordinator still owns pairing authorization, key creation and handling of the authenticated exchange.

The [desktop preference module](../../preferences/ProviderSelection.h) selects the startup provider using the saved preference and the available runtimes.

## Offline replay

From the repository root with a selected Qt 5.15 or Qt 6 SDK on the toolchain path:

~~~sh
cmake -S desktop/app/src/adaptors/tailscale -B build/native-tailscale -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-tailscale --config Release
ctest --test-dir build/native-tailscale -C Release --output-on-failure
~~~

The fixture keeps its simulated state in memory and exercises the RPCs over child pipes. An embedding CMake target can add this directory and link AuroraNativeTailscale. The tests build only when this directory is configured as the top-level project with BUILD_TESTING enabled.

## Remaining coordinator work

- Wire startup, shutdown, per-user paths, polling and context generations. Read and persist the Windows provider preference and apply the macOS embedded-only policy.
- Wire user confirmation, browser authorization and login/logout readback to the desktop entry point.
- Implement Windows system-client discovery, service and protected-pipe access, system login/logout, and saved-profile list and switching.
- Wire the host, publication, presence, stream lease, and pairing lease methods into discovery, pairing, and the coordinator.
