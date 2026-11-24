# Native Moonlight launcher

Link `aurora_native_stream` from `add_subdirectory(native/stream)`. The
target uses C++17 and Qt 5.15 or Qt 6 Core and Network. Its route type uses
`QHostAddress` for numeric formatting and validation.

## Build the command

1. Call `Aurora::Stream::findMoonlight(QCoreApplication::applicationDirPath())`.
   An empty result means the runtime is missing. The search retains the current
   Windows `Components/Moonlight` layout, legacy integrated and separate
   directories, and the installed Moonlight fallback. The native Windows shell
   can pass its own directory from inside `Components/Moonlight`. On macOS,
   the integrated runtime is the `AuroraStream` executable in `Contents/MacOS`,
   with its existing sibling `Frameworks`, `PlugIns`, and `Resources` layout.
   A standalone Moonlight app remains a fallback. The working directory is
   the selected executable's directory for every command.
2. Fill a `Request` with an `Operation` and the endpoint selected by the
   coordinator. The coordinator owns access approval, host identity and
   certificate trust, pairing exchanges, and relay leases. For embedded
   Tailnet, pass the leased `127.0.0.1` endpoint with its base port, the
   selected host ID and `viaEmbeddedTailnet = true`. Retain the lease until
   the child finishes. Direct routes use numeric IPv4 or unscoped IPv6.
3. Call `makeCommand(program, request)` and handle the returned
   `CommandError` if there is no command. For playback, its optional
   `preferenceArguments` callback receives the same viewer and bundled
   default selectors as the .NET launcher. Supply the full argument list
   from the validated preference snapshot. Without a callback, the builder
   uses the legacy flags for an empty set of overrides. For a named macOS
   session, supply the session-control snapshot in `sessionPreferences`.
   For a Windows embedded session, supply the coordinator's existing
   `HWND:PID:session-id` invitation and request connection details.
4. For capability checks, use `makeProbeCommand(program, probe)` and
   `probeSucceeded(probe, exitCode, stdout)`. The three version probes are
   standalone options. The background-pair probe sends the existing
   background help command. Embedded-window support is a Windows probe.

The builder passes fields as individual arguments. The bundled runtime gets
`--aurora-runtime` before commands, including background pairing, list,
wallpaper, power, and privacy. Standalone Moonlight gets its upstream
playback, pairing, and list arguments. For wallpaper, an empty
`wallpaperRevision` represents the existing `-` argument. The builder
replaces any inherited session invitation, connection-details, macOS
session-control and relay fields with the data for this launch.

## Own the child

Create one `MoonlightProcess` per launch on the coordinator's Qt event-loop
thread. Connect the inherited `QProcess` signals before calling
`launch(*result.command, &error)`. A successful submission can still emit
`errorOccurred(QProcess::FailedToStart)`. For capture commands, consume
`readyReadStandardOutput` and read the final bytes at `finished`.
Stdout carries the existing protocol and exit codes pass through QProcess.
Forwarded commands retain the parent's standard channels.

Set command deadlines in the coordinator and call `cancel()` on timeout
or cancellation. A Windows embedded session can first signal its existing
stop event and use `cancel()` after the grace period. Keep the process
owner alive until completion. Destruction cleans up a running child. On
Windows 10 and later, a kill-on-close job binds the child at creation and
also handles abrupt parent exit.

## Verify with fixtures

Configure with `cmake -S desktop/qt-shell/native/stream/tests -B <build-dir>`,
build with the shell's Qt toolchain, and run CTest. The tests stage synthetic
install layouts and execute only the fake Moonlight child. The fake runs the
repository's `AuroraStreamEntry.h` gate before reporting its arguments,
working directory and selected environment fields. On Windows, a disposable
fixture supervisor also checks child cleanup after abrupt parent termination.
