// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../qml"

AuroraMainWindow {
    id: demo
    visible: true
    deviceAccess: "ready"
    accountName: "林小满"
    accountSubtitle: "Tailscale"
    deviceModel: fixtures
    enabledDeviceTools: ({
        study: ["files", "view", "terminal", "ports"],
        studio: ["files", "view", "terminal", "ports"]
    })

    ListModel {
        id: fixtures
        ListElement {
            deviceId: "study"
            displayName: "书房主机"
            online: true
            canConnect: true
            lanAddress: "192.168.1.36"
            tailscaleAddress: "100.100.0.36"
            wallpaperUrl: ""
        }
        ListElement {
            deviceId: "studio"
            displayName: "工作室电脑"
            online: true
            canConnect: true
            lanAddress: ""
            tailscaleAddress: "100.100.0.24"
            wallpaperUrl: ""
        }
        ListElement {
            deviceId: "laptop"
            displayName: "随身笔电"
            online: true
            canConnect: false
            lanAddress: ""
            tailscaleAddress: "100.100.0.38"
            wallpaperUrl: ""
        }
        ListElement {
            deviceId: "living"
            displayName: "客厅电脑"
            online: false
            canConnect: false
            lanAddress: ""
            tailscaleAddress: "100.100.0.56"
            wallpaperUrl: ""
        }
    }

    onRefreshRequested: {
        console.info("Demo refreshRequested")
        refreshFeedback = ""
        refreshing = true
        demoRefresh.restart()
    }
    onConnectRequested: function(deviceId) { console.info("Demo connectRequested", deviceId) }
    onDeviceToolRequested: function(deviceId, action) { console.info("Demo deviceToolRequested", deviceId, action) }
    onMoreRequested: function(action) { console.info("Demo moreRequested", action) }
    onNavigationRequested: function(destination) { console.info("Demo navigationRequested", destination) }

    Timer {
        id: demoRefresh
        interval: 700
        onTriggered: {
            demo.refreshFeedback = "演示 · 已刷新设备列表"
            demo.refreshing = false
        }
    }
}
