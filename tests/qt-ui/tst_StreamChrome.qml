// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/app/qml/stream" as Stream

Item {
    width: 1000
    height: 620

    Rectangle {
        id: surface
        objectName: "testSurface"
        x: 0
        y: chrome.contentTop
        width: chrome.width
        height: chrome.height - y
        color: "#151D2A"
    }

    Stream.StreamSessionChrome {
        id: chrome
        width: parent.width
        height: parent.height
        deviceName: "书房主机"
        reduceMotion: true
        phase: Stream.StreamSessionChrome.Connected
        canCancel: true
        canDisconnect: true
        canSetFullScreen: true
        canCreateDisplay: true
    }

    SignalSpy { id: selectSpy; target: chrome; signalName: "displayRequested" }
    SignalSpy { id: createSpy; target: chrome; signalName: "createDisplayRequested" }
    SignalSpy { id: removeSpy; target: chrome; signalName: "removeDisplayRequested" }
    SignalSpy { id: cancelSpy; target: chrome; signalName: "cancelRequested" }
    SignalSpy { id: disconnectSpy; target: chrome; signalName: "disconnectRequested" }
    SignalSpy { id: fpsSpy; target: chrome; signalName: "frameRateRequested" }
    SignalSpy { id: hdrSpy; target: chrome; signalName: "hdrRequested" }
    SignalSpy { id: bitrateSpy; target: chrome; signalName: "bitrateRequested" }
    SignalSpy { id: privacySpy; target: chrome; signalName: "privacyScreenRequested" }
    SignalSpy { id: mouseSpy; target: chrome; signalName: "mouseModeRequested" }
    SignalSpy { id: audioSpy; target: chrome; signalName: "audioConfigurationRequested" }
    SignalSpy { id: remoteAudioSpy; target: chrome; signalName: "remoteAudioMutedRequested" }
    SignalSpy { id: fullscreenSpy; target: chrome; signalName: "fullScreenRequested" }

    TestCase {
        id: tests
        name: "StreamChrome"
        when: windowShown

        function lookup(name) {
            var item = findChild(chrome, name)
            verify(item !== null, name)
            return item
        }

        function click(name) {
            wait(20)
            var item = lookup(name)
            verify(item.visible, name + " is visible")
            mouseClick(item, item.width / 2, item.height / 2)
            wait(20)
            return item
        }

        function menu(page) {
            click("controlCenter")
            tryCompare(chrome, "panelKind", "controls")
            click("controlPage:" + page)
            tryCompare(lookup("controlBack"), "activeFocus", true)
        }

        function init() {
            chrome.closePanel(false)
            surface.forceActiveFocus()
            chrome.width = 1000
            chrome.height = 620
            chrome.brandAtEnd = false
            chrome.trailingWindowInset = 138
            chrome.leadingWindowInset = 0
            chrome.fullScreen = false
            chrome.revealControls = false
            chrome.networkQuality = Stream.StreamSessionChrome.Normal
            chrome.phase = Stream.StreamSessionChrome.Connected
            chrome.displays = [
                { id: "main", name: "显示器 1", ready: true, resolution: "2560 × 1440", kind: "主显示器" },
                { id: "virtual", name: "虚拟显示器 1", shortName: "虚拟屏 1", ready: true, removable: true },
                { id: "pending", name: "远端显示器 3", ready: false }
            ]
            chrome.activeDisplayId = "main"
            chrome.connectionMetrics = ({ fps: "不使用", latency: "12 ms", frameRate: "59.9 fps", networkLoss: "0%" })
            chrome.preferences = ({
                resolution: { value: "1920x1080", writable: true, options: [
                    {value: "1280x720", label: "720p"}, {value: "1920x1080", label: "1080p"},
                    {value: "2560x1440", label: "1440p"}, {value: "3840x2160", label: "4K"} ] },
                frameRate: { value: 60, writable: true, options: [ {value: 30, label: "30 FPS"}, {value: 60, label: "60 FPS"} ] },
                bitrate: {value: 20000, writable: true, minimum: 500, maximum: 150000, step: 500},
                codec: {value: "auto", writable: true, options: [
                    {value: "auto", label: "自动"}, {value: "h264", label: "H.264"},
                    {value: "hevc", label: "HEVC"}, {value: "av1", label: "AV1"} ] },
                hdr: {value: false, writable: true},
                performanceOverlay: {value: false, writable: false},
                audioConfiguration: {value: "stereo", writable: true, options: [
                    {value: "stereo", label: "立体声"}, {value: "5.1", label: "5.1 声道"} ] },
                remoteAudioMuted: {value: true, writable: true},
                muteOnFocusLoss: {value: false, writable: true},
                swapMouseButtons: {value: false, writable: true},
                reverseScroll: {value: false, writable: true}
            })
            chrome.mouseMode = ({value: "desktop", writable: true, options: [
                {value: "desktop", label: "桌面指针"}, {value: "relative", label: "锁定鼠标（相对）"} ]})
            chrome.privacyScreen = null
            chrome.creatingDisplay = false
            chrome.canCreateDisplay = true
            chrome.displayActionError = ""
            var spies = [selectSpy, createSpy, removeSpy, cancelSpy, disconnectSpy, fpsSpy, hdrSpy,
              bitrateSpy, privacySpy, mouseSpy, audioSpy, remoteAudioSpy, fullscreenSpy]
            spies.forEach(function(spy) { spy.clear() })
            wait(20)
        }

        function test_connectionAndDismissal() {
            chrome.phase = Stream.StreamSessionChrome.Connecting
            compare(lookup("connectionStatus").text, "连接中")
            verify(!lookup("controlCenter").enabled)
            verify(!lookup("addDisplay").enabled)
            click("endSession")
            compare(cancelSpy.count, 1)

            chrome.phase = Stream.StreamSessionChrome.Failed
            compare(lookup("connectionStatus").text, "连接失败")
            verify(!lookup("endSession").visible)
            compare(surface.y, 44)

            chrome.phase = Stream.StreamSessionChrome.Connected
            chrome.networkQuality = Stream.StreamSessionChrome.Poor
            compare(lookup("connectionStatus").text, "网络较差")
            click("connectionStatus")
            tryCompare(chrome, "panelKind", "connection")
            compare(lookup("metric:接收帧率").text, "59.9 fps")
            compare(lookup("metric:网络丢帧").text, "0%")
            compare(findChild(chrome, "metric:平均解码耗时"), null)
            mouseClick(surface, 700, 480)
            compare(chrome.panelKind, "")

            chrome.networkQuality = Stream.StreamSessionChrome.Bad
            compare(lookup("connectionStatus").text, "网络很差")
            click("connectionStatus")
            keyClick(Qt.Key_Escape)
            compare(chrome.panelKind, "")
            verify(lookup("connectionStatus").activeFocus)
            click("endSession")
            compare(disconnectSpy.count, 1)
        }

        function test_displayLifecycle() {
            click("displayTab:main")
            tryCompare(chrome, "panelKind", "displays")
            compare(lookup("metric:桌面分辨率").text, "2560 × 1440")
            click("displayTab:main")
            compare(chrome.panelKind, "")

            click("displayTab:pending")
            compare(selectSpy.count, 0)
            click("displayTab:virtual")
            compare(selectSpy.signalArguments[0][0], "virtual")
            compare(chrome.activeDisplayId, "main")
            chrome.activeDisplayId = "virtual"
            click("closeTab:virtual")
            compare(removeSpy.signalArguments[0][0], "virtual")
            compare(chrome.displays.length, 3)
            chrome.displays = [chrome.displays[0], chrome.displays[2]]
            chrome.activeDisplayId = "main"
            compare(findChild(chrome, "closeTab:virtual"), null)
            click("addDisplay")
            compare(createSpy.count, 1)
            chrome.creatingDisplay = true
            click("addDisplay")
            compare(createSpy.count, 1)
            chrome.creatingDisplay = false
            chrome.displayActionError = "暂时无法新增虚拟显示器"
            click("addDisplay")
            compare(createSpy.count, 2)
        }

        function test_compactAndScrolling() {
            chrome.width = 760
            verify(lookup("displayStrip").visible)
            chrome.width = 759
            verify(lookup("compactDisplays").visible)
            compare(lookup("controlCenter").width, 32)
            chrome.width = 490
            compare(lookup("connectionStatus").width, 94)
            chrome.width = 489
            compare(lookup("connectionStatus").width, 25)
            chrome.width = 400
            chrome.height = 280
            click("compactDisplays")
            tryCompare(chrome, "panelKind", "displays")
            compare(lookup("streamPanel").x, 8)
            click("displayOption:virtual")
            compare(selectSpy.signalArguments[0][0], "virtual")
            chrome.activeDisplayId = "virtual"
            verify(lookup("displayOption:virtual").selected)
            click("closeOption:virtual")
            compare(removeSpy.signalArguments[0][0], "virtual")
            chrome.displays = [chrome.displays[0],
                { id: "virtual", name: "虚拟显示器 1", ready: true, removable: true, removalPending: true },
                chrome.displays[2]]
            click("closeOption:virtual")
            compare(removeSpy.count, 1)
            click("controlCenter")
            compare(lookup("streamPanel").x, 400 - 288 - 8)
            click("controlPage:quality")
            var scroller = lookup("streamPanelScroll")
            verify(scroller.contentHeight > scroller.height)
            lookup("hdr").forceActiveFocus(Qt.TabFocusReason)
            verify(scroller.contentY > 0)
            var device = lookup("streamDeviceName")
            verify(device.width > 0)
            chrome.trailingWindowInset = 114
            tryCompare(device, "truncated", false)
            compare(surface.y, 44)
        }

        function test_keyboardAndControlRequests() {
            chrome.focusControls()
            keyClick(Qt.Key_Return)
            tryCompare(lookup("controlPage:quality"), "activeFocus", true)
            keyClick(Qt.Key_Return)
            tryCompare(lookup("controlBack"), "activeFocus", true)
            click("frameRate:30")
            compare(fpsSpy.signalArguments[0][0], 30)
            verify(lookup("frameRate:60").selected)
            verify(!lookup("frameRate:30").selected)
            click("hdr")
            compare(hdrSpy.signalArguments[0][0], true)
            compare(lookup("hdr").Accessible.checked, false)
            verify(!lookup("performanceOverlay").enabled)
            var slider = lookup("bitrateSlider")
            slider.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Right)
            tryCompare(bitrateSpy, "count", 1)
            compare(bitrateSpy.signalArguments[0][0], 20500)
            tryCompare(slider, "value", 20000)
            keyClick(Qt.Key_Escape)
            tryCompare(lookup("controlPage:quality"), "activeFocus", true)
            keyClick(Qt.Key_Escape)
            tryCompare(lookup("controlCenter"), "activeFocus", true)
        }

        function test_audioSettingsAndTabWrap() {
            chrome.focusControls()
            keyClick(Qt.Key_Return)
            tryCompare(lookup("controlPage:quality"), "activeFocus", true)
            keyClick(Qt.Key_Tab, Qt.ShiftModifier)
            tryCompare(lookup("fullScreen"), "activeFocus", true)
            keyClick(Qt.Key_Tab)
            tryCompare(lookup("controlPage:quality"), "activeFocus", true)
            click("controlPage:audio")
            click("audioConfiguration:5.1")
            compare(audioSpy.count, 1)
            compare(audioSpy.signalArguments[0][0], "5.1")
            verify(lookup("audioConfiguration:stereo").selected)
            click("remoteAudioMuted")
            compare(remoteAudioSpy.count, 1)
            compare(remoteAudioSpy.signalArguments[0][0], false)
            verify(lookup("remoteAudioMuted").Accessible.checked)
            chrome.phase = Stream.StreamSessionChrome.Failed
            compare(chrome.panelOpen, false)
        }

        function test_wideTabOverflow() {
            var toolbar = lookup("streamTitleBar")
            chrome.width = 760
            var model = chrome.displays.slice()
            for (var i = 0; i < 8; ++i)
                model.push({ id: "extra" + i, name: "Display " + i, ready: true })
            chrome.displays = model
            var strip = lookup("displayStrip")
            tryVerify(function() { return strip.contentWidth > strip.width })
            wait(20)
            var add = lookup("addDisplay")
            var addX = add.mapToItem(toolbar, 0, 0).x
            chrome.activeDisplayId = "extra7"
            tryVerify(function() { return strip.contentX > 0 })
            compare(add.mapToItem(toolbar, 0, 0).x, addX)
            click("displayTab:extra7")
            compare(chrome.panelKind, "displays")
        }

        function test_immediateControls() {
            menu("input")
            click("mouseMode:relative")
            compare(mouseSpy.signalArguments[0][0], "relative")
            verify(lookup("mouseMode:desktop").selected)
            click("controlBack")
            click("controlPage:safety")
            verify(!lookup("privacyScreen").enabled)
            chrome.privacyScreen = ({value: false, writable: true})
            click("privacyScreen")
            compare(privacySpy.signalArguments[0][0], true)
            compare(chrome.privacyScreen.value, false)
            chrome.privacyScreen = ({value: true, writable: false})
            click("privacyScreen")
            compare(privacySpy.count, 1)
            click("controlBack")
            click("fullScreen")
            compare(fullscreenSpy.signalArguments[0][0], true)
            compare(chrome.fullScreen, false)
            surface.forceActiveFocus()
            chrome.fullScreen = true
            compare(surface.y, 0)
            chrome.focusControls()
            tryCompare(lookup("controlCenter"), "activeFocus", true)
            keyClick(Qt.Key_Escape)
            compare(fullscreenSpy.signalArguments[1][0], false)
            compare(surface.y, 0)
        }
    }
}
