// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/app/qml/common" as Aurora

Item {
    width: 560
    height: 540

    ListModel {
        id: catalog
        ListElement {
            deviceId: "test-device"
            displayName: "Fixture"
            online: true
            canConnect: true
            supportsPower: false
            lanAddress: "192.0.2.10"
            tailscaleAddress: ""
            wallpaperUrl: ""
        }
    }

    Loader {
        id: devices
        anchors.fill: parent
    }

    SignalSpy {
        id: confirmation
        target: devices.item
        signalName: "powerConfirmationRequested"
    }

    TestCase {
        name: "DevicePower"
        when: windowShown

        function openMenu(card) {
            const button = findChild(card, "tools-test-device")
            const menu = findChild(button, "deviceMenu-test-device")
            mouseClick(button)
            tryCompare(menu, "opened", true)
            return menu
        }

        function test_catalogUpdatesAndReasons() {
            devices.setSource(Qt.resolvedUrl("../../desktop/app/qml/pages/devices/DevicesPage.qml"), {
                deviceModel: catalog, accessState: "ready", deviceEpoch: 3
            })
            tryCompare(devices, "status", Loader.Ready)
            const page = devices.item
            const card = findChild(page, "device-test-device")
            const menu = openMenu(card)
            const restart = findChild(menu, "restart-test-device")
            const shutdown = findChild(menu, "shutdown-test-device")
            const reason = findChild(menu, "powerUnavailable-test-device")
            const copyAddress = findChild(menu, "copyLan-test-device")
            // Make the platform's window text resemble the disabled color.
            menu.palette.windowText = "#8390A1"

            compare(restart.enabled, false)
            compare(shutdown.enabled, false)
            compare(restart.contentItem.children[1].color.toString(), "#8390a1")
            verify(reason.visible)
            compare(reason.text, "被控端未声明支持远程电源操作。")
            mouseClick(restart)
            compare(confirmation.count, 0)

            catalog.setProperty(0, "canConnect", false)
            tryCompare(reason, "text", "未发现可用的被控服务。")
            catalog.setProperty(0, "online", false)
            tryCompare(reason, "text", "设备离线。")
            catalog.set(0, { online: true, canConnect: true, supportsPower: true })
            tryCompare(restart, "enabled", true)
            compare(shutdown.enabled, true)
            compare(reason.visible, false)
            compare(restart.contentItem.children[1].color, Aurora.AuroraTheme.ink)
            compare(shutdown.contentItem.children[1].color, Aurora.AuroraTheme.ink)
            compare(copyAddress.contentItem.children[1].color, Aurora.AuroraTheme.ink)
            compare(shutdown.contentItem.children[0].icon.color, Aurora.AuroraTheme.ink)

            if (!menu.opened)
                openMenu(card)
            mouseClick(shutdown)
            tryCompare(confirmation, "count", 1)
            compare(confirmation.signalArguments[0][0], card)
            compare(confirmation.signalArguments[0][1], false)
            compare(confirmation.signalArguments[0][2], 3)

            openMenu(card)
            mouseClick(restart, restart.leftPadding + restart.icon.width / 2, restart.height / 2)
            tryCompare(confirmation, "count", 2)
            compare(confirmation.signalArguments[1][1], true)

            page.deviceEpoch = -1
            openMenu(card)
            compare(restart.enabled, false)
            compare(shutdown.enabled, false)
            compare(reason.text, "设备列表状态已失效，请刷新。")
            menu.close()
        }
    }
}
