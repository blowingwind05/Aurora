# Windows system Tailscale reads

AuroraSystemTailscale is a Windows-only C++17 target. A coordinator can add this directory and link the target alongside AuroraNativeTailscale. macOS continues with the embedded provider.

inspectWindowsInstallation reads ImagePath from HKLM\SYSTEM\CurrentControlSet\Services\Tailscale. It handles REG_SZ and REG_EXPAND_SZ, applies the legacy executable-name and argument-boundary checks, excludes the old Program Files\AuroraTailnet\<version> preview, and checks for the sibling tailscale.exe. The Service Control Manager handle requests status access only. An injected locator supplies the executable and service state for fixtures.

SystemTailscaleClient accepts one request at a time on the coordinator's event-loop thread. requestStatus and requestProfiles report through their respective signals with the caller's generation. A missing installation reports unavailable for status and an empty profile list. Stopped and pending services return their status directly. Profile reads only run for a running service.

The client passes the lowercase protected pipe as one argv entry and uses fixed status --json and switch --list --json commands. A CLI that explicitly rejects the JSON flag gets one switch --list fallback. Both attempts share the eight-second deadline. Stdout limits are 4 MiB minus framing space for status and 1 MiB for profiles. Stderr is bounded at 64 KiB and is used only to recognize the legacy flag error. Public results carry error codes. Raw CLI diagnostics stay inside the client.

The status adapter validates the CLI JSON object and passes the original bytes inside an envelope to the shared Tailscale decoder. This preserves exact int64 owner IDs. The shared decoder keeps a connected state when older clients omit the stable tailnet ID and withholds the account identity and peer list in that case.

## Offline replay

From the repository root with Qt 5.15 or Qt 6 and a Windows C++ toolchain:

~~~powershell
cmake -S desktop/qt-shell/native/tailscale/system -B build/system-tailscale -DCMAKE_BUILD_TYPE=Release
cmake --build build/system-tailscale --config Release
ctest --test-dir build/system-tailscale -C Release --output-on-failure
~~~

The suite uses synthetic registry values, Program Files paths, service states and CLI output. The subprocess fixture verifies the exact protected-pipe argument and the read commands. Coordinator integration owns provider selection and the Windows system login, logout and profile-switch actions.
