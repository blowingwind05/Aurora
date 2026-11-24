# Native desktop preferences

Compile `DesktopPreferences.cpp` as C++17 and link Qt Core (Qt 5.15 or Qt 6).
Include `DesktopPreferences.h` and use the `Aurora::Preferences` namespace.
The two stores accept an absolute Aurora directory. Their default constructor
uses `defaultDirectory()`. Pass a temporary directory explicitly in tests.

## Shared files

On Windows the directory is `%LOCALAPPDATA%/Aurora`. On macOS it is
`~/Library/Application Support/Aurora`, which is where .NET 9 resolves both
application-data folders. Qt Core resolves the base directory without using the
Qt application's name or organization.
[Qt path reference](https://doc.qt.io/qt-6/qstandardpaths.html)
[.NET 9 macOS path implementation](https://github.com/dotnet/runtime/blob/v9.0.0/src/libraries/System.Private.CoreLib/src/System/Environment.GetFolderPathCore.Unix.cs#L88-L94)

| File | API | Legacy format and fresh value |
| --- | --- | --- |
| `exit-on-close` | `closeToTray()`, `setCloseToTray(bool)` | File presence means exit on close. Fresh profiles prefer the tray when a tray is available. |
| `host-sleep-mode` | `sleepMode()`, `setSleepMode(SleepMode)` | Text with the case-sensitive values `System`, `OnAc`, or `Always`. Missing or invalid text gives `System`. |
| `allow-managed-host` | `managedHostAllowed()`, `setManagedHostAllowed(bool)` | File presence means allowed. Fresh profiles start disabled. |
| `startup-initialized` | `startupInitialized()`, `claimStartupInitialization()` | File presence records that the startup default was claimed. It also preserves an existing user's opt-out. |
| `stream-preferences.json` | `read()`, `set(option, value)`, `reset()` on `StreamPreferences` | JSON object with string-valued overrides, capped at 16 KiB. Fresh profiles follow the host resolution at 60 FPS with a 10,000 kbps estimate. |

The startup integration should check platform support before claiming initialization.
Only `StartupClaim::Claimed` allows the caller to apply the first-run default.
Apply it only when the operating system reports that there is no registration.
For an explicit startup change, proceed after either `Claimed` or
`AlreadyInitialized`. Read the actual enabled state from the platform
integration. The caller also owns tray availability, sleep assertions, and
host lifecycle.

`StreamPreferences::read()` returns one snapshot. Use `snapshot.value("bitrate")`
for the effective estimate or explicit value, and `snapshot.overrides` when only
explicit, validated values are needed. Each write returns its persistence result.
A stream `set()` rejects invalid values and preserves other JSON entries.
Unreadable, malformed, and oversized files give effective defaults. An explicit
`reset()` can replace an invalid file with an empty JSON object. A caller that
wants a session-only change after a failed save can keep that value in memory.

## Coexistence

Migration uses the existing files directly. Reads leave files untouched and
recheck disk on every call. Marker updates touch one file, startup claims use
exclusive creation, and text and JSON saves use atomic replacement. Stream writes
lock out other Qt writers while reading and updating the latest copy.

The installed C# app keeps some preferences in memory. Restart it to refresh
those values. Its stream store can replace the full JSON object from a stale
snapshot, so use one app at a time to edit stream preferences when both versions
are running.

## Offline fixtures

From the repository root, point `CMAKE_PREFIX_PATH` at a Qt 5.15 or Qt 6 SDK.
On Windows, put that SDK's `bin` directory on `PATH` so the fixture can load Qt Core.

```sh
cmake -S desktop/qt-shell/native/preferences -B build/preferences -DCMAKE_PREFIX_PATH="/path/to/Qt/SDK"
cmake --build build/preferences --config Release
cmake -E chdir build/preferences ctest -C Release --output-on-failure
```

The fixtures create and remove temporary profiles in the build directory. They
check defaults without writes, legacy marker/text/JSON contents, round trips,
startup claims, changes after construction, and blocked or invalid writes. They
use Qt Core only and require no installed Aurora instance, startup registration,
account, or running host.
