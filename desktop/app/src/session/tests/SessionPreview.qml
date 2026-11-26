// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../../../qml/stream" as Stream

Item {
    id: preview
    width: 780
    height: 480
    property alias chrome: chrome

    readonly property int videoTop: chrome.contentTop

    function updateViewport() {
        streamSurface.setViewport(Qt.rect(0, videoTop, width, Math.max(0, height - videoTop)), false)
    }

    onWidthChanged: updateViewport()
    onHeightChanged: updateViewport()
    onVideoTopChanged: updateViewport()
    Component.onCompleted: updateViewport()

    Rectangle { anchors.fill: parent; color: "#151D2A" }

    Stream.StreamSessionChrome {
        id: chrome
        anchors.fill: parent
        deviceName: "Synthetic stream surface"
        phase: streamSurface.attachmentFailed ? Stream.StreamSessionChrome.Failed
            : streamSurface.videoReady ? Stream.StreamSessionChrome.Connected : Stream.StreamSessionChrome.Connecting
        canCancel: true
        canDisconnect: true
        canSetFullScreen: true
        windowsOverlays: true
        onCancelRequested: streamSurface.requestStop()
        onDisconnectRequested: streamSurface.requestStop()
        onFullScreenRequested: function(enabled) { fullScreen = enabled }
    }

    Connections {
        target: streamSurface
        function onFullscreenRequested() { chrome.fullScreen = !chrome.fullScreen }
    }
}
