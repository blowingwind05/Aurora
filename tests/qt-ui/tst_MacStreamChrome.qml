// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/app/qml/stream" as Stream

Item {
    width: 750
    height: 390

    Stream.MacConnectionStage {
        id: connection
        anchors.fill: parent
        deviceName: "测试主机"
        reduceMotion: true
    }

    Stream.MacStreamControls {
        id: menu
        visible: false
        x: 12
        y: 8
    }

    SignalSpy { id: cancel; target: connection; signalName: "cancelRequested" }
    SignalSpy { id: retry; target: connection; signalName: "retryRequested" }
    SignalSpy { id: quitPrevious; target: connection; signalName: "quitPreviousRequested" }
    SignalSpy { id: page; target: menu; signalName: "nativeSettingPageRequested" }
    SignalSpy { id: fullscreen; target: menu; signalName: "toggleFullscreenRequested" }
    SignalSpy { id: details; target: menu; signalName: "connectionDetailsRequested" }
    SignalSpy { id: disconnect; target: menu; signalName: "disconnectRequested" }

    TestCase {
        name: "MacStreamChrome"
        when: windowShown

        function click(root, name) {
            var item = findChild(root, name)
            verify(item && item.visible && item.enabled, name)
            mouseClick(item)
            return item
        }

        function test_connectionActions() {
            connection.visible = true
            var titleBar = findChild(connection, "streamTitleBar")
            verify(titleBar !== null)
            var device = findChild(titleBar, "streamDeviceName")
            compare(device.text, connection.deviceName)
            verify(device.mapToItem(connection, 0, 0).x >= connection.trafficLightsInset)

            click(titleBar, "endSession")
            compare(cancel.count, 1)
            connection.connecting = false
            connection.failed = true
            compare(findChild(titleBar, "endSession").visible, false)
            click(connection, "sessionAction")
            compare(retry.count, 1)

            connection.failed = false
            connection.offerQuit = true
            click(connection, "sessionAction")
            compare(quitPrevious.count, 1)
            click(titleBar, "endSession")
            compare(cancel.count, 2)
        }

        function test_fullscreenMenuActions() {
            connection.visible = false
            menu.visible = true
            menu.fullScreen = true
            const initial = fullscreen.count
            click(menu, "controlConnectionDetails")
            compare(details.count, 1)
            const exit = click(menu, "fullScreen")
            compare(exit.text, "退出全屏")
            compare(fullscreen.count, initial + 1)
            menu.fullScreen = false
            menu.visible = false
            connection.visible = true
            fullscreen.clear()
        }

        function test_windowedMenuActions() {
            connection.visible = false
            menu.visible = true
            tryCompare(menu, "width", 288)
            tryVerify(function() { return menu.height > 160 })

            var ids = ["quality", "audio", "input", "safety"]
            for (var i = 0; i < ids.length; ++i) {
                click(menu, "controlPage:" + ids[i])
                compare(page.count, i + 1)
                compare(page.signalArguments[i][0], ids[i])
            }

            menu.focusInitial(Qt.TabFocusReason)
            tryCompare(findChild(menu, "controlPage:quality"), "visualFocus", true)
            click(menu, "fullScreen")
            compare(fullscreen.count, 1)
            click(menu, "disconnectRemote")
            compare(disconnect.count, 1)
        }
    }
}
