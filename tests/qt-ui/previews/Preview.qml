// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../../../desktop/app/qml/stream" as Stream

Item {
    id: preview
    width: 1000
    height: 620
    property alias chrome: chrome

    Rectangle {
        id: placeholder
        y: chrome.contentTop
        width: parent.width
        height: parent.height - y
        color: chrome.connected ? "#151D2A" : "#F9FAFC"

        Image {
            anchors.fill: parent
            visible: chrome.connected
            source: "../../../docs/visual-design/assets/wallpapers/default/mountain-valley.webp"
            fillMode: Image.PreserveAspectFit
        }
    }

    Stream.StreamSessionChrome {
        id: chrome
        anchors.fill: parent
        deviceName: qsTr("书房主机")
        phase: Stream.StreamSessionChrome.Connected
        canCreateDisplay: true
        canCancel: true
        canDisconnect: true
        canSetFullScreen: true
        trailingWindowInset: brandAtEnd ? 0 : tight ? 114 : 138
        leadingWindowInset: brandAtEnd ? 78 : 0
        activeDisplayId: "main"
        displays: [
            { id: "main", name: qsTr("显示器 1"), ready: true, kind: qsTr("主显示器"),
              resolution: "2560 × 1440", refreshRate: "60 Hz", scale: "125%" },
            { id: "virtual1", name: qsTr("虚拟显示器 1"), shortName: qsTr("虚拟屏 1"), ready: true, removable: true },
            { id: "virtual2", name: qsTr("虚拟显示器 2"), shortName: qsTr("虚拟屏 2"), ready: true, removable: true }
        ]
        connectionMetrics: ({
            latency: "12 ms", resolution: "1920 × 1080", frameRate: "59.9 fps",
            bitrate: "18.6 Mbps", networkLoss: "0.0%", codec: "HEVC (H.265)", decodeTime: "1.8 ms"
        })
        preferences: ({
            resolution: {value: "1920x1080", writable: true, options: [
                {value: "1280x720", label: "720p"}, {value: "1920x1080", label: "1080p"},
                {value: "2560x1440", label: "1440p"}, {value: "3840x2160", label: "4K"} ]},
            frameRate: {value: 60, writable: true, options: [{value: 30, label: "30 FPS"}, {value: 60, label: "60 FPS"}]},
            bitrate: {value: 20000, minimum: 500, maximum: 150000, step: 500, writable: true},
            codec: {value: "auto", writable: true, options: [
                {value: "auto", label: qsTr("自动")}, {value: "h264", label: "H.264"},
                {value: "hevc", label: "HEVC"}, {value: "av1", label: "AV1"} ]},
            hdr: {value: false, writable: true},
            performanceOverlay: {value: false, writable: true},
            audioConfiguration: {value: "stereo", writable: true, options: [
                {value: "stereo", label: qsTr("立体声")}, {value: "5.1", label: qsTr("5.1 声道")}, {value: "7.1", label: qsTr("7.1 声道")} ]},
            remoteAudioMuted: {value: true, writable: true},
            muteOnFocusLoss: {value: false, writable: true},
            swapMouseButtons: {value: false, writable: true},
            reverseScroll: {value: false, writable: true}
        })
        mouseMode: ({value: "desktop", writable: true, options: [
            {value: "desktop", label: qsTr("桌面指针")}, {value: "relative", label: qsTr("锁定鼠标（相对）")} ]})
        privacyScreen: ({mode: "off", writable: true})

        onDisplayRequested: function(displayId) {
            activeDisplayId = displayId
            console.log("display", displayId)
        }
        onFullScreenRequested: function(enabled) {
            fullScreen = enabled
            console.log("fullscreen", enabled)
        }
        onPrivacyModeRequested: function(mode) { privacyScreen = ({mode: mode, writable: true}) }
        onCancelRequested: console.log("cancel")
        onDisconnectRequested: console.log("disconnect")
        onCreateDisplayRequested: console.log("createDisplay")
        onRemoveDisplayRequested: function(displayId) { console.log("removeDisplay", displayId) }
    }
}
