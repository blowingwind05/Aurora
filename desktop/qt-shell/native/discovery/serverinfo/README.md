# Native Sunshine server-info

Link `aurora_native_serverinfo` with C++17 and Qt 5.15 or Qt 6. The API is in
`Aurora::Discovery::ServerInfo`. It uses the existing native discovery
`NetworkSnapshot`, `PresentDevice`, `Endpoint`, and `StreamHost` types.

## Authenticated probes

1. On the client's event-loop thread, call
   `updateDiscovery(generation, network, lan.devices, tailnet.devices, embedded)`
   with the current verified presence snapshots. It admits hosting devices
   only, and a Tailnet address must still belong to one peer in the authenticated
   account. On an account or provider change, update the snapshots and
   generation before issuing more work. Outstanding results from the old
   scope are discarded.
2. Pass a list of `Candidate` values to `probeBatch()`. Use an address from
   verified presence for the candidate. For an embedded candidate, supply the
   literal loopback address and port from the existing authenticated stream
   relay and hold the lease through completion. Keep relay creation and
   lifetime in the coordinator. An eight-exchange limit applies to the batch.
   The default batch budget is four seconds, with a five-second maximum to
   accommodate the legacy LAN budget. The default port is the bundled
   Sunshine port, 48989.
3. Use `batchFinished(request, result)` to feed the appropriate LAN or Tailnet
   route snapshot. Results retain the candidate's original endpoint and
   embedded flag, including when the request traveled through a loopback
   relay. Failed probes are omitted. A batch deadline or explicit cancellation
   can return partial results. An authentication-scope change discards them.

The legacy `_nvstream._tcp.local.` service is an address hint for the presence
step. The server-info exchange uses the authorized numeric candidate and the
bundled port. It does not use ports, addresses, or identities advertised through
mDNS or port and address fields returned in the XML to choose its target.

## Revalidation

Call `revalidate(expectedHost, relay, requirement)` for a selected LAN or
Tailnet route. The reply must match the selected Sunshine ID using the existing
ordinal case-insensitive comparison. A different ID yields
`Status::IdentityChanged` without a current host. The power requirement also
checks the fresh power flag. The caller owns LAN-first selection and applying
any fallback policy.

A successful check can report `encoderProbePending`. Before launching that
route, call `waitForEncoder(expectedHost, relay)` and wait for
`checkFinished()`. It probes 300 ms apart after each response, retries
unavailable replies, stops on a different ID, and returns once the same host
reports a status other than `probing`. Its overall deadline is 30 seconds.
Keep the same embedded relay lease for the wait. One batch and one route
operation can be active at a time. `cancel(request)` ends the specified
operation.

## Wire behavior and limits

Each exchange writes one direct HTTP/1.x `GET /serverinfo` request with a
numeric Host header and no query. It uses a Qt TCP socket with the proxy
disabled. No redirect, cookie, or HTTP authentication negotiation is
performed. The request uses connection close and identity encoding. The
response must have a successful HTTP status. Content-length, chunked, and
connection-close framing are supported with bounded headers and framing
bytes. Each exchange has a hard 1,100 ms limit and an XML body limit of
32,768 bytes.

The XML reader rejects DTDs and extra document content. It requires an
unqualified `root` with `status_code="200"`, direct `PairStatus` and
`appversion` elements, and nonblank `hostname` and `uniqueid` values.
Sunshine IDs are limited to 128 UTF-16 code units after trimming. The first
matching direct element supplies each value. The Aurora encoder, power,
and session-privacy fields retain their legacy spelling and values.

## Offline verification

With an installed Qt SDK, run from the repository root. On Windows, put the
SDK's `bin` directory on `PATH` for the fixture executables.

```sh
cmake -S desktop/qt-shell/native/discovery/serverinfo -B build/serverinfo -DCMAKE_PREFIX_PATH="/path/to/Qt/SDK"
cmake --build build/serverinfo --config Release
cmake -E chdir build/serverinfo ctest -C Release --output-on-failure
```

The fixtures use synthetic XML, synthetic account and presence records, and
TCP servers bound to ephemeral IPv4 or IPv6 loopback ports. The IPv6 fixture
skips when the test host lacks IPv6 loopback.
