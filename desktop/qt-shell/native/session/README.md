# Native stream session

Link `aurora_native_session` from `add_subdirectory(native/session)`. The
target links the existing `aurora_native_stream` and `AuroraNativeTailscale`
libraries. `SessionSupervisor` embeds the bundled worker on Windows and
launches the bundled native Moonlight window on macOS.

## Coordinator integration

1. Construct an `Aurora::Session::SessionSupervisor` on the GUI thread
   with the bundled executable and, for embedded Tailnet routes, the
   existing `EmbeddedStatusClient`. Submit a `SessionCandidate` with the
   coordinator's generation, display name, and selected host ID. Pass a
   `DirectRoute` for a direct IP and port or an `EmbeddedRoute` with the
   verified peer ID and address. `prepare()` returns the attempt token.
2. Wait for `prepared(PreparedSession)`. This indicates local runtime
   compatibility and provides the exact executable and endpoint the
   supervisor will use. On an embedded Tailnet route, that endpoint is
   the supervisor-owned loopback lease. Confirm the current account,
   device identity, access, and existing Moonlight pairing for that
   prepared route through the coordinator's existing checks. Those
   checks can use the prepared endpoint while this attempt is active.
3. After the checks pass, call `start({prepared.attempt, prepared.generation}, view)`.
   The coordinator supplies the grant only for the prepared session it
   approved. The supervisor accepts it once. Cancel the attempt if the
   checks fail, the context changes, or the user abandons it. Relay loss
   also stops the attempt. A cancel during allocation uses
   `EmbeddedStatusClient::cancelOpen()` and lets the transport drain the reply.
4. On Windows, pass the top-level stream `QWindow` and logical viewport
   in `SessionView`. Use `setViewport(rect, suppressed)` for layout and
   panel visibility. Bind the existing chrome to `videoReady`, `fullScreen`,
   `connectionMetrics` and `networkPoor`. Route full-screen changes to
   `setFullScreen()` and Cancel and Disconnect to `requestStop()`. The
   worker's full-screen event toggles the same owner and restores its
   previous normal or maximized state afterward.
5. On macOS, omit the owner window. `Phase::Standalone` means the native
   worker started. Its own UI owns video readiness and window controls.
   Pass the existing preference argument provider and macOS session
   snapshot in `SessionView`. Handle `preferenceChangeRequested` in the
   preference layer, which validates values and persists them.

Stop keeps the relay leased until the worker exits. Windows signals the
named stop event, including when the owner receives a close request.
macOS requests termination from the standalone process. The supervisor
reaps an unresponsive worker after the six-second grace period. The
terminal `finished` signal carries the attempt, generation, reason, and
any worker exit code. It follows worker exit and submission of lease
cleanup. The tailnet client acknowledges that cleanup asynchronously.
Coordinator code owns window disposal, access policy, and any retry.

## Windows surface helper

The supervisor uses the existing `StreamSurface` API. The lower-level
integration remains available to callers that manage their own worker:

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
build with the shell's Qt toolchain and run CTest. The supervisor fixture
uses the repository's stream entry gate and fake Moonlight and tailnet
children. It covers the authorization gate, viewport and full-screen
handling, relay lifetime, cancellation during allocation, and graceful
and forced worker stops. The Windows surface fixture still loads the
existing stream chrome in a `QQuickView` and exercises the original
invitation, window validation and events. Both fixtures use local child
processes and pipes.
