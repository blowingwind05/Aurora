// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import "../common"
import "../pages" as Pages
import "../pages/devices" as Devices
import "../pages/host" as Host
import "../pages/tailscale" as Tailscale

Window {
    id: window
    title: qsTr("Aurora远程桌面")
    width: 1000
    height: 580
    minimumWidth: 320
    minimumHeight: 400
    color: qmlWindowFrame ? "transparent" : AuroraTheme.frame
    flags: qmlWindowFrame ? Qt.Window | Qt.FramelessWindowHint : Qt.Window

    property bool customWindowChrome: AuroraTheme.windows
    property bool nativeWindowsFrame: false
    readonly property bool qmlWindowFrame: customWindowChrome && !nativeWindowsFrame
    property int macTitleBarHeight: 52
    property var deviceModel: null
    property int deviceEpoch: -1
    property string deviceAccess: "loading"
    property bool refreshAvailable: true
    property bool refreshing: false
    property string refreshFeedback: ""
    property bool refreshFailed: false
    property int refreshRevision: 0
    property bool deviceRefreshResult: false
    property bool reduceMotion: false
    property bool connectionPending: false
    property var enabledDeviceTools: ({})
    property string accountName: ""
    property string accountSubtitle: ""
    property url accountAvatar
    property var hostState: ({})
    property string currentPage: "devices"
    property Component destinationPage: null
    property string moreVersion: ""
    property string moreFeedbackAddress: ""
    property bool checkingUpdates: false
    property string moreNoticeText: ""
    readonly property bool showingMorePage: currentPage === "about" || currentPage === "feedback"

    signal navigationRequested(string destination)
    signal moreRequested(string action)
    signal hostActionRequested(string action)
    signal sunshineRequested(string action, var data)
    signal managedCloseRequested()
    property bool managedClose: false

    function navigateFromSidebar(destination) {
        if (currentPage === "host" && destination !== "host" && hostPage.hasUnfinishedSettings)
            hostPage.finishSettings(function() { window.navigationRequested(destination) })
        else
            navigationRequested(destination)
    }

    onClosing: function(event) {
        if (hostPage.hasUnfinishedSettings) {
            event.accepted = false
            hostPage.finishSettings(function() { window.close() })
        } else if (managedClose) {
            event.accepted = false
            managedCloseRequested()
        }
    }

    signal refreshRequested()
    signal connectRequested(string deviceId)
    signal deviceToolRequested(string deviceId, string action)
    signal devicePowerRequested(string deviceId, bool restart, int epoch)

    property int nextPowerConfirmationId: 0
    property var pendingDevicePower: null
    property Devices.DeviceCard powerDevice: null
    property Item powerFocus: null
    readonly property bool devicePowerContext: visible && visibility !== Window.Minimized &&
        currentPage === "devices" && deviceAccess === "ready"
    onDevicePowerContextChanged: invalidateDevicePower()
    onDeviceEpochChanged: invalidateDevicePower()
    onPowerDeviceChanged: invalidateDevicePower()

    Connections {
        target: window.powerDevice
        function onPowerAvailableChanged() { window.invalidateDevicePower() }
        function onDeviceIdChanged() { window.invalidateDevicePower() }
        function onDisplayNameChanged() { window.invalidateDevicePower() }
    }

    function devicePowerCurrent(request) {
        return devicePowerContext && deviceEpoch === request.epoch && powerDevice && powerDevice.powerAvailable &&
            powerDevice.deviceId === request.deviceId && powerDevice.displayName === request.displayName
    }

    function invalidateDevicePower() {
        if (pendingDevicePower && !devicePowerCurrent(pendingDevicePower))
            clearDevicePower()
    }

    function confirmDevicePower(device, restart, epoch, source) {
        if (pendingDevicePower || !devicePowerContext || epoch !== deviceEpoch || !device.powerAvailable)
            return
        powerDevice = device
        powerFocus = source
        const name = device.displayName
        pendingDevicePower = {
            id: ++nextPowerConfirmationId,
            deviceId: device.deviceId,
            displayName: name,
            epoch: epoch,
            restart: restart,
            title: restart ? qsTr("要重启「%1」吗？").arg(name) : qsTr("要将「%1」关机吗？").arg(name),
            body: restart ? qsTr("重启将断开远程连接。请先保存这台设备上的工作。")
                          : qsTr("关机将断开远程连接。请先保存这台设备上的工作，重新开机后才能连接。"),
            label: restart ? qsTr("确认重启") : qsTr("确认关机"),
            destructive: true
        }
    }

    function clearDevicePower() {
        pendingDevicePower = null
        powerDevice = null
        powerFocus = null
    }

    Tailscale.TailscaleConfirmation {
        objectName: "devicePowerConfirmation"
        confirmation: window.pendingDevicePower
        returnFocusItem: window.powerFocus
        onDecision: function(id, approved) {
            const request = window.pendingDevicePower
            if (!request || id !== request.id)
                return
            const send = approved && window.devicePowerCurrent(request)
            window.clearDevicePower()
            if (send)
                window.devicePowerRequested(request.deviceId, request.restart, request.epoch)
        }
    }

    readonly property bool compact: width <= 470
    readonly property bool narrow: width <= 650
    readonly property bool maximized: visibility === Window.Maximized
    readonly property bool expanded: maximized || visibility === Window.FullScreen
    readonly property int sidebarWidth: compact ? 43 : narrow ? 144 : 184
    readonly property int windowRadius: qmlWindowFrame && !expanded ? 13 : 0

    function showMoreNotice(message, successful) {
        moreNoticeText = message
        moreNotice.successful = successful === true
        moreNotice.visibleWhileTimed = true
        moreNoticeTimer.restart()
    }

    function toggleMaximized() {
        if (maximized)
            showNormal()
        else if (!expanded)
            showMaximized()
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        color: AuroraTheme.frame
        radius: window.windowRadius
    }

    Item {
        id: titlebar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 1
        anchors.topMargin: AuroraTheme.mac ? 0 : anchors.margins
        height: AuroraTheme.mac ? window.macTitleBarHeight : 52

        MouseArea {
            anchors.fill: parent
            enabled: window.customWindowChrome && !window.expanded
            onDoubleClicked: window.toggleMaximized()
        }
        DragHandler {
            target: null
            enabled: window.customWindowChrome && !window.expanded
            onActiveChanged: {
                if (active)
                    window.startSystemMove()
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: window.customWindowChrome && window.maximized
            onDoubleClicked: window.toggleMaximized()
        }

        Row {
            anchors.left: AuroraTheme.mac ? undefined : parent.left
            anchors.right: AuroraTheme.mac ? parent.right : undefined
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Item {
                width: 32
                height: 32
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 1

                Image {
                    width: AuroraTheme.windows ? 24 : 32
                    height: width
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    source: AuroraTheme.desktopAsset(AuroraTheme.windows ? "Aurora-Windows-32.png" : "Aurora.png")
                }
            }
            Item {
                width: latin.width + han.width
                height: 20
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    id: latin
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Aurora"
                    font.family: AuroraTheme.windows ? "Segoe UI" : Qt.application.font.family
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: AuroraTheme.ink
                }
                Text {
                    id: han
                    anchors.left: latin.right
                    anchors.baseline: latin.baseline
                    text: qsTr("远程桌面")
                    font.family: AuroraTheme.font
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: AuroraTheme.ink
                }
            }
        }

        Row {
            visible: window.customWindowChrome && window.visibility !== Window.FullScreen
            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            component CaptionButton: AuroraButton {
                id: caption
                required property string glyph
                width: window.compact ? 32 : 40
                height: 34
                hint: text

                contentItem: Text {
                    text: caption.glyph
                    font.family: "Segoe MDL2 Assets"
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    renderType: Text.NativeRendering
                    color: caption.foreground
                    Accessible.ignored: true
                }
            }
            CaptionButton {
                text: qsTr("最小化")
                glyph: "\uE921"
                onClicked: window.showMinimized()
            }
            CaptionButton {
                objectName: "maximizeWindow"
                text: window.maximized ? qsTr("还原") : qsTr("最大化")
                glyph: window.maximized ? "\uE923" : "\uE922"
                onClicked: window.toggleMaximized()
            }
            CaptionButton {
                text: qsTr("关闭")
                glyph: "\uE8BB"
                hoverFill: "#C42B1C"
                foreground: hovered || down ? "white" : AuroraTheme.ink
                onClicked: window.close()
            }
        }
    }

    AuroraSidebar {
        id: sidebar
        objectName: "auroraSidebar"
        anchors.top: titlebar.bottom
        anchors.left: parent.left
        anchors.leftMargin: 1
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 1
        width: window.sidebarWidth
        compact: window.compact
        currentPage: window.currentPage
        checkingUpdates: window.checkingUpdates
        reduceMotion: window.reduceMotion
        hostAvailable: window.hostState.available !== false
        accountName: window.accountName
        accountSubtitle: window.accountSubtitle
        accountAvatar: window.accountAvatar
        onNavigationRequested: function(destination) { window.navigateFromSidebar(destination) }
        onMoreRequested: function(action) { window.moreRequested(action) }
    }

    Rectangle {
        id: workspace
        objectName: "workspace"
        anchors.top: titlebar.bottom
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 4
        anchors.bottomMargin: 4
        color: AuroraTheme.workspace
        radius: 12

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.radius
            color: parent.color
        }

        Devices.DevicesPage {
            objectName: "devicesPage"
            anchors.fill: parent
            visible: window.currentPage === "devices"
            compact: window.compact
            narrow: window.narrow
            deviceModel: window.deviceModel
            deviceEpoch: window.deviceEpoch
            accessState: window.deviceAccess
            refreshAvailable: window.refreshAvailable
            refreshing: window.refreshing
            refreshFeedback: window.refreshFeedback
            refreshFailed: window.refreshFailed
            refreshRevision: window.refreshRevision
            deviceRefreshResult: window.deviceRefreshResult
            reduceMotion: window.reduceMotion
            connectionPending: window.connectionPending
            enabledDeviceTools: window.enabledDeviceTools
            onRefreshRequested: window.refreshRequested()
            onConnectRequested: function(deviceId) { window.connectRequested(deviceId) }
            onToolRequested: function(deviceId, action) { window.deviceToolRequested(deviceId, action) }
            onPowerConfirmationRequested: function(device, restart, epoch, source) {
                window.confirmDevicePower(device, restart, epoch, source)
            }
            onAccountRequested: window.navigationRequested("account")
        }

        Host.HostPage {
            id: hostPage
            objectName: "hostPage"
            anchors.fill: parent
            visible: window.currentPage === "host"
            compact: window.compact
            narrow: window.narrow
            reduceMotion: window.reduceMotion
            hostState: window.hostState
            onActionRequested: function(action) { window.hostActionRequested(action) }
            onSunshineRequested: function(action, data) { window.sunshineRequested(action, data) }
        }

        Loader {
            anchors.fill: parent
            active: window.currentPage !== "devices" && window.currentPage !== "host"
            sourceComponent: window.showingMorePage ? morePage : window.destinationPage
        }

        Component {
            id: morePage
            Pages.MorePage {
                section: window.currentPage
                compact: window.compact
                narrow: window.narrow
                versionLabel: window.moreVersion
                feedbackAddress: window.moreFeedbackAddress
                onLinkRequested: function(destination) { window.moreRequested(destination) }
            }
        }
    }

    Rectangle {
        id: moreNotice
        objectName: "moreNotice"
        property bool visibleWhileTimed: false
        property bool successful: false
        readonly property int iconReserve: successful ? 28 : 0
        anchors.centerIn: parent
        z: 2
        width: Math.min(window.width - 32, message.implicitWidth + 32 + iconReserve)
        height: message.implicitHeight + 22
        radius: 10
        color: "white"
        border.color: AuroraTheme.panelLine
        opacity: visibleWhileTimed ? 1 : 0
        visible: opacity > 0
        Behavior on opacity {
            enabled: !window.reduceMotion
            NumberAnimation { duration: 160 }
        }

        AuroraGlyph {
            visible: moreNotice.successful
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            icon.source: Qt.resolvedUrl("../../../../docs/visual-design/assets/icons/phosphor/check-bold.svg")
            icon.color: AuroraTheme.statusBlue
        }

        Text {
            id: message
            objectName: "moreNoticeText"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16 + moreNotice.iconReserve
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: moreNotice.successful ? Text.AlignLeft : Text.AlignHCenter
            text: window.moreNoticeText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AuroraTheme.ink
            font.family: AuroraTheme.font
            font.pixelSize: moreNotice.successful ? 15 : 13
            font.weight: moreNotice.successful ? Font.Bold : Font.Normal
            Accessible.role: Accessible.StaticText
        }

        Timer {
            id: moreNoticeTimer
            interval: 3000
            onTriggered: moreNotice.visibleWhileTimed = false
        }
    }

    Rectangle {
        objectName: "windowOutline"
        anchors.fill: parent
        visible: window.qmlWindowFrame
        color: "transparent"
        radius: window.windowRadius
        border.color: AuroraTheme.frameLine
        border.width: 1
    }

    Repeater {
        model: window.customWindowChrome && !window.expanded ? [
            Qt.LeftEdge, Qt.RightEdge, Qt.TopEdge, Qt.BottomEdge,
            Qt.LeftEdge | Qt.TopEdge, Qt.RightEdge | Qt.TopEdge,
            Qt.LeftEdge | Qt.BottomEdge, Qt.RightEdge | Qt.BottomEdge
        ] : []

        MouseArea {
            required property int modelData
            readonly property bool resizeLeft: (modelData & Qt.LeftEdge) !== 0
            readonly property bool resizeRight: (modelData & Qt.RightEdge) !== 0
            readonly property bool resizeTop: (modelData & Qt.TopEdge) !== 0
            readonly property bool resizeBottom: (modelData & Qt.BottomEdge) !== 0
            readonly property bool resizeHorizontal: resizeLeft || resizeRight
            readonly property bool resizeVertical: resizeTop || resizeBottom

            width: resizeHorizontal ? (resizeVertical ? 10 : 5) : window.width - 20
            height: resizeVertical ? (resizeHorizontal ? 10 : 5) : window.height - 20
            x: resizeLeft ? 0 : resizeRight ? window.width - width : 10
            y: resizeTop ? 0 : resizeBottom ? window.height - height : 10
            hoverEnabled: true
            cursorShape: resizeHorizontal && resizeVertical ? (resizeLeft === resizeTop ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor)
                : resizeHorizontal ? Qt.SizeHorCursor : Qt.SizeVerCursor
            onPressed: window.startSystemResize(modelData)
        }
    }
}
