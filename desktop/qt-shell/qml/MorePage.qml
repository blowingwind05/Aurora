// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: page
    property string section: "about"
    property string versionLabel: ""
    property string feedbackAddress: ""
    property bool compact: false
    property bool narrow: false
    signal linkRequested(string destination)

    readonly property int inset: compact ? 8 : narrow ? 13 : 21

    function reveal(control) {
        const top = control.mapToItem(scroll.contentItem, 0, 0).y - 4
        const bottom = top + control.height + 8
        if (top < scroll.contentY)
            scroll.contentY = Math.max(0, top)
        else if (bottom > scroll.contentY + scroll.height)
            scroll.contentY = Math.min(scroll.contentHeight - scroll.height, bottom - scroll.height)
    }

    component Heading: Text {
        font.family: AuroraTheme.font
        font.pixelSize: 15
        font.weight: Font.DemiBold
        color: AuroraTheme.ink
        Accessible.role: Accessible.Heading
    }

    component Detail: Text {
        font.family: AuroraTheme.font
        font.pixelSize: 12
        color: AuroraTheme.muted
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }

    component Panel: Rectangle {
        color: "white"
        border.color: AuroraTheme.panelLine
        radius: 11
    }

    component ProjectLink: AuroraButton {
        id: link
        property string detail: ""
        property string destination
        property url mark: ""
        property bool card: false
        readonly property bool stacked: !card && availableWidth < 330

        implicitHeight: Math.max(card ? 82 : 33, body.implicitHeight + topPadding + bottomPadding)
        padding: card ? (page.compact ? 10 : 14) : 8
        topPadding: card ? 13 : 5
        bottomPadding: topPadding
        Accessible.name: detail ? text + "，" + detail : text
        onActiveFocusChanged: if (activeFocus) page.reveal(link)
        onClicked: page.linkRequested(destination)

        background: Rectangle {
            radius: link.card ? 11 : 6
            color: link.hovered || link.down ? AuroraTheme.hover : link.card ? "white" : "transparent"
            border.width: link.visualFocus ? 2 : link.card ? 1 : 0
            border.color: link.visualFocus ? AuroraTheme.blue : AuroraTheme.panelLine
        }

        contentItem: RowLayout {
            id: body
            spacing: link.card ? 11 : 8

            Item {
                visible: link.card
                implicitWidth: 24
                implicitHeight: 24
                AuroraGlyph {
                    anchors.centerIn: parent
                    width: 22
                    height: 22
                    icon.source: link.mark
                    icon.color: link.foreground
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: link.card || link.stacked ? 1 : 2
                columnSpacing: 8
                rowSpacing: link.card ? 4 : 2

                Text {
                    text: link.text
                    textFormat: Text.PlainText
                    color: link.foreground
                    font.family: AuroraTheme.font
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    Layout.preferredWidth: link.card || link.stacked ? -1 : 120
                    Layout.fillWidth: link.card || link.stacked
                }

                Detail {
                    text: link.detail
                    visible: text.length > 0
                    Layout.fillWidth: true
                }
            }

            AuroraGlyph {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                width: 16
                height: 16
                rotation: -45
                icon.source: AuroraTheme.icon("arrow-right")
                icon.color: AuroraTheme.muted
            }
        }
    }

    component AboutGroup: Column {
        id: group
        property string heading
        property var entries
        spacing: 6
        Heading { text: group.heading }
        Panel {
            width: parent.width
            height: rows.implicitHeight + 16
            Column {
                id: rows
                x: 8
                y: 8
                width: parent.width - 16
                Repeater {
                    model: group.entries
                    ProjectLink {
                        width: rows.width
                        objectName: "moreLink-" + modelData.id
                        text: modelData.title
                        detail: modelData.detail
                        destination: modelData.id
                    }
                }
            }
        }
    }

    Heading {
        id: title
        objectName: "morePageHeading"
        anchors.top: parent.top
        anchors.topMargin: page.compact ? 16 : 20
        anchors.left: parent.left
        anchors.leftMargin: page.inset
        height: 34
        verticalAlignment: Text.AlignVCenter
        text: page.section === "feedback" ? qsTr("反馈") : qsTr("关于")
        font.pixelSize: page.compact ? 21 : 26
    }

    Flickable {
        id: scroll
        objectName: "moreScroll"
        anchors.top: title.bottom
        anchors.topMargin: 12
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: page.inset
        anchors.rightMargin: page.inset
        contentWidth: width
        contentHeight: bodyLoader.height + 12
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Loader {
            id: bodyLoader
            width: scroll.width
            sourceComponent: page.section === "feedback" ? feedbackPage : aboutPage
        }
    }

    onSectionChanged: scroll.contentY = 0

    Component {
        id: feedbackPage
        GridLayout {
            columns: width < 480 ? 1 : 2
            columnSpacing: 12
            rowSpacing: 12

            ProjectLink {
                objectName: "moreMail"
                card: true
                text: qsTr("邮箱")
                detail: page.feedbackAddress
                destination: "mail"
                mark: AuroraTheme.icon("mail")
                Layout.fillWidth: true
                Layout.preferredWidth: 1
            }
            ProjectLink {
                objectName: "moreIssues"
                card: true
                text: qsTr("GitHub Issues")
                detail: qsTr("公开报告问题或提出功能建议。")
                destination: "issues"
                mark: AuroraTheme.github
                Layout.fillWidth: true
                Layout.preferredWidth: 1
            }
        }
    }

    Component {
        id: aboutPage
        Column {
            spacing: 12

            Panel {
                width: parent.width
                height: overview.implicitHeight + 28

                Column {
                    id: overview
                    x: page.compact ? 12 : 16
                    y: 14
                    width: parent.width - 2 * x
                    spacing: 9

                    Row {
                        width: parent.width
                        spacing: 12
                        Image {
                            id: brand
                            width: 56
                            height: 56
                            source: AuroraTheme.desktopAsset("Aurora.png")
                            fillMode: Image.PreserveAspectFit
                            Accessible.ignored: true
                        }
                        Column {
                            width: parent.width - brand.width - parent.spacing
                            spacing: 3
                            Heading {
                                width: parent.width
                                text: qsTr("Aurora远程桌面")
                                font.pixelSize: 19
                                wrapMode: Text.Wrap
                            }
                            Detail {
                                objectName: "moreVersion"
                                width: parent.width
                                text: page.versionLabel || qsTr("版本未知")
                            }
                        }
                    }
                    Detail {
                        width: parent.width
                        text: qsTr("极光是开源远程桌面应用，可通过局域网或 Tailscale 连接自己的电脑并串流桌面。")
                    }
                }
            }

            AboutGroup {
                width: parent.width
                heading: qsTr("项目信息")
                entries: [
                    { id: "author", title: qsTr("作者"), detail: "blowingwind05" },
                    { id: "source", title: qsTr("开源仓库"), detail: "GitHub" },
                    { id: "license", title: qsTr("开源许可"), detail: "GPL-3.0-only" }
                ]
            }

            AboutGroup {
                width: parent.width
                heading: qsTr("主要组件")
                entries: [
                    { id: "moonlight", title: "Moonlight", detail: qsTr("在主控端播放串流并传送键鼠输入。") },
                    { id: "sunshine", title: "Sunshine", detail: qsTr("在被控端采集、编码并发送桌面画面。") },
                    { id: "tailscale", title: "Tailscale", detail: qsTr("发现同账号设备并建立跨网连接。") }
                ]
            }
        }
    }
}
