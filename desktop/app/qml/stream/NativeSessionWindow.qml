// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import ".." as Aurora

Window {
    id: window

    property var session
    property string deviceName
    property bool finished: false
    readonly property int videoTop: chrome.fullScreen ? (chrome.controlsVisible ? chrome.barHeight : 2)
                                                     : chrome.contentTop

    width: 1040
    height: 640
    minimumWidth: 420
    minimumHeight: 280
    title: deviceName
    color: Aurora.AuroraTheme.workspace

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
        networkQuality: session && session.networkPoor ? StreamSessionChrome.Poor : StreamSessionChrome.Normal
        privacyScreen: session && !window.finished ? (session.privacyScreen || null) : null
        privacyScreenMessage: session && !window.finished ? session.privacyScreenMessage : ""

        onPanelOpenChanged: window.updateViewport()
        onPanelKindChanged: {
            if (panelKind === "controls" && !window.finished && session)
                session.refreshPrivacyScreen()
        }
        onCancelRequested: session.requestStop()
        onDisconnectRequested: session.requestStop()
        onFullScreenRequested: function(enabled) { session.setFullScreen(enabled) }
        onPrivacyScreenRequested: function(enabled) {
            if (!window.finished && session)
                session.setPrivacyScreen(enabled)
        }
    }

}
