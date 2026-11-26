// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import "../../common"

FocusScope {
    id: card
    required property string deviceId
    required property string displayName
    required property bool online
    required property bool canConnect
    required property string lanAddress
    required property string tailscaleAddress
    required property string wallpaperUrl

    property url defaultWallpaper
    property var enabledTools: []
    property bool connectionPending: false
    property bool reduceMotion: false
    property bool supportsPower: false
    property int deviceEpoch: -1
    signal connectRequested(string deviceId)
    signal toolRequested(string deviceId, string action)
    signal copyRequested(string address)
    signal powerConfirmationRequested(bool restart, int epoch, Item source)

    onDeviceEpochChanged: if (toolsMenu.visible) toolsMenu.close()

    readonly property bool ready: online && canConnect && deviceId !== ""
    readonly property bool powerAvailable: online && supportsPower && deviceId !== "" && deviceEpoch >= 0
    readonly property bool compactTools: width - 2 <= 325
    readonly property bool compactAddress: width - 18 <= 250
    readonly property real cardRadius: 14
    readonly property real coverHeight: Math.max(176, (width - 2) / 1.88)
    height: coverHeight + 47

    Rectangle {
        anchors.fill: parent
        color: "white"
        radius: card.cardRadius
    }

    Item {
        id: cover
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        height: card.coverHeight
        clip: true

        Rectangle {
            anchors.fill: parent
            color: "#446D66"
        }
        Item {
            id: wallpapers
            objectName: "deviceWallpaper-" + card.deviceId
            anchors.fill: parent
            readonly property bool animateHover: card.ready && !card.connectionPending && !card.reduceMotion
            scale: animateHover && connectButton.hovered ? 1.04 : 1
            transformOrigin: Item.Center
            Behavior on scale {
                enabled: wallpapers.animateHover
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                source: cachedWallpaper.status === Image.Ready ? "" : card.defaultWallpaper
                asynchronous: true
            }
            Image {
                id: cachedWallpaper
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                source: card.wallpaperUrl
                asynchronous: true
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: !card.online
            color: Qt.rgba(251 / 255, 251 / 255, 251 / 255, 0.3)
        }
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.26; color: "#000B1122" }
                GradientStop { position: 0.68; color: "#700B1122" }
                GradientStop { position: 1.0; color: "#DB0B1122" }
            }
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: card.compactAddress ? 8 : 14
            anchors.rightMargin: anchors.leftMargin
            anchors.bottomMargin: 12
            spacing: 4

            Text {
                objectName: "deviceName-" + card.deviceId
                width: parent.width
                text: card.displayName
                textFormat: Text.PlainText
                color: "white"
                font.family: AuroraTheme.windows ? "Segoe UI, Microsoft YaHei UI" : AuroraTheme.font
                font.pixelSize: 17
                font.weight: Font.DemiBold
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            Flow {
                width: parent.width
                spacing: card.compactAddress ? 4 : 9

                component Address: Text {
                    width: Math.min(implicitWidth, parent.width)
                    color: "#F3F6FF"
                    font.family: AuroraTheme.font
                    font.pixelSize: card.compactAddress ? 11 : 12
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }

                Address {
                    id: lan
                    visible: card.lanAddress.length > 0
                    text: qsTr("局域网 %1").arg(card.lanAddress)
                }
                Item {
                    visible: !card.compactAddress && lan.visible && tailnet.visible
                        && lan.width + tailnet.width + 2 * parent.spacing + width <= parent.width
                    width: 1
                    height: lan.height
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 1
                        height: 10
                        color: "#70F3F6FF"
                    }
                }
                Address {
                    id: tailnet
                    visible: card.tailscaleAddress.length > 0
                    text: qsTr("Tailscale %1").arg(card.tailscaleAddress)
                }
            }
        }

        AuroraButton {
            id: connectButton
            objectName: "connect-" + card.deviceId
            anchors.fill: parent
            enabled: card.ready && !card.connectionPending
            Accessible.name: qsTr("连接桌面：%1").arg(card.displayName)
            background: null
            onClicked: card.connectRequested(card.deviceId)

            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                visible: connectButton.visualFocus
                color: "transparent"
                border.color: AuroraTheme.blue
                border.width: 2
                radius: 10
            }
        }

        Rectangle {
            id: status
            x: 11
            y: 11
            width: statusText.implicitWidth + 30
            height: 24
            radius: 6
            color: "#F5F7FA"
            Accessible.name: card.ready ? qsTr("在线，可以连接") : card.online
                ? qsTr("在线，未检测到串流") : qsTr("离线")
            Accessible.role: Accessible.StaticText

            Row {
                anchors.centerIn: parent
                spacing: 6
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    radius: 4
                    color: card.ready ? AuroraTheme.statusBlue : "transparent"
                    border.width: card.ready ? 0 : 1.5
                    border.color: card.online ? "#956522" : "#68788D"
                }
                Text {
                    id: statusText
                    text: card.online ? qsTr("在线") : qsTr("离线")
                    color: card.ready ? AuroraTheme.statusBlue : "#5C6878"
                    font.family: AuroraTheme.font
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
            }
            MouseArea {
                id: statusHover
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
            }
            AuroraToolTip {
                anchorItem: status
                text: status.Accessible.name
                active: statusHover.containsMouse
            }
        }
    }

    // Cover the two outer corners of the wallpaper without a shader dependency.
    Repeater {
        model: 2
        Canvas {
            required property int index
            width: card.cardRadius
            height: width
            x: index === 0 ? 0 : card.width - width
            transform: Scale {
                origin.x: card.cardRadius / 2
                xScale: index === 0 ? 1 : -1
            }
            onPaint: {
                const ctx = getContext("2d")
                const radius = card.cardRadius
                ctx.reset()
                ctx.fillStyle = AuroraTheme.workspace
                ctx.beginPath()
                ctx.moveTo(0, 0)
                ctx.lineTo(radius, 0)
                ctx.arc(radius, radius, radius, -Math.PI / 2, -Math.PI, true)
                ctx.closePath()
                ctx.fill()
            }
        }
    }

    Row {
        id: toolbar
        x: 3
        y: cover.y + cover.height + 3
        width: parent.width - 6
        height: 39
        readonly property int moreWidth: card.compactAddress ? 29 : 34
        readonly property real toolWidth: (width - moreWidth) / 4

        Repeater {
            model: [
                { action: "files", label: qsTr("文件传输"), iconName: "folder-sync" },
                { action: "view", label: qsTr("观看模式"), iconName: "square-play" },
                { action: "terminal", label: qsTr("终端"), iconName: "square-terminal" },
                { action: "ports", label: qsTr("端口映射"), iconName: "panels-top-left" }
            ]
            Item {
                required property var modelData
                required property int index
                width: toolbar.toolWidth
                height: toolbar.height
                Rectangle {
                    visible: parent.index > 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1
                    height: 17
                    color: AuroraTheme.divider
                }
                AuroraButton {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                    enabled: card.ready && !card.connectionPending && card.enabledTools.indexOf(parent.modelData.action) >= 0
                    text: parent.modelData.label
                    font.pixelSize: card.compactTools ? 11 : 12
                    spacing: card.compactTools ? 2 : 5
                    display: card.compactTools ? AbstractButton.TextUnderIcon : AbstractButton.TextBesideIcon
                    icon.source: AuroraTheme.icon(parent.modelData.iconName)
                    onClicked: card.toolRequested(card.deviceId, parent.modelData.action)
                }
            }
        }

        Item {
            width: toolbar.moreWidth
            height: toolbar.height
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: 17
                color: AuroraTheme.divider
            }
            AuroraButton {
                id: more
                objectName: "tools-" + card.deviceId
                anchors.fill: parent
                anchors.leftMargin: 2
                text: qsTr("更多工具")
                hint: text
                selected: toolsMenu.opened
                display: AbstractButton.IconOnly
                icon.source: Qt.resolvedUrl("../../../../../docs/visual-design/assets/icons/fluent/ic_fluent_apps_24_regular.svg")
                onClicked: toolsMenu.open()

                AuroraMenu {
                    id: toolsMenu
                    objectName: "deviceMenu-" + card.deviceId
                    x: more.width - width
                    property real anchorTop: 0
                    y: anchorTop >= height + 8 ? -height - 3 : more.height + 3
                    property var powerChoice: null
                    readonly property string powerUnavailable: qsTr("远程电源操作需要被控端在线并支持此功能")

                    function choosePower(restart) {
                        if (!card.powerAvailable)
                            return
                        powerChoice = {
                            restart: restart,
                            deviceId: card.deviceId,
                            displayName: card.displayName,
                            epoch: card.deviceEpoch
                        }
                        close()
                    }

                    onAboutToShow: {
                        powerChoice = null
                        anchorTop = more.mapToItem(null, 0, 0).y
                    }
                    onClosed: {
                        const choice = powerChoice
                        powerChoice = null
                        if (choice && card.powerAvailable && choice.deviceId === card.deviceId &&
                                choice.displayName === card.displayName && choice.epoch === card.deviceEpoch)
                            card.powerConfirmationRequested(choice.restart, choice.epoch, more)
                    }

                    property Item powerHint: MouseArea {
                        id: unavailablePower
                        parent: toolsMenu.contentItem ? toolsMenu.contentItem.parent : null
                        visible: toolsMenu.opened && !card.powerAvailable
                        readonly property point start: restartItem.parent && parent
                            ? restartItem.parent.mapToItem(parent, restartItem.x, restartItem.y) : Qt.point(0, 0)
                        x: start.x
                        y: start.y
                        width: restartItem.width
                        height: restartItem.height + shutdownItem.height
                        z: 1
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                        AuroraToolTip {
                            anchorItem: unavailablePower
                            text: toolsMenu.powerUnavailable
                            active: unavailablePower.containsMouse
                        }
                    }

                    AuroraMenuItem {
                        visible: card.lanAddress.length > 0
                        height: visible ? 32 : 0
                        objectName: "copyLan-" + card.deviceId
                        text: qsTr("复制局域网地址")
                        icon.source: AuroraTheme.icon("copy")
                        onTriggered: card.copyRequested(card.lanAddress)
                    }
                    AuroraMenuItem {
                        visible: card.tailscaleAddress.length > 0
                        height: visible ? 32 : 0
                        objectName: "copyTailnet-" + card.deviceId
                        text: qsTr("复制 Tailscale 地址")
                        icon.source: AuroraTheme.icon("copy")
                        onTriggered: card.copyRequested(card.tailscaleAddress)
                    }
                    MenuSeparator {
                        visible: card.lanAddress.length > 0 || card.tailscaleAddress.length > 0
                        height: visible ? 6 : 0
                        contentItem: Item {}
                    }
                    AuroraMenuItem {
                        id: restartItem
                        objectName: "restart-" + card.deviceId
                        enabled: card.powerAvailable
                        text: qsTr("重启")
                        icon.source: AuroraTheme.icon("rotate-ccw")
                        Accessible.description: enabled ? "" : toolsMenu.powerUnavailable
                        onTriggered: toolsMenu.choosePower(true)
                    }
                    AuroraMenuItem {
                        id: shutdownItem
                        objectName: "shutdown-" + card.deviceId
                        enabled: card.powerAvailable
                        text: qsTr("关机")
                        icon.source: AuroraTheme.icon("power")
                        Accessible.description: enabled ? "" : toolsMenu.powerUnavailable
                        onTriggered: toolsMenu.choosePower(false)
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: card.cardRadius
        color: "transparent"
        border.color: AuroraTheme.panelLine
    }
}
