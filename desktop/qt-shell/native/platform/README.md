# Native desktop platform services

Link `aurora_native_platform` from this directory with `add_subdirectory(native/platform)`.
The target uses C++17 and Qt 5.15 or Qt 6 Core. On macOS, enable Objective-C++
in the parent project and use the macOS 13 SDK or newer. The macOS 12 runtime
minimum is compatible with the sleep controller. Startup registration is
available from macOS 13.

## Startup registration

Construct `Aurora::Platform::StartupRegistration(claimInitialization, windowsLauncherPath)`.
On Windows, pass the installed Aurora desktop launcher as an absolute path.
The backend reads and updates the current user's
`Software\Microsoft\Windows\CurrentVersion\Run` value `org.aurora-remote.desktop`.
It writes a quoted Unicode `REG_SZ` command with a 260-character limit.
A different command is reported as `OtherLocation` so the user can turn the
old entry off and enable the current installation. On macOS, pass an empty
Windows launcher path. The backend uses the main bundle with identifier
`org.aurora-remote.desktop` and registers its declared `CFBundleExecutable`
through `SMAppService.mainAppService`. Keep the executable named by the
packaged Info.plist in `Contents/MacOS`, including when that executable
launches the Qt desktop process alongside it.

The preference owner supplies a callback that atomically creates the legacy
`startup-initialized` file if absent. Return `Claimed` after creating it,
`AlreadyClaimed` for an existing marker, or `Failed` for a storage error.
The existing Aurora user-data directory is `%LOCALAPPDATA%\Aurora` on Windows
and `~/Library/Application Support/Aurora` on macOS. Resolve it through the
preference layer and keep the marker even when startup is disabled or an OS
operation fails.

Call `initializeDefault()` once from the primary desktop process. It enables
only when the callback newly claims the marker and the registration is
`Off`. Call `change(enabled)` for explicit changes and re-read the state
with `read()`. `On`, `OtherLocation`, and `NeedsApproval` map to a checked
toggle. `Unavailable` and `ReadFailed` map to a disabled control with their
respective feedback. Use `initializationFailed()` for the legacy default
registration feedback. A successful explicit change clears that flag.

## Host idle sleep

Construct `Aurora::Platform::HostSleepPrevention(savedMode)` on the desktop
event-loop thread. Call `setHostActive(true)` when the managed host is ready,
and `setHostActive(false)` when it stops or the desktop shuts down.
Destruction also releases the assertion and stops monitoring.

The preference owner reads `host-sleep-mode` in the same user-data directory.
Trim the file text before passing it to `parseHostSleepMode()`. Missing,
unreadable, or unrecognized values use `HostSleepMode::System`. Persist
`hostSleepModeToken(mode)` atomically and call `applyMode(mode)` after the
save succeeds. These functions use the same case-sensitive tokens as the
Qt preference protocol.

- `System` follows the operating system's idle policy.
- `OnAc` holds an idle system sleep assertion while the host is active and
  AC power is confirmed. The controller checks immediately and every two seconds.
- `Always` holds the same assertion while the host is active, including on
  battery, subject to the platform's power policy.

Windows consumes automatic-resume notifications on the controller's two-second
timer and renews the assertion if the current mode and power source require it.
Both platforms request idle system sleep prevention and leave display sleep
and manual sleep to the operating system.

## Fixture

Configure with `cmake -S desktop/qt-shell/native/platform/tests -B <build-dir>`,
then build and run CTest. Use the same Qt toolchain as the shell. The fixture
links the native backend and exercises the controllers with in-memory platform
and marker implementations. The Windows command checks use synthetic paths.
