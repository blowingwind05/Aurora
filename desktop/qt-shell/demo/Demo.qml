// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../qml"

AuroraMainWindow {
    id: demo
    visible: true
    deviceAccess: "ready"
    hostState: ({
        status: demoHostBusy ? (demoHostAllowed ? "stopping" : "starting")
                            : demoHostAllowed ? "running" : "disabled",
        managed: true,
        allowed: demoHostAllowed,
        toggleAvailable: true,
        busy: demoHostBusy,
        refreshAvailable: !demoHostBusy,
        lanAddress: demoHostAllowed ? "192.0.2.18" : "",
        tailscaleAddress: demoHostAllowed ? "100.100.0.18" : "",
        permissionsAvailable: AuroraTheme.mac && demoHostAllowed,
        settingsAvailable: demoHostAllowed,
        feedback: demoHostFeedback
    })

    property bool demoHostAllowed: true
    property bool demoHostBusy: false
    property string demoHostFeedback: ""

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
    onNavigationRequested: function(destination) {
        if (destination === "host" || destination === "devices")
            demo.currentPage = destination
        else
            console.info("Demo navigationRequested", destination)
    }
    onHostActionRequested: function(action) {
        switch (action) {
        case "hostEnable":
        case "hostDisable":
            demoHostFeedback = ""
            demoHostBusy = true
            demoHostChange.restart()
            break
        case "hostRefresh":
        case "hostContinuePermissions":
            demoHostFeedback = demoHostAllowed ? qsTr("演示 · 检测完成，Sunshine 正在运行。") : qsTr("演示 · 尚未启用 Sunshine。")
            break
        case "hostCopyLan":
        case "hostCopyTailnet":
            demoHostFeedback = qsTr("演示 · 已请求复制地址。")
            break
        case "hostOpenPermissions":
        case "hostOpenSettings":
            demoHostFeedback = qsTr("演示 · 已请求打开设置。")
            break
        }
    }

    Timer {
        id: demoHostChange
        interval: 500
        onTriggered: {
            demo.demoHostAllowed = !demo.demoHostAllowed
            demo.demoHostBusy = false
            demo.demoHostFeedback = demo.demoHostAllowed ? qsTr("演示 · 已允许本机被控") : qsTr("演示 · 已关闭本机被控")
        }
    }

    Timer {
        id: demoRefresh
        interval: 700
        onTriggered: {
            demo.refreshFeedback = "演示 · 已刷新设备列表"
            demo.refreshing = false
        }
    }
}
