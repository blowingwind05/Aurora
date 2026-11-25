# Native stream control

Add `native/stream/control` after the native stream, server-info, and Tailscale
targets and link `Aurora::StreamControl`. The adapter runs on the event-loop
thread that owns any supplied stream lease.

The caller authorizes one `Control::Operation` and assigns a fresh token. After
correlating the route-check request with its completion, pass the selected
`Discovery::StreamHost`, successful `ServerInfo::CheckResult`, and trusted
bundled runtime path in `Control::Authorization`. For embedded routes, include
the exact loopback endpoint used for that check and the caller's live
`StreamLease`. For privacy, use the active session's executable and route.
The adapter compares the host UUID, literal address, port, and provider and
requires the fresh capability for the requested operation. The bundled
Moonlight worker enforces the saved pairing certificate and paired HTTPS.

`Adapter::start()` accepts one request at a time. A successful submission
emits `finished(token, result)` asynchronously after the worker has exited.
Use a read-only `stillCurrent(token)` predicate for account, route, or session
revocation and call `cancel(token)` to revoke immediately. The adapter holds
its reference to the relay until it reaps the child. The caller continues to
own its original reference. Cancellation can kill a worker that has already
transmitted a request.

Power returns `Accepted` for exit 0 with `accepted`, and `PairingRequired` for
exit 3. Privacy returns `PrivacyReported` with `Off`, `On`, or `Unavailable`
on exit 0 with the corresponding response. Other command responses return
`Unconfirmed`. The operation deadlines are 12 seconds for power and 8 seconds
for privacy from submission to the adapter. Embedded power reserves at most
4 seconds of its deadline for the existing relay capability probe.

## Offline verification

```sh
cmake -S desktop/app/src/stream/control -B build/stream-control -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build/stream-control --config Release
ctest --test-dir build/stream-control -C Release --output-on-failure
```

On Windows, put the Qt kit's `bin` directory on `PATH`. The fake Moonlight
worker only records its arguments and returns synthetic responses. The fake
Tailscale helper serves fixture RPCs over stdio.
