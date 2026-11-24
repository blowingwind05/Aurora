// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15

Window {
    id: window
    title: qsTr("Aurora远程桌面")
    width: 1000
    height: 580
    minimumWidth: 320
    minimumHeight: 400
    color: customWindowChrome ? "transparent" : AuroraTheme.frame
    flags: customWindowChrome ? Qt.Window | Qt.FramelessWindowHint : Qt.Window

    property bool customWindowChrome: AuroraTheme.windows
    property var deviceModel: null
    property string deviceAccess: "loading"
    property bool refreshAvailable: true
    property bool refreshing: false
    property string refreshFeedback: ""
    property bool refreshFailed: false
    property bool reduceMotion: false
    property bool connectionPending: false
    property var enabledDeviceTools: ({})
    property string accountName: ""
    property string accountSubtitle: ""
    property string currentPage: "devices"
    property Component destinationPage: null

    signal navigationRequested(string destination)
    signal moreRequested(string action)
    signal refreshRequested()
    signal connectRequested(string deviceId)
    signal deviceToolRequested(string deviceId, string action)

    readonly property bool compact: width <= 470
    readonly property bool narrow: width <= 650
    readonly property bool maximized: visibility === Window.Maximized
    readonly property bool expanded: maximized || visibility === Window.FullScreen
    readonly property int sidebarWidth: compact ? 43 : narrow ? 144 : 184
    readonly property int windowRadius: customWindowChrome && !expanded ? 13 : 0

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
        height: 52

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
                width: window.compact ? 32 : 40
                height: 34
                display: AuroraButton.IconOnly
                icon.width: 15
                icon.height: 15
                hint: text
            }
            CaptionButton {
                text: qsTr("最小化")
                icon.source: AuroraTheme.icon("minus")
                onClicked: window.showMinimized()
            }
            CaptionButton {
                objectName: "maximizeWindow"
                text: window.maximized ? qsTr("还原") : qsTr("最大化")
                icon.source: window.maximized ? "" : AuroraTheme.icon("square")
                onClicked: window.toggleMaximized()

                Item {
                    visible: window.maximized
                    anchors.centerIn: parent
                    width: 12
                    height: 12
                    Rectangle {
                        x: 3
                        width: 9
                        height: 9
                        color: "transparent"
                        border.color: AuroraTheme.ink
                    }
                    Rectangle {
                        y: 3
                        width: 9
                        height: 9
                        color: parent.parent.hovered ? AuroraTheme.hover : AuroraTheme.frame
                        border.color: AuroraTheme.ink
                    }
                }
            }
            CaptionButton {
                text: qsTr("关闭")
                icon.source: AuroraTheme.icon("x")
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
        accountName: window.accountName
        accountSubtitle: window.accountSubtitle
        onNavigationRequested: function(destination) { window.navigationRequested(destination) }
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

        DevicesPage {
            objectName: "devicesPage"
            anchors.fill: parent
            visible: window.currentPage === "devices"
            compact: window.compact
            narrow: window.narrow
            deviceModel: window.deviceModel
            accessState: window.deviceAccess
            refreshAvailable: window.refreshAvailable
            refreshing: window.refreshing
            refreshFeedback: window.refreshFeedback
            refreshFailed: window.refreshFailed
            reduceMotion: window.reduceMotion
            connectionPending: window.connectionPending
            enabledDeviceTools: window.enabledDeviceTools
            onRefreshRequested: window.refreshRequested()
            onConnectRequested: function(deviceId) { window.connectRequested(deviceId) }
            onToolRequested: function(deviceId, action) { window.deviceToolRequested(deviceId, action) }
            onAccountRequested: window.navigationRequested("account")
        }

        Loader {
            anchors.fill: parent
            active: window.currentPage !== "devices"
            sourceComponent: window.destinationPage
        }
    }

    Rectangle {
        objectName: "windowOutline"
        anchors.fill: parent
        visible: window.customWindowChrome
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
