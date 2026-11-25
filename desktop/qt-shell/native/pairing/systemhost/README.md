# Windows system pairing host

## Sunshine coordinator

Add this Windows-only directory with CMake and link
`Aurora::SystemPairingCoordinator`. Construct `SystemPairingCoordinator` with
the existing `LifecycleController` and a callback returning
`std::optional<SystemPairingContext>`. Keep both objects on the same Qt
event-loop thread.

- Supply an approved system enrollment scope, verified account and local
  address, stable account epoch, and the authenticated Sunshine settings
  session. Use the lifecycle's admin port and certificate hash in the scope.
  Keep the epoch stable across ordinary status refreshes.
- Call `start()` explicitly. Read `phase()` and `error()` on `stateChanged()`.
  The coordinator probes the pinned Sunshine pending endpoint before
  opening the transport.
- Call `contextChanged()` synchronously on enrollment, provider, account
  epoch, or local address changes. Return `std::nullopt` during transitions
  or sign-out. Call `stop()` when hosting ends. The coordinator also
  observes the Sunshine lifecycle and retires on session loss or pin changes.

The coordinator sends the legacy offer to one authenticated peer at a
time. It polls pinned Sunshine for exactly one row whose marker and
normalized source address match, whose request ID is unique in the
response, and whose exact row remains unchanged on confirmation. The
system path accepts IPv4, mapped IPv4, and IPv6 peer addresses. It then
revalidates the transport peer immediately before submitting that row's ID
and the offer PIN to the lifecycle worker. The cross-thread guard checks
the original connection deadline and is revoked on disconnect, unexpected
input, relay exit, or context retirement. A submitted approval occupies
the session's approval slot until the worker settles it.

## Transport API

For direct transport access, link `Aurora::SystemPairingHost` and include
`SystemPairingHost.h`. Keep the adapter, its consumer, and the signal handlers
on the same Qt event-loop thread.

1. Once the managed Sunshine host and enrollment are ready, call
   `setAuthorization(generation, {identity, localAddress})` with the verified
   system-provider snapshot, then call `start(generation)`. Keep the returned
   host ID. A zero return means the adapter did not accept the start.
   `listening(host, generation)` reports foreground Serve readiness, and
   `stopped(host, generation, reason)` identifies an ended host.
2. Use `authenticated(host, generation, connection, peer, channel)` to begin
   the pairing exchange. The adapter has consumed the PROXYv2 and
   `AURORA/PAIR/1` frames. Write the offer to the borrowed `QTcpSocket`.
   The adapter owns the socket and monitors it for disconnect or extra
   client data. Match the pending request in managed Sunshine using
   `peer.address` and the offer marker.
3. Call `revalidate(host, connection)` after finding the matching request.
   Submit approval only on `revalidated(host, generation, connection)` after
   checking that the coordinator's enrollment, generation, and pending
   request are still current. A failed check closes the connection.
   `connectionClosed` invalidates the borrowed channel and cancels the
   coordinator's matching or approval work. Call `closeConnection` when the
   exchange ends and `stop(host)` when hosting ends. Pass `std::nullopt` and
   advance the generation on sign-out, provider change, or transition.
   Revoke authorization if the managed session or enrollment loses validity.
   For approval on the Sunshine worker, use a per-attempt guard invalidated
   by connection closure or a coordinator context change.

Startup pins the registered running installation and verifies the exact
owner, self node, stable tailnet, and local address through fresh status.
`serve status --json` checks for TCP and Funnel conflicts, including
foreground sessions. Startup uses an exclusive ephemeral IPv4 loopback
listener. The hidden foreground Serve process uses the fixed tailnet TCP
port 43826 and PROXYv2. The Windows kill-on-close job binds the relay to the
adapter's process. The listener closes on relay exit or authorization
revocation. Serve preflight and launch are separate daemon operations.

All CLI commands use the lowercase protected system pipe with bounded
output, closed stdin, and a private environment. For each accepted socket,
the adapter obtains the server PID of that protected pipe and requires an
established TCP row with that PID and the exact client-to-listener loopback
tuple. Pipe or TCP-table failures reject the socket. A valid PROXYv2 frame
and matching request are required within three seconds. The adapter checks
the same daemon PID and refreshes status before handoff and before approval.
The shared status decoder supplies eligible same-owner peers, and the
source must map to one stable peer ID. The system path uses the asserted
tailnet address as the Sunshine source. Four connections can be in flight
with one offer at a time and a 31-second lifetime per connection.

## Offline verification

From the repository root with a Windows C++ toolchain and Qt on the build
and runtime paths, with compatible TLS libraries:

~~~powershell
cmake -S desktop/qt-shell/native/pairing/systemhost -B build/system-pairing-host -DCMAKE_BUILD_TYPE=Release
cmake --build build/system-pairing-host --config Release
ctest --test-dir build/system-pairing-host -C Release --output-on-failure
~~~

The fixtures use a fake CLI and synthetic protected-pipe ownership results.
They exercise the real Windows owner-PID TCP-table lookup with test loopback
sockets.

The coordinator fixtures use the Sunshine lifecycle worker and a local
pinned TLS fixture to verify the exact POST body and cancellation before
HTTP bytes are sent. The expiry case allows the real 31-second transport
deadline to elapse with the desktop event loop paused.
