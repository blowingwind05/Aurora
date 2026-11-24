# Native Tailscale status

AuroraNativeTailscale is the read-only embedded provider for the standalone Qt coordinator. It links Qt Core and Network and uses the packaged AuroraTailnet helper.

The coordinator owns one EmbeddedStatusClient on its event-loop thread. Pass the Windows package root or the macOS bundle's Contents/MacOS directory to bundledExecutable, and pass the existing absolute, per-user Aurora/Tailnet state directory to the constructor. isAvailable checks the explicit bundled executable. The client accepts one status request at a time and echoes the caller's generation in statusFinished so a coordinator can discard a result after changing context.

The child uses the existing --state-dir argument and writes newline-delimited replies to stdout. The client sends only status with an empty params object and a string request ID. It keeps one child across successful reads, applies a 12-second deadline including startup, and limits each reply line to 4 MiB. A timeout or malformed frame discards the owned child before it can be reused. Helper stderr and RPC error text are excluded from the public result. The helper can start or resume its own node when launched. The fixtures use a separate inert executable and a temporary path.

The snapshot keeps Self.UserID as a positive signed 64-bit value. Use its decimal string form when forwarding the ID to QML. The decoder treats Self.ID and CurrentTailnet.StableID as opaque identifiers. User display data is selected by that exact owner. Peer rows are keyed by Peer entries' stable ID field rather than the rotatable keys of the Peer map. The projection omits tagged, shared, other-owner, duplicate-ID and unusable-address records. A connected backend can lack a verified account identity or local address. The peer list is status data. The helper's account and peer checks remain authoritative for pairing and streaming.

## Offline replay

From the repository root with a selected Qt 5.15 or Qt 6 SDK on the toolchain path:

~~~sh
cmake -S desktop/qt-shell/native/tailscale -B build/native-tailscale -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-tailscale --config Release
ctest --test-dir build/native-tailscale -C Release --output-on-failure
~~~

An embedding CMake target can add this directory and link AuroraNativeTailscale. The test executable is built only when this directory is configured as the top-level project with BUILD_TESTING enabled.

## Remaining coordinator work

- Wire startup, shutdown, status polling, provider selection and context generations into the standalone coordinator. Resolve the existing state directory from Windows Local AppData or macOS Application Support. The current Windows and macOS entry points still use the existing host.
- Port embedded login and logout, browser authorization, confirmation and retry. The status decoder only exposes the legacy-approved login URL during NeedsLogin.
- Port Windows system Tailscale detection, service states, the protected system pipe and CLI reads, system login and logout, and saved profile selection and switching. The macOS coordinator must retain its embedded-only policy and own the bundled helper lifecycle.
- Wire host and publish, presence, pairing (pair-host, pair-open, pair-source) and stream leases (open and close) through the existing helper. Account, host, and lease mutations need coordinator ownership of the same helper process and their own lifecycle and generation gates.
