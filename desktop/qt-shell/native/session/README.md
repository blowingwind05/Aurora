# Windows native stream surface

Link `aurora_native_session` from `add_subdirectory(native/session)`. The
helper uses Qt Gui and C++17 on Windows. `StreamSurface::available()`
reports availability for the Windows QPA backend. The macOS shell can
query the same API and continue through its existing session path.

## Coordinator integration

1. On the GUI thread, create an `Aurora::Session::StreamSurface` for the
   top-level session `QWindow`. Call `prepare(owner, viewport)` before
   launching the worker. The viewport is in the owner's client coordinates
   and uses Qt logical pixels. An empty invitation indicates that the
   native surface could not be prepared.
2. Bind the actual launcher `QProcess` with `bindWorker(&child)` before
   starting it. Pass the invitation as the launcher's `embeddedInvitation`
   and request connection details. The worker receives the existing
   `AURORA_EMBEDDED_SESSION` value. Keep the existing
   `AURORA_QT_SESSION=1` selection for its optional Qt connecting stage.
3. Feed complete stdout lines from that process to
   `handleWorkerLine(&child, line)`. The existing
   `aurora-connection-stage/1,qt` line selects the connecting child after
   the helper verifies its parent and PID. Continue handling the rest of
   the child's output in the coordinator. Bind `videoReady` to
   `StreamSessionChrome.Connected` and `attachmentFailed` to
   `StreamSessionChrome.Failed`. Exit code 74 before validated video
   readiness reports attachment failure.
4. Call `setViewport(rect, suppressed)` from the chrome's layout and
   overlay bindings. Observe `clientPixelSizeChanged` for the actual
   Win32 client size. The existing worker reads that size and resizes its
   own child. Run the window's fullscreen toggle when the helper emits
   `fullscreenRequested`.
5. Route the chrome's cancel/disconnect actions and the owner's close
   handling through `requestStop()`. Keep the owner and worker alive
   through the existing graceful stop window of six seconds. The
   coordinator owns the force-stop fallback, process lifetime and the
   decision to close or keep the session chrome after `workerFinished`.
   On native handle loss the helper signals stop and revokes the
   invitation. Each subsequent `prepare` creates a new session ID.

## Worker contract

The invitation is the decimal `HWND:PID:32-lowercase-hex-id`. Its HWND is
a real native child of the identified local owner. The helper uses the
existing `Aurora.Embedded.<id>` window property with value `1` and the
`Local\\Aurora.Embedded.<id>` named events. `.ready` and `.stop` are
manual-reset, and `.fullscreen` is auto-reset. The helper rejects existing
event objects instead of reusing them.

The worker already checks the parent PID, child style and window property
before opening the events. The helper pins the bound worker's process
handle and accepts video readiness only if a visible, direct renderer
child belongs to that worker. The parent checks the host HWND, property
and owner throughout the session.

## Same-window gateway and texture evidence

The current Windows Moonlight patch creates the
`AuroraEmbeddedStreamSurface` child HWND and passes it to
`SDL_CreateWindowFrom`. The Qt connection-stage patch attaches a separate
`QQuickView` to the same parent HWND. These hooks expose window handles,
named events and text status. There is no shared graphics resource or
frame transfer in that IPC for Qt Quick to import as a texture.

In the fixture, the existing `StreamSessionChrome.qml` stays in a
separate client-area band above the video. In fullscreen the fixture
reserves a two-pixel pointer strip when the controls are hidden and
expands the band when they appear. Opening an inline QML panel temporarily
hides the native video child so the panel remains visible and interactive.
The existing child HWND stays attached and returns when the panel closes.
A live overlay over the HWND would need a separate native popup or a
shared-frame contract.

## Offline verification

Configure `cmake -S desktop/qt-shell/native/session/tests -B <build-dir>`,
build with the shell's Windows Qt toolchain and run CTest. The test preview
loads the existing stream chrome in a `QQuickView` and launches a Win32
fixture that implements the original invitation and events. It checks the
connecting stage, ready and fullscreen signaling, child geometry,
occlusion and minimization, invalid owner and worker handles, the stop
signal, and cleanup on owner destruction. The fixture uses no Moonlight
runtime or network operations.
