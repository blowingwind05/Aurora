// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "stream" as Stream

Item {
    id: sidebar
    property bool compact: false
    property bool hostAvailable: true
    property bool checkingUpdates: false
    property bool reduceMotion: false
    property string currentPage: "devices"
    property string accountName: ""
    property string accountSubtitle: ""
    property url accountAvatar
    signal navigationRequested(string destination)
    signal moreRequested(string action)

    onCheckingUpdatesChanged: if (!checkingUpdates) moreMenu.close()

    readonly property int inset: compact ? 4 : 13
    readonly property int rowHeight: compact ? 38 : 50
    readonly property int rowSpacing: compact ? 4 : 2

    component CompactMenuItem: AuroraMenuItem { cornerRadius: 5 }

    component NavButton: AuroraButton {
        id: button
        property string subtitle: ""
        property bool initialAvatar: false
        width: parent.width
        height: sidebar.rowHeight
        spacing: 0
        font.pixelSize: 14
        font.weight: selected ? Font.DemiBold : Font.Medium
        Accessible.name: subtitle ? text + "，" + subtitle : text
        hint: sidebar.compact ? (subtitle ? text + " · " + subtitle : text) : ""

        contentItem: Item {
            Item {
                id: iconSlot
                width: 24
                height: 24
                x: sidebar.compact ? (parent.width - width) / 2 : 8
                anchors.verticalCenter: parent.verticalCenter
                AuroraGlyph {
                    anchors.centerIn: parent
                    width: button.icon.width
                    height: width
                    icon.source: button.icon.source
                    icon.color: button.foreground
                    visible: !button.initialAvatar
                }
                AuroraAccountAvatar {
                    anchors.fill: parent
                    visible: button.initialAvatar
                    photo: button.initialAvatar ? sidebar.accountAvatar : ""
                    initial: Array.from(button.text)[0] || ""
                    fallbackBackground: "#DCE9F8"
                    fallbackForeground: "#2350B4"
                }
            }
            Column {
                anchors.left: parent.left
                anchors.leftMargin: 42
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: !sidebar.compact
                Text {
                    width: parent.width
                    text: button.text
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font: button.font
                    color: button.foreground
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: button.subtitle
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: AuroraTheme.font
                    font.pixelSize: 11
                    color: button.selected ? button.foreground : AuroraTheme.muted
                }
            }
        }
    }

    Column {
        anchors.top: parent.top
        anchors.topMargin: 13
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: sidebar.inset
        anchors.rightMargin: sidebar.inset
        spacing: sidebar.rowSpacing

        NavButton {
            objectName: "navAccount"
            text: sidebar.accountName || qsTr("Tailscale 账户")
            subtitle: sidebar.accountSubtitle
            initialAvatar: sidebar.accountName.length > 0 || sidebar.accountAvatar.toString().length > 0
            icon.source: AuroraTheme.tailscale
            icon.width: 19
            selected: sidebar.currentPage === "account"
            onClicked: sidebar.navigationRequested("account")
        }
        NavButton {
            objectName: "navDevices"
            text: qsTr("我的设备")
            selected: sidebar.currentPage === "devices"
            icon.source: AuroraTheme.icon("monitor")
            icon.width: 22
            onClicked: sidebar.navigationRequested("devices")
        }
        NavButton {
            objectName: "navHost"
            visible: sidebar.hostAvailable
            text: qsTr("本机被控")
            selected: sidebar.currentPage === "host"
            icon.source: AuroraTheme.icon("panel-top")
            icon.width: 22
            onClicked: sidebar.navigationRequested("host")
        }
    }

    Column {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: sidebar.inset
        spacing: sidebar.rowSpacing

        NavButton {
            objectName: "navSettings"
            text: qsTr("设置")
            selected: sidebar.currentPage === "settings"
            icon.source: AuroraTheme.icon("settings")
            icon.width: 22
            onClicked: sidebar.navigationRequested("settings")
        }
        NavButton {
            id: moreButton
            objectName: "navMore"
            text: qsTr("更多")
            selected: moreMenu.opened || sidebar.currentPage === "feedback" || sidebar.currentPage === "about"
            icon.source: AuroraTheme.icon("menu")
            icon.width: 22
            onClicked: moreMenu.open()

            AuroraMenu {
                id: moreMenu
                panelRadius: 8
                objectName: "sidebarMenu"
                x: moreButton.width + 6
                y: moreButton.height - height
                AuroraButton {
                    id: updateEntry
                    objectName: "checkUpdates"
                    width: moreMenu.availableWidth
                    height: 32
                    leftPadding: 8
                    rightPadding: 8
                    enabled: !sidebar.checkingUpdates
                    text: sidebar.checkingUpdates ? qsTr("正在检查…") : qsTr("检查更新")
                    onClicked: sidebar.moreRequested("checkUpdates")

                    contentItem: Row {
                        spacing: 8
                        Item {
                            width: 16
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            AuroraGlyph {
                                anchors.fill: parent
                                visible: !sidebar.checkingUpdates
                                icon.source: AuroraTheme.icon("refresh-cw")
                            }
                            Stream.StreamRing {
                                objectName: "updateRing"
                                anchors.fill: parent
                                visible: sidebar.checkingUpdates
                                spinning: moreMenu.opened
                                reduceMotion: sidebar.reduceMotion
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: updateEntry.text
                            font: updateEntry.font
                            color: AuroraTheme.ink
                        }
                    }

                    background: Rectangle {
                        radius: 5
                        color: updateEntry.enabled && (updateEntry.hovered || updateEntry.down)
                               ? AuroraTheme.hover : "transparent"
                        border.width: updateEntry.visualFocus ? 2 : 0
                        border.color: AuroraTheme.blue
                    }
                }
                CompactMenuItem {
                    objectName: "moreSourceMenu"
                    text: qsTr("开源地址")
                    icon.source: AuroraTheme.github
                    onTriggered: sidebar.moreRequested("source")
                }
                CompactMenuItem {
                    objectName: "moreHelpMenu"
                    text: qsTr("帮助")
                    icon.source: AuroraTheme.icon("circle-question-mark")
                    onTriggered: sidebar.moreRequested("help")
                }
                CompactMenuItem {
                    objectName: "moreFeedbackMenu"
                    text: qsTr("反馈")
                    icon.source: AuroraTheme.icon("message-square")
                    onTriggered: sidebar.navigationRequested("feedback")
                }
                CompactMenuItem {
                    objectName: "moreAboutMenu"
                    text: qsTr("关于")
                    icon.source: AuroraTheme.icon("info")
                    onTriggered: sidebar.navigationRequested("about")
                }
            }
        }
    }
}
