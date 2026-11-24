# Bundled Sunshine lifecycle

`Aurora::SunshineLifecycle` dispatches the existing bundled host and pinned admin
modules from a Qt application. The patch is based on Aurora `816ca54` and builds
with the bundled host patch applied. That base already contains the catalog and
admin targets.

## Controller interface

Add this directory with `add_subdirectory`, link `Aurora::SunshineLifecycle`, and
include `LifecycleController.h`. Construct a `LifecycleController` with the
`HostLayout` returned by `HostLayout::installed()`. The owner calls `start()`
when the user has enabled the bundled host. Construction starts only the worker
event loop.

- Use `start()`, `restart()`, `stop()`, and `shutdown()` on the controller's
  Qt event-loop thread. Each returns whether it queued a new request. A start
  from `Failed` retries. Restart is available from `Ready`. A new start during
  stop waits for cleanup before launching.
- Read `state()` on `stateChanged()`. It gives the request number, phase, error
  message, and `cleanupPending`. Phases are `Stopped`, `Starting`, `Ready`,
  `Restarting`, `Stopping`, and `Failed`. A failed stop retains the host and its
  profile lease so a later stop or start can retry cleanup.
- Call `shutdown()` and wait for `shutdownFinished()` before closing the
  application's event loop. Destruction also queues worker cleanup. The owner
  can use the state signal for app state and sleep assertions.

Every fresh launch checks the managed port family before staging a profile.
Windows inspects TCP listeners and UDP bindings for IPv4 and IPv6. macOS probes
the IPv4 wildcard binds used by the legacy launcher. The check is a snapshot
of port occupancy. The companion opens its own sockets afterward.

The worker owns profile preparation, subprocess launch and waits, port checks,
admin requests, and cleanup. It uses `BundledAdmin::readConfig` with the current
launch credential. `Ready` requires pinned, authenticated config readback and
a still-running child. Startup allows up to 60 seconds after the launch
handshake. A missing certificate or a transport failure can be retried.
An HTTP authorization or config response failure ends startup and cleans up.

Stop or shutdown invalidates older readiness results immediately and cancels
pending admin work on the worker. A synchronous subprocess call finishes on
the worker before queued cleanup can take over. Exit code `42` from a ready
launch causes a fresh launch and authentication. A second automatic request
within 30 seconds fails the lifecycle so the owner can retry explicitly.

## Offline verification

From a checkout with the catalog, admin, host, and lifecycle slices applied:

```sh
cmake -S desktop/qt-shell/native/sunshine/lifecycle -B build/sunshine-lifecycle -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build/sunshine-lifecycle --config Release
ctest --test-dir build/sunshine-lifecycle -C Release --output-on-failure
```

On Windows, include the Qt kit's `bin` directory on `PATH` for CTest. The suite
uses temporary profiles, injected process and admin adapters, ephemeral loopback
listeners, and the admin slice's synthetic TLS identities. Tests cover port
conflicts, pinned authentication, retry and deadlines, worker responsiveness,
stale callbacks, restart suppression, and ownership after failed cleanup.
