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
    readonly property int videoTop: chrome.fullScreen ? (chrome.controlsVisible ? chrome.barHeight : 2)
                                                     : chrome.contentTop

    width: 1040
    height: 640
    minimumWidth: 420
    minimumHeight: 280
    title: deviceName
    color: Aurora.AuroraTheme.workspace

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

    function requestPictureSetting(option, value) {
        if (!window.finished && session && !switchingPicture)
            session.requestVideoSetting(option, String(value))
    }

    function updateViewport() {
        if (!finished && visible && session)
            session.setViewport(Qt.rect(0, videoTop, width, Math.max(0, height - videoTop)), chrome.panelOpen)
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

    StreamSessionChrome {
        id: chrome
        anchors.fill: parent
        deviceName: window.deviceName
        phase: session && session.videoReady ? StreamSessionChrome.Connected : StreamSessionChrome.Connecting
        fullScreen: !!session && session.fullScreen
        canSetFullScreen: connected
        canCancel: !connected
        canDisconnect: connected
        connectionMetrics: session ? session.connectionMetrics : ({})
        preferences: window.picturePreferences()
        videoSettingsApplyDuringSession: !!preferences.resolution
        liveVideoBitrateLimit: session && session.videoSettings ? session.videoSettings.liveBitrateMaximum || 0 : 0
        videoSettingsMessage: session && !window.finished ? session.videoSettingsMessage || "" : ""
        networkQuality: session && session.networkPoor ? StreamSessionChrome.Poor : StreamSessionChrome.Normal
        privacyScreen: session && !window.finished ? (session.privacyScreen || null) : null
        privacyScreenMessage: session && !window.finished ? session.privacyScreenMessage : ""

        onPanelOpenChanged: window.updateViewport()
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
