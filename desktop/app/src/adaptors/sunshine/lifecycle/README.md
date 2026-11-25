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

## Bundled settings projection

The settings API is available on the same controller. `State::settingsSession` is
the QML session string for the current authenticated launch, including launches
initiated by exit code `42`. It clears when leaving `Ready`. The worker retains
the original pinned `BundledAdmin` for that launch.

- `bool readSettings(SettingsReadCompletion)` fetches a fresh view. If accepted, its
  callback receives a `SettingsReadResult` on the controller thread. On success,
  publish `snapshot->config` as `host.sunshine.config` and
  `snapshot->version.session` as `host.sunshine.session`. The JSON shape is the
  existing `{options, values, privacy}` projection. Keep the full
  `SettingsVersion` in C++ alongside the published projection.
- `bool saveSettings(SettingsEdit, SettingsSaveCompletion)` accepts the version of
  the view the edit came from, the QML request `id`, and the `changes` JSON object.
  At command admission, match `sunshineSave.session` to the published session and
  capture the published version. The revision stays in C++. The existing QML
  command envelope stays `{session, id, changes}`.
- `SettingsSaveResult::receiptForQml()` returns `{id, saved, errors}`. If the callback
  still belongs to the published session, publish the receipt and any returned
  snapshot together, and adopt the snapshot's new version. Use
  `SettingsSaveResult::submitted` to match callbacks to their original view.
  Use `refreshRequired` to trigger a fresh read when POST may have reached the
  host and readback did not confirm its state.
- Both methods return `false` without a callback when the host has no ready
  session, another settings operation is in flight, or the controller is closing.
  `settingsBusy()` reports an accepted operation until completion or cancellation.
  The caller owns publication of the settings page's busy flag. An accepted edit
  with an old session or revision receives a `Stale` receipt. Stop and restart
  cancel pending callbacks and revoke the old token.

Saves use the managed catalog gate, merge accepted fields over a fresh GET,
and POST one payload. An externally changed submitted field rejects the edit
before POST. Unedited upstream fields are carried forward. A writer outside
the controller can still race the endpoint between GET and POST. The receipt
acknowledges preferences and catalog options only when their respective groups
match authenticated readback. Unsupported or invalid fields have per-field errors.
Bundled config transport currently projects `privacy: null`; the display privacy
controls require the separate privacy endpoint.

## Forwarding a supervisor-approved pairing

On the controller thread, capture `state().settingsSession` for the authenticated
launch and pass it to both calls.

- `readPendingPairings(session, PendingCompletion)` performs the read-only lookup.
  Correlate its untrusted metadata in the coordinator.
- `approvePairing(session, SupervisorPairingApproval::authorized(id, pin, name),
  ApprovalCompletion)` dispatches the exact approval after the coordinator's
  supervisor authorization. Reuse the session captured for that approval.

These calls share `adminBusy()` with settings and reuse the authenticated client.
A stale session returns `StaleSession` without sending HTTP. Stop or restart
invalidates the session, cancels pending completion, and fences approvals that
have yet to transmit. An approval already sent to the server can have taken
effect when cancellation is reported. The caller owns enrollment decisions and
any retry or reconciliation.

## Offline verification

From a checkout with the catalog, admin, host, and lifecycle slices applied:

```sh
cmake -S desktop/app/src/adaptors/sunshine/lifecycle -B build/sunshine-lifecycle -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build/sunshine-lifecycle --config Release
ctest --test-dir build/sunshine-lifecycle -C Release --output-on-failure
```

On Windows, include the Qt kit's `bin` directory on `PATH` for CTest. The suite
uses temporary profiles, injected process and admin adapters, ephemeral loopback
listeners, and the admin slice's synthetic TLS identities. Tests cover port
conflicts, pinned authentication, retry and deadlines, worker responsiveness,
stale callbacks, restart suppression, and ownership after failed cleanup.
