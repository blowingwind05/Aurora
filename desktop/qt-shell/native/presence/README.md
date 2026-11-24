# Native Aurora presence

The module uses C++17 with Qt Core and Qt Network from Qt 5.15 or Qt 6.
The API is in `Aurora::Presence` and the transport accepts the existing
`aurora::tailscale::Status` snapshot. It builds and tests independently of
the desktop coordinator.

## Wire format

UDP port `48988`, 192 bytes per request or response, as in
`desktop/AuroraPresence.cs` and the existing tsnet helper.

| Byte range | Value |
| --- | --- |
| 0–16 | ASCII `AURORA/PRESENCE/1` |
| 17 | Request `1` or reply `2` |
| 18–33 | 16 random request bytes, echoed by the responder |
| 34–49 | Presence GUID in .NET byte order |
| 50 | Platform `0` unknown, `1` Windows, `2` macOS |
| 51 | Hosting flag, true only for `1` |
| 52–191 | UTF-8 machine name with zero padding and no split character |

Use `createRequest()` for a system-random nonce and `encodeRequest(nonce)` for
fixtures. `encodeReply(request, local)` echoes the nonce and rewrites the
announcement fields. `announcementTemplate(local)` produces the zero-nonce
kind-2 packet the embedded helper expects. `decodeReply()` checks both frame
headers, the nonce, and the remote GUID. Empty or local GUIDs are rejected.
An empty or control-bearing name falls back to the supplied source address.
Use `platformName()` and the response's address and announcement fields to
populate a discovery `PresentDevice`.

## Datagram transport

Link the `aurora_native_presence` CMake target or compile `PresencePacket.cpp`,
`PresenceProbe.cpp`, and `UdpPresenceTransport.cpp` with Qt AUTOMOC enabled
for `UdpPresenceTransport.h`. Create the transport on a Qt event-loop thread
with a stable local presence GUID. The defaults bind IPv4 and IPv6 separately.
`UdpBindings` can restrict a family to a specific local address or disable it
with a null address.

- `updateAccess(links, systemStatus, generation)` replaces the access policy.
  Pass eligible IPv4 local addresses and masks supplied by the coordinator.
  Pass the authenticated system-provider snapshot for that generation. Pass
  null for other providers or when signed out. Clear the LAN list when LAN
  discovery is disabled. Advance the generation on provider or account
  transitions. Stale generations are ignored.
- `startResponding()` reports which families bound UDP port 48988 exclusively.
  The responder accepts the current same-subnet sources and eligible system
  Tailnet peers. Ambiguous addresses claimed by different peers are excluded.
  It sends a valid reply to the original sender endpoint. Update the name,
  platform and hosting flag with `updateAnnouncement()`. `stopResponding()`
  releases the listener sockets.
- `probeLan()` broadcasts once per allowed link using a socket bound to its
  local address. `probePeers(targets)` sends only to eligible supplied
  addresses and groups them by family. `scanFinished(requestId, result)`
  asynchronously returns observations for the ID from either method. An
  unchanged policy allows concurrent requests. An access-policy change
  invalidates outstanding results with `ScanStatus::AccessChanged`.
- Each socket reuses one request nonce. A probe completes at its distinct
  address bound or after the 900 ms window. The LAN limit is 512 replies per
  link. A newer valid reply replaces the earlier one for that address on the
  same socket. If several links see the address, the first link in the input
  order wins. `cancelScan(id)` closes the probe and can return partial results.
  `ScanStatus::SocketError` reports a bind or send failure and can also carry
  partial results.

For callers with their own socket, `DatagramProbe` and `answerDatagram()` are
also usable directly. Their source predicates receive IPv4-mapped IPv6
addresses normalized to IPv4. Pass the real sender endpoint and preserve the
complete datagram or retain at least 193 bytes to distinguish oversized packets.
## Embedded transport

Call `EmbeddedProbe::start(status, generation, target, localId)` with the active
embedded status. Send `requestParameters()` to the existing `presence` RPC.
Pass its correlated result and the **current** status and generation to
`decodeResult()`. The target must still map to the same unique stable peer
under the same account, local node, tailnet, local address and generation.
The helper continues to validate the connected UDP endpoint and peer
membership. The caller owns mode transitions and the legacy limit of eight
simultaneous embedded probes.

`embeddedPublishParameters()` returns serialized JSON params for the existing
`publish` RPC, including the numeric owner ID, local node ID, and base64
announcement. Insert those bytes as the params object in the RPC envelope
without reserializing them through Qt 5, which can round large numeric IDs.
Use the existing unpublish operation when the account or mode becomes inactive.

## Offline fixtures

From the repository root, point CMake at the installed Qt SDK. On Windows, add
that SDK's `bin` directory to `PATH` before running the fixtures.

```sh
cmake -S desktop/qt-shell/native/presence -B build/presence -DCMAKE_PREFIX_PATH="/path/to/Qt/SDK"
cmake --build build/presence --config Release
cmake -E chdir build/presence ctest -C Release --output-on-failure
```

The packet fixtures use a .NET-generated golden reply and in-memory results.
The UDP fixtures bind only IPv4 or IPv6 loopback, exercise the responder and
actual probe sockets there, and check directed broadcast and subnet rules with
synthetic addresses. They send no LAN broadcasts and write no Aurora state.
