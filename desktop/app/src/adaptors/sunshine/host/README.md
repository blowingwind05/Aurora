# Bundled Sunshine host

`Aurora::SunshineHost` implements profile preparation and the subprocess handshake
for the bundled companion. It uses the `Aurora::SunshineConfig` target already
present at Aurora `c12a259`.

## Interface

Add this directory with `add_subdirectory`, link `Aurora::SunshineHost`, and include
`BundledHost.h`. The API is in `Aurora::Sunshine`.

- `HostLayout::installed()` resolves the layout from the application executable
  directory and the user's local application-data directory. Windows launches
  `Components/Sunshine/AuroraHost.exe` from its payload directory. macOS 14.2+
  launches `Contents/Helpers/AuroraHost.app/Contents/MacOS/AuroraHost` with the
  helper's `Resources` directory as its working directory. Both use the user's
  `Aurora/Sunshine/Bundled/config` profile. `forDirectories` takes explicit roots
  for packaging tools and offline fixtures.
- `prepareBundledProfile(layout, error)` creates the profile if needed, takes the
  native `running.lock` lease, and publishes a new `sunshine.conf` only when it
  is missing. Existing config files are preserved byte for byte. Account and
  pairing-state paths must be separate files. The check rejects symbolic links and
  hard-link aliases using metadata. The preparer creates no certificates, accounts,
  or pairing state.
- `BundledHost host(layout)` uses the native backend. Keep it on a worker thread
  with a Qt event loop and serialize calls. `start(error)` stages the profile and
  passes all of the catalog's managed values as arguments on each launch. These
  include the fixed port, certificate and key locations, `account.json`, and
  `sunshine_state.json`. Keep profile staging leases and active host ownership
  separate by releasing any standalone `PreparedProfile` before calling `start`.
- The Windows backend creates a dedicated manual-reset stop event and attaches
  the child to a kill-on-close job before writing stdin. The macOS backend
  establishes a dedicated process group. The launch frame is exactly 64 ASCII
  hex bytes with no terminator. Windows closes stdin after the frame and
  signals its event to stop. macOS holds stdin open and closes it to stop.
- `launchPassword()` returns the in-memory frame after a successful handshake
  and while the child is running. When the companion has created its public
  certificate, the lifecycle owner can hand off directly to the separate admin
  module with `BundledAdmin::open(host.layout().profileDirectory(),
  host.launchPassword(), error)`. Dispose the admin client when stopping the host or observing its exit.
- `stop(error)` revokes password access, waits up to 11 seconds after requesting
  graceful shutdown, and allows three seconds to reap after a forced stop. The
  lock stays held while a child remains unreaped. `exitCode()` observes normal
  exit and exposes the companion's restart request code `42` for the owner.

The owner still needs to wire the port-occupancy preflight, pinned HTTPS readiness
and authenticated config read, restart scheduling, sleep and permission services,
and the root app integration. A successful `start` confirms the pipe handshake.
Service readiness comes from the separate admin check.

## Offline verification

From a checkout at `c12a259` with the host patch applied, configure with a Qt 5.15
or Qt 6 development kit.

```sh
cmake -S desktop/app/src/adaptors/sunshine/host -B build/sunshine-host -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build/sunshine-host --config Release
ctest --test-dir build/sunshine-host -C Release --output-on-failure
```

On Windows, add the Qt kit's `bin` directory to `PATH` for CTest. The test stages
temporary profiles and injects a fake process for each platform. It exercises the
signalling and stdin order, existing config preservation, credential-file aliases,
lease contention, failure cleanup, exit code, and credential revocation.
