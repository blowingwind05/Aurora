# Native Aurora presence

Compile `PresencePacket.cpp` and `PresenceProbe.cpp` as C++17 and link Qt Core
and Qt Network from Qt 5.15 or Qt 6. The API is in `Aurora::Presence`. The
embedded adapter accepts the existing `aurora::tailscale::Status` snapshot.
It can build and test independently of the desktop coordinator.

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

Create one `DatagramProbe::unicast()` per address-family target set or one
`DatagramProbe::lan()` per LAN source interface. Send its request to the
existing transport destinations on port `48988`. Feed the full reply and its
actual sender endpoint to `addReply()`. Keep at least 193 bytes in a receive
buffer when detecting oversized datagrams, or pass the original complete
datagram.

The window is at most 900 ms. Unicast admits only the supplied target addresses
and caps distinct results at the target count. LAN uses the supplied
same-subnet policy and caps distinct results at 512. The latest accepted
announcement for an address replaces its earlier one. Stop when
`remainingMilliseconds()` reaches zero. `cancel()` closes the window and keeps
the collected replies. Discard stale account results before updating the
catalog.

Use `answerDatagram()` with the transport's current authorized-source predicate
before replying. Pass the real sender to both APIs. The predicates see
IPv4-mapped IPv6 senders normalized to IPv4, as in the legacy receive loop.
The caller owns the socket, interface or group configuration, current trusted
peer set, and LAN subnet policy. Return a response only to the original
request sender.

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

The fixtures use a .NET-generated golden reply, synthetic sender addresses and
in-memory helper results to exercise framing, names, filtering, probe bounds
and account changes. They open no sockets and write no Aurora state.
