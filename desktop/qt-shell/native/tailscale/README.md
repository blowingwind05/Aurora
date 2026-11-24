# Native Tailscale helper

AuroraNativeTailscale links Qt Core and Network and uses the bundled AuroraTailnet helper. The coordinator owns one EmbeddedStatusClient on its event-loop thread. Pass the Windows package root or the macOS bundle's Contents/MacOS directory to bundledExecutable, and pass the existing absolute, per-user Aurora/Tailnet state directory to the constructor.

The three request methods share a child process and one in-flight slot. A method that returns false has queued nothing. Each accepted request completes asynchronously through its matching signal and echoes the supplied generation. The coordinator gates requests against its current context and discards stale completions.

| Request and signal | Successful Go result | Coordinator use |
| --- | --- | --- |
| requestStatus / statusFinished | Status object | Use the snapshot for display and account identity. |
| requestLogin / loginFinished | An object containing only the boolean pending | When true, the helper has queued or started authorization. When false, the node is already running. Poll status for a vetted login URL. |
| requestLogout / logoutFinished | Empty object | Refresh status and resolve signed-out presentation from that result. |

The child uses the existing --state-dir argument. Every request has an empty params object and a string request ID. The deadline is 12 seconds including startup. Status replies are limited to 4 MiB and login/logout replies to 4 KiB. A timeout, malformed frame, or broken pipe invalidates the transport. Mutations are sent once. After an ambiguous failure, refresh status before deciding whether to retry. Stderr and helper error text stay out of public results. The helper can start or resume its node when launched.

Only status can expose a login URL. The legacy HTTPS host and path check applies during NeedsLogin. The helper client leaves browser launch and user confirmation to its caller.

The snapshot keeps Self.UserID as a positive signed 64-bit value. Use its decimal string form when forwarding the ID to QML. The decoder treats Self.ID and CurrentTailnet.StableID as opaque identifiers. User display data is selected by that exact owner. Peer rows use the stable ID field in each Peer entry. The projection omits tagged, shared, other-owner, duplicate-ID and unusable-address records. A connected backend can lack a verified account identity or local address. The helper's account and peer checks remain authoritative for pairing and streaming.

selectWindowsProvider implements the legacy startup fallback from saved preference text and the two availability flags. The caller supplies these inputs. System remains the default when both runtimes are absent.

## Offline replay

From the repository root with a selected Qt 5.15 or Qt 6 SDK on the toolchain path:

~~~sh
cmake -S desktop/qt-shell/native/tailscale -B build/native-tailscale -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-tailscale --config Release
ctest --test-dir build/native-tailscale -C Release --output-on-failure
~~~

The fixture executable accepts the three RPCs and keeps its simulated state in memory. An embedding CMake target can add this directory and link AuroraNativeTailscale. The test executable is built only when this directory is configured as the top-level project with BUILD_TESTING enabled.

## Remaining coordinator work

- Wire startup, shutdown, per-user paths, polling and context generations. Read and persist the Windows provider preference and apply the macOS embedded-only policy.
- Wire user confirmation, browser authorization and login/logout readback to the desktop entry point.
- Implement Windows system-client discovery, service and protected-pipe access, system login/logout, and saved-profile list and switching.
- Wire host and publish, presence, pairing (pair-host, pair-open, pair-source) and stream leases (open and close) through the existing helper.
