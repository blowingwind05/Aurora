# Windows system Tailscale

AuroraSystemTailscale is a Windows-only C++17 target. A coordinator can add this directory and link the target alongside AuroraNativeTailscale. macOS continues with the embedded provider.

inspectWindowsInstallation reads ImagePath from HKLM\SYSTEM\CurrentControlSet\Services\Tailscale. It handles REG_SZ and REG_EXPAND_SZ, applies the legacy executable-name and argument-boundary checks, excludes the old Program Files\AuroraTailnet\<version> preview, and checks for the sibling tailscale.exe. The Service Control Manager handle requests status access only. An injected locator supplies the executable and service state for fixtures.

SystemTailscaleClient accepts one request at a time on the coordinator's event-loop thread. Each accepted request completes asynchronously and echoes the caller's generation. The coordinator owns provider and generation checks, action authorization, user confirmation, and browser authorization.

| Request | Fixed CLI command or result |
| --- | --- |
| requestStatus | status --json. Stopped and pending services return their state directly. A missing installation reports unavailable. |
| requestProfiles | switch --list --json for a running service. A recognized unsupported JSON flag triggers one switch --list fallback. |
| requestLogin / requestResume | up with the legacy five-second deadline. Exit zero or a timeout returns ReadStatus so the coordinator can re-read status and use any vetted login URL. |
| requestLogout | logout with the legacy eight-second deadline. |
| requestProfileSwitch | Fresh profile listing, switch -- ID if needed, and positive profile readback. An already selected profile completes without a switch. |

All CLI commands use the lowercase protected pipe in one argv entry. The 8-second read deadline covers both formats of each profile listing. The switch command has its own 12-second deadline. The client accepts profile IDs from the latest returned list for the caller's generation and binds the choice to the same registered installation. Each accepted profile selection requires a fresh list to confirm availability. A successful switch result requires the requested profile to be current in readback. Nonzero exits and timeouts trigger readback without resending the mutation. Failed revalidation preserves the original switch failure. Action results carry the mutation's normal CLI exit code when one was received and report the action and its outcome.

Stdout limits are 4 MiB minus framing space for status, 1 MiB for profiles, and 64 KiB for mutations. Stderr is bounded at 64 KiB. Raw CLI diagnostics stay inside the client.

The status adapter validates the CLI JSON object and passes the original bytes inside an envelope to the shared Tailscale decoder. This preserves exact int64 owner IDs. The shared decoder keeps a connected state when older clients omit the stable tailnet ID and withholds the account identity and peer list in that case.

## Offline replay

From the repository root with Qt 5.15 or Qt 6 and a Windows C++ toolchain:

~~~powershell
cmake -S desktop/app/src/adaptors/tailscale/system -B build/system-tailscale -DCMAKE_BUILD_TYPE=Release
cmake --build build/system-tailscale --config Release
ctest --test-dir build/system-tailscale -C Release --output-on-failure
~~~

The suite uses synthetic registry values, Program Files paths, service states and CLI output. The subprocess fixture checks the protected-pipe argument, the exact command arguments and profile readback order. The fixture records simulated changes in its own temporary command trace.

## Outgoing system pairing relay

Use `SystemPairingNc` on the coordinator's Qt event-loop thread. It accepts
one typed pairing target at a time and returns a borrowed sequential
`QIODevice` for binary stdout and stdin.

1. Call `setAuthorization(generation, account)` with the current verified
   system-provider account. Pass no account on sign-out, provider change, or
   transition and advance the generation. Older updates are ignored.
2. Call `start({generation, account, peerId, address})` with the peer's stable
   ID and the typed address from the verified GameStream route. Zero means
   the request was rejected or the adapter is busy.
3. On `ready(id, device)` pass the device as both reader and writer to the
   pairing client's system stdio path. Keep the adapter alive. The adapter
   owns the device. On pairing completion or cancellation call `cancel(id)`
   or close the device. `finished(id, result)` reports transport completion
   and the CLI's normal exit code when available. Pairing success comes
   from the pairing client.

Before launching `nc`, the adapter runs a fresh `status --json` using the
registered running installation and the same fixed lowercase protected
pipe. The existing system decoder preserves the exact integer owner ID
and filters ineligible peers. The adapter requires the pinned owner,
local node and tailnet to match, the requested address to resolve to one
eligible peer with the expected ID, and the same installation to still
be running. IPv4-mapped requests are normalized to IPv4. It then runs the
equivalent of `tailscale --socket=<protected pipe> nc <literal address> 43826`.
The adapter does not take arbitrary arguments. A status check and a later
CLI launch are separate daemon operations. The supervisor must invalidate
authorization when it learns of an account or provider transition.

The status phase has an eight-second cap inside the 38-second overall
deadline. The child starts hidden with a private environment containing
only the Windows directory variables and a path to the registered CLI
directory and System32. Stdin, stdout and stderr on the relay are each
bounded at 64 KiB. Stdout and stdin carry only binary protocol data and
stderr is drained privately. Natural EOF preserves unread stdout until the
next accepted request. A timeout, cancellation, or authorization change
closes the channel and kills the child before reporting completion.

The `system-pair-nc-*` fixtures run a fake CLI with synthetic installation
state, status JSON, and binary stdio. They exercise IPv4 and IPv6 arguments,
identity and installation changes, the environment, cancellation, and
timeouts and byte limits. They do not invoke the installed Tailscale CLI or
touch a real service or account.
