// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

Popup {
    id: menu
    objectName: "tailscaleProfileMenu"
    property var profiles: []
    property int maximumHeight: 260
    signal profileRequested(string key)

    focus: true
    padding: 5
    margins: 6
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    height: Math.min(maximumHeight, contentItem.implicitHeight + topPadding + bottomPadding)

    onOpened: {
        let selected = 0
        for (let i = 0; i < profiles.length; ++i) {
            if (profiles[i].current) {
                selected = i
                break
            }
        }
        options.currentIndex = selected
        options.forceActiveFocus()
    }

    function choose(index) {
        if (index < 0 || index >= profiles.length)
            return
        const profile = profiles[index]
        close()
        if (!profile.current)
            profileRequested(profile.key)
    }

    background: Rectangle {
        radius: 7
        color: "white"
        border.color: AuroraTheme.panelLine
    }

    contentItem: ListView {
        id: options
        implicitHeight: contentHeight
        clip: true
        highlightMoveDuration: 0
        model: menu.profiles
        ScrollBar.vertical: ScrollBar { width: 6 }
        Keys.onReturnPressed: menu.choose(currentIndex)
        Keys.onEnterPressed: menu.choose(currentIndex)

        delegate: ItemDelegate {
            id: option
            required property int index
            required property var modelData
            objectName: "tailscaleProfile-" + modelData.key
            width: options.width
            leftPadding: 8
            rightPadding: 24
            topPadding: 6
            bottomPadding: 6
            text: modelData.name
            hoverEnabled: true
            highlighted: options.activeFocus && ListView.isCurrentItem
            onClicked: menu.choose(index)

            contentItem: Column {
                spacing: 3
                Text {
                    width: parent.width
                    text: option.text
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: AuroraTheme.font
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: AuroraTheme.ink
                }
                Text {
                    width: parent.width
                    readonly property var profile: option.modelData
                    text: [profile.loginName, profile.tailnet].filter(function(value) { return value }).join(" · ")
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: AuroraTheme.font
                    font.pixelSize: 12
                    color: AuroraTheme.muted
                }
            }
            AuroraGlyph {
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                visible: option.modelData.current
                icon.source: Qt.resolvedUrl("../../../docs/visual-design/assets/icons/phosphor/check-bold.svg")
                icon.color: AuroraTheme.statusBlue
            }

            background: Rectangle {
                radius: 5
                color: option.modelData.current ? AuroraTheme.selected
                    : option.highlighted || option.hovered ? AuroraTheme.hover : "transparent"
            }
        }

        footer: Text {
            width: options.width
            leftPadding: 8
            rightPadding: 8
            topPadding: 8
            bottomPadding: 6
            text: qsTr("切换也会更改系统 Tailscale 的当前账号。")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: AuroraTheme.font
            font.pixelSize: 12
            color: AuroraTheme.muted
        }
    }
}