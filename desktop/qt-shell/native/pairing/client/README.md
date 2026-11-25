# Native automatic pairing client

Link `aurora_native_pairing_client` and include `PairingClient.h`. The module
uses C++17, Qt Core and Qt Network, and the existing native discovery,
PairingWire and Moonlight targets. Keep the coordinator and its devices on
the same Qt event-loop thread.

## Supervisor API

1. Call `setAuthorization(epoch, true)` for the active account and provider.
   Advance the epoch on account or provider changes and disable it on
   sign-out or transition. Stale epochs are ignored. A change cancels active
   work and suppresses stale success.
2. Fill in a `Request` with that epoch, the verified Tailnet peer's stable ID,
   the revalidated remote `StreamHost`, the bundled Moonlight program, and the
   applicable GameStream route described below. The supervisor supplies a
   QObject lifetime guard for the route. Cancel on explicit revocation or
   destroy the guard.
3. Call `startLoopback(network, request, socket, bridgePort, key)` with the
   already-connected TCP socket from the separately authorized pair-open
   lease. It must connect from loopback to the exact IPv4 loopback port and
   use the lease's 32-byte HMAC key. Use `127.0.0.1` for the GameStream
   lease with the verified UUID and `viaEmbeddedTailnet = true`. The caller
   owns the socket object and the pair-open RPC lease. The coordinator closes the accepted control
   connection after the pairing child exits or on failure. Clear the
   supervisor's key copy and release the pair-open lease in its owner.
4. Alternatively call `startStdio(network, request, reader, writer)` for an
   authorized system pairing relay. The devices are open, asynchronous,
   sequential stdio channels. One duplex device can supply both ends.
   This variant skips the HMAC challenge. Set `leasedRoute` to the verified
   direct Tailnet GameStream address, port and host UUID, with
   `viaEmbeddedTailnet = false`. The supervisor supplies its lifetime guard
   and owns the stdio devices and relay process.
   It closes the devices and stops the relay after completion. The
   coordinator never starts a system Tailscale process.

A nonzero `StartResult::request` completes once through
`finished(request, result)`. A zero ID reports a synchronous start error and
leaves transport ownership with the caller. `cancel(request)` stops the
active exchange and kills its child if necessary. Wait for `finished` before
releasing the GameStream lease during ordinary completion.

## Exchange and trust

The coordinator checks the expected peer against the authenticated discovery
snapshot before sending anything. The embedded control path reads the
32-byte challenge and uses `PairingWire::dialProof()`. It sends
`AURORA/PAIR/1\n` and reads the existing one-byte rejection or 37-byte offer.
The challenge has a 3-second limit and the offer a 10-second limit. A
38-second overall deadline bounds the exchange, child processes, and
post-pair confirmation. Cleanup reaps a cancelled Moonlight child before
publishing its result.

After a valid offer, the coordinator checks the bundled worker's background
pairing capability for direct stdio or relay capability for embedded mode.
It launches `BackgroundPair` with the supplied marker and PIN.
The control channel stays open while the child pairs. Only a normal exit of
zero permits trust readback. The coordinator reads the exact host UUID's
persisted `srvcert` from Moonlight's own per-user settings, honoring
`portable.dat` and Moonlight's backup-array precedence. It then runs
Moonlight's `list` on the same route to confirm the pinned connection. `Status::Paired` returns the SHA-256 digest only after
both checks succeed. The settings reader writes no pins.

Embedded pairing requires the UUID-bound IPv4 loopback lease. System stdio
uses the authenticated direct Tailnet route, including for `list`, matching
the legacy system flow.

## Offline fixtures

Use an installed Qt SDK and its TLS backend for certificate parsing. On
Windows, add the Qt DLL directory and the matching OpenSSL runtime directory
to `PATH` when required by that Qt build.

```sh
cmake -S desktop/qt-shell/native/pairing/client -B build/pairing-client -DCMAKE_PREFIX_PATH="/path/to/Qt/SDK"
cmake --build build/pairing-client --config Release
cmake -E chdir build/pairing-client ctest -C Release --output-on-failure
```

The fixtures use ephemeral IPv4 loopback sockets or in-memory stdio, a fake
Moonlight executable, a public test certificate, and a disposable portable
settings directory. They exercise partial framing, trust readback, runtime
failure, deadlines, cancellation, and account and lease invalidation.
The direct-stdio fixture verifies direct `pair` and `list` arguments in the
fake worker. The embedded fixture performs the control HMAC over loopback.
