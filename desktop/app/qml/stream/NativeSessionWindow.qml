// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import "../common" as Aurora

Window {
    id: window

    property var session
    property string deviceName
    property bool finished: false
    readonly property bool switchingPicture: !!session && session.videoSettingsPending === true
    property bool nativeWindowsFrame: false
    readonly property int videoTop: chrome.contentTop
    readonly property bool windowsChrome: Aurora.AuroraTheme.windows

    width: 1040
    height: 640
    minimumWidth: 420
    minimumHeight: 280
    title: deviceName
    flags: windowsChrome && !nativeWindowsFrame ? Qt.Window | Qt.FramelessWindowHint : Qt.Window
    color: "#151D2A"

    function toggleMaximized() {
        if (visibility === Window.Maximized)
            showNormal()
        else if (visibility !== Window.FullScreen)
            showMaximized()
    }

    function picturePreferences() {
        const current = session ? session.videoSettings : null
        if (!current || !current.resolution)
            return {}
        const editable = chrome.connected && !switchingPicture && !window.finished
        return {
            resolution: { value: current.resolution, writable: editable, options: [
                {value: "host", label: qsTr("跟随被控端")},
                {value: "1280x720", label: "720p"},
                {value: "1920x1080", label: "1080p"},
                {value: "2560x1440", label: "1440p"},
                {value: "3840x2160", label: "2160p"}
            ] },
            frameRate: {value: Number(current.fps), writable: editable, options: [
                {value: 30, label: "30 FPS"}, {value: 60, label: "60 FPS"}
            ]},
            bitrate: {value: Number(current.bitrate), writable: editable, minimum: 500, maximum: 150000, step: 500},
            codec: {value: current["video-codec"], writable: editable, options: [
                {value: "auto", label: qsTr("自动")}, {value: "H.264", label: "H.264"},
                {value: "HEVC", label: "HEVC"}, {value: "AV1", label: "AV1"}
            ]},
            hdr: {value: current.hdr === "on", writable: editable}
        }
    }

    function openPrivacyAppearance() {
        if (finished || !session)
            return
        chrome.closePanel(false)
        artwork.openEditor()
    }

    function requestPictureSetting(option, value) {
        if (!window.finished && session && !switchingPicture)
            session.requestVideoSetting(option, String(value))
    }

    function updateViewport() {
        if (!finished && visible && session)
            session.setViewport(Qt.rect(0, videoTop, width, Math.max(0, height - videoTop)), false)
    }

    onWidthChanged: updateViewport()
    onHeightChanged: updateViewport()
    onVisibleChanged: updateViewport()
    onVideoTopChanged: updateViewport()
    onClosing: function(event) {
        if (!finished && session) {
            event.accepted = false
            session.requestStop()
        }
    }

    ConnectionStage {
        y: window.videoTop
        width: parent.width
        height: Math.max(0, parent.height - y)
        visible: !chrome.connected
        reduceMotion: chrome.reduceMotion
        heading: window.switchingPicture ? qsTr("正在切换画质…") : qsTr("正在连接…")
    }

    Window {
        id: artwork

        transientParent: window
        modality: Qt.WindowModal
        flags: Qt.Dialog | Qt.FramelessWindowHint
        color: "transparent"
        title: qsTr("设置隐私屏保")
        width: Math.min(686, window.width - 24)
        height: Math.min(520, window.height - 24)
        x: window.x + (window.width - width) / 2
        y: window.y + (window.height - height) / 2

        property int pendingSave: 0
        property var submittedStyle: null
        property bool ready: false
        property string reason
        readonly property var defaultStyle: ({id: "default", kind: "default"})

        function openEditor() {
            ready = false
            reason = qsTr("读取被控设备的已保存样式…")
            editor.beginEdit(window.deviceName, defaultStyle, defaultStyle)
            show()
            if (!window.session.requestPrivacyAppearance("read"))
                reason = qsTr("当前无法读取被控设备的屏保设置。")
            editor.focusEditor()
        }

        onClosing: {
            pendingSave = 0
            submittedStyle = null
            editor.endEdit()
            if (!window.finished && chrome.connected)
                chrome.focusControls()
        }

        Connections {
            target: window.session
            function onPrivacyAppearanceChanged() {
                if (!artwork.visible)
                    return
                const reply = window.session.privacyAppearance
                if (reply.state === "ready") {
                    artwork.ready = true
                    artwork.reason = ""
                    const confirmed = reply.image ? {id: "remote", kind: "custom",
                        previewUrl: reply.image, name: qsTr("已保存样式")} : artwork.defaultStyle
                    if (artwork.pendingSave && editor.saveState === PrivacyScreensaverEditor.Unconfirmed) {
                        editor.reconcileSaved(artwork.pendingSave, confirmed)
                        artwork.pendingSave = 0
                        artwork.submittedStyle = null
                    } else {
                        editor.beginEdit(window.deviceName, confirmed, artwork.defaultStyle)
                        editor.focusEditor()
                    }
                } else if (reply.state === "saved" && artwork.pendingSave) {
                    editor.completeSave(artwork.pendingSave, PrivacyScreensaverEditor.Succeeded,
                        reply.message, artwork.submittedStyle)
                    artwork.pendingSave = 0
                    artwork.submittedStyle = null
                } else if (reply.state === "unconfirmed" && artwork.pendingSave) {
                    editor.completeSave(artwork.pendingSave, PrivacyScreensaverEditor.Unconfirmed, reply.message)
                } else if (reply.state === "failed" || reply.state === "unsupported" || reply.state === "unconfirmed") {
                    if (artwork.pendingSave) {
                        editor.completeSave(artwork.pendingSave, PrivacyScreensaverEditor.Failed, reply.message)
                        artwork.pendingSave = 0
                        artwork.submittedStyle = null
                    } else {
                        artwork.reason = reply.message || qsTr("当前无法读取被控设备的屏保设置。")
                    }
                }
            }
        }

        PrivacyScreensaverEditor {
            id: editor
            anchors.fill: parent
            available: artwork.ready
            disabledReason: artwork.reason
            screensaverActive: !!chrome.privacyScreen && chrome.privacyScreen.mode === "screensaver"
            importHint: qsTr("JPG 或 PNG")
            reduceMotion: chrome.reduceMotion
            onCloseRequested: artwork.close()
            onImportRequested: function(id) {
                const source = auroraBridge.choosePrivacyAppearanceImage()
                const path = String(source)
                if (!path) {
                    editor.finishImport(id, "")
                    return
                }
                editor.provideImport(id, {id: "local-" + id, kind: "custom",
                    name: decodeURIComponent(path.slice(path.lastIndexOf("/") + 1)), previewUrl: path})
            }
            onSaveRequested: function(id, style) {
                artwork.pendingSave = id
                artwork.submittedStyle = style
                const accepted = style.kind === "default" ? window.session.requestPrivacyAppearance("reset")
                    : window.session.requestPrivacyAppearance("save", style.previewUrl)
                if (!accepted) {
                    editor.completeSave(id, PrivacyScreensaverEditor.Failed, qsTr("当前无法同步屏保样式。"))
                    artwork.pendingSave = 0
                    artwork.submittedStyle = null
                }
            }
        }
    }

    StreamSessionChrome {
        id: chrome
        anchors.fill: parent
        deviceName: window.deviceName
        phase: session && session.videoReady ? StreamSessionChrome.Connected : StreamSessionChrome.Connecting
        fullScreen: !!session && session.fullScreen
        windowsOverlays: window.windowsChrome
        windowControls: window.windowsChrome ? window : null
        trailingWindowInset: window.windowsChrome ? (compact ? 96 : 114) : 0
        maximized: window.visibility === Window.Maximized
        canSetFullScreen: connected
        canCancel: !connected
        canDisconnect: connected
        readonly property var displayState: session && !window.finished ? session.displayState : ({})
        displays: (displayState.displays || []).map(function(entry) {
            return { id: entry.id, name: entry.name, ready: entry.selectable && displayState.canSelect === true && !displayState.busy,
                aspectBlocked: entry.aspectBlocked,
                kind: entry.kind === "virtual" ? qsTr("虚拟显示器") : qsTr("系统显示器"),
                resolution: entry.resolution, refreshRate: entry.refreshRate,
                removable: entry.removable, removalPending: displayState.busy }
        })
        activeDisplayId: displayState.activeDisplayId || ""
        switchingDisplayId: displayState.switchTarget || ""
        displayControls: displays.length > 0
        canCreateDisplay: displayState.canCreate === true && !displayState.busy
        creatingDisplay: displayState.creating === true
        displayActionError: displayState.message || ""
        onDisplayRequested: function(id) { if (session) session.requestDisplay("select", id) }
        onCreateDisplayRequested: if (session) session.requestDisplay("create")
        onRemoveDisplayRequested: function(id) { if (session) session.requestDisplay("remove", id) }
        connectionMetrics: session ? session.connectionMetrics : ({})
        preferences: window.picturePreferences()
        videoSettingsApplyDuringSession: !!preferences.resolution
        liveVideoBitrateLimit: session && session.videoSettings ? session.videoSettings.liveBitrateMaximum || 0 : 0
        videoSettingsMessage: session && !window.finished ? session.videoSettingsMessage || "" : ""
        networkQuality: session && session.networkPoor ? StreamSessionChrome.Poor : StreamSessionChrome.Normal
        privacyScreen: session && !window.finished ? (session.privacyScreen || null) : null
        privacyScreenMessage: session && !window.finished ? session.privacyScreenMessage : ""
        privacyAppearanceAvailable: connected && !!privacyScreen
        onPrivacyAppearanceRequested: window.openPrivacyAppearance()

        onPanelKindChanged: {
            if (panelKind === "controls" && !window.finished && session) {
                session.refreshPrivacyScreen()
                session.refreshVideoBitrate()
            }
        }
        onCancelRequested: session.requestStop()
        onDisconnectRequested: session.requestStop()
        onFullScreenRequested: function(enabled) { session.setFullScreen(enabled) }
        onResolutionRequested: function(value) { window.requestPictureSetting("resolution", value) }
        onFrameRateRequested: function(value) { window.requestPictureSetting("fps", value) }
        onBitrateRequested: function(value) { window.requestPictureSetting("bitrate", value) }
        onCodecRequested: function(value) { window.requestPictureSetting("video-codec", value) }
        onHdrRequested: function(enabled) { window.requestPictureSetting("hdr", enabled ? "on" : "off") }
        onPrivacyModeRequested: function(mode) {
            if (!window.finished && session)
                session.setPrivacyMode(mode)
        }
    }

}
