// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/app/qml/stream" as Stream

Item {
    width: 900
    height: 150

    Stream.MacConnectedTitleBar {
        id: chrome
        width: 780
        height: implicitHeight
        layoutWidth: 880
        deviceName: "测试主机"
        reduceMotion: true
    }

    SignalSpy { id: panels; target: chrome; signalName: "nativePanelRequested" }

    SignalSpy { id: displayRequests; target: chrome; signalName: "displayRequested" }
    SignalSpy { id: createRequests; target: chrome; signalName: "createDisplayRequested" }

    TestCase {
        name: "MacConnectedTitleBar"
        when: windowShown

        function test_connectedState() {
            compare(chrome.height, 44)
            compare(chrome.statusText, "已连接")
            chrome.networkPoor = true
            compare(chrome.statusText, "网络较差")
            chrome.networkPoor = false
            compare(findChild(chrome, "streamDeviceName").text, chrome.deviceName)
            verify(findChild(chrome, "displayTab:current").visible)
            verify(!findChild(chrome, "displayTab:current").enabled)
            verify(!findChild(chrome, "addDisplay").visible)
            verify(!findChild(chrome, "endSession").visible)
        }

        function test_displayInventoryRespectsHostCapability() {
            var physical = {id: "10", name: "主显示器", kind: "physical", ready: true}
            var virtual = {id: "20", name: "Aurora 虚拟显示器", kind: "virtual", ready: true, removable: true}
            displayRequests.clear()
            createRequests.clear()
            chrome.displaySnapshot = {activeDisplayId: "10", canCreate: true, canSelect: false,
                                      displays: [physical, virtual]}
            tryVerify(function() { return !!findChild(chrome, "displayTab:20") })
            verify(!findChild(chrome, "displayTab:20").enabled)
            verify(findChild(chrome, "addDisplay").enabled)
            mouseClick(findChild(chrome, "addDisplay"))
            compare(createRequests.count, 1)
            compare(displayRequests.count, 0)

            chrome.displayRequestBusy = true
            verify(!findChild(chrome, "addDisplay").enabled)
            verify(!findChild(chrome, "closeTab:20").enabled)
            chrome.displayRequestBusy = false
            chrome.displaySnapshot = {}
        }

        function test_panelActions() {
            panels.clear()
            mouseClick(findChild(chrome, "connectionStatus"))
            compare(panels.count, 1)
            compare(panels.signalArguments[0][0], "connection")
            compare(chrome.panelOpen, false)

            var controls = findChild(chrome, "controlCenter")
            mouseClick(controls)
            compare(panels.count, 2)
            compare(panels.signalArguments[1][0], "controls")
            chrome.panelKind = "controls"
            verify(controls.selected)
            verify(!findChild(chrome, "streamPanel").visible)
            chrome.panelKind = ""

            chrome.focusNativeControls()
            tryCompare(controls, "visualFocus", true)
            keyClick(Qt.Key_Space)
            compare(panels.count, 3)
            compare(panels.signalArguments[2][0], "controls")
            compare(panels.signalArguments[2][1], true)
        }

        function test_usesWindowWidthForResponsiveLayout() {
            chrome.width = 655
            chrome.layoutWidth = 761
            compare(chrome.compact, false)
            chrome.layoutWidth = 759
            compare(chrome.compact, true)
            verify(findChild(chrome, "compactDisplays").visible)
            verify(!findChild(chrome, "compactDisplays").enabled)
            chrome.layoutWidth = 400
            chrome.width = 288
            compare(chrome.tight, true)
            verify(findChild(chrome, "connectionStatus").enabled)
            verify(findChild(chrome, "controlCenter").enabled)
            chrome.layoutWidth = 880
            chrome.width = 780
        }
    }
}
