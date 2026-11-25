// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Popup {
    id: dialog
    property string heading
    property string message
    property string action
    property bool confirmed: false
    signal accepted()
    signal canceled()

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(412, parent.width - 32)
    padding: parent.width <= 420 ? 16 : 22
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    onAboutToShow: confirmed = false
    onOpened: cancel.forceActiveFocus(Qt.TabFocusReason)
    onClosed: {
        if (!confirmed)
            canceled()
    }

    Overlay.modal: Rectangle { color: "#5919283F" }
    background: Rectangle {
        radius: 13
        color: "white"
        border.color: AuroraTheme.panelLine
    }

    contentItem: ColumnLayout {
        spacing: 10
        Text {
            Layout.fillWidth: true
            text: dialog.heading
            wrapMode: Text.Wrap
            color: AuroraTheme.ink
            font.family: AuroraTheme.font
            font.pixelSize: 21
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
        Text {
            Layout.fillWidth: true
            text: dialog.message
            wrapMode: Text.Wrap
            color: AuroraTheme.muted
            font.family: AuroraTheme.font
            font.pixelSize: 13
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: 10
            spacing: 8
            AuroraButton {
                id: cancel
                objectName: dialog.objectName + ":cancel"
                Layout.minimumWidth: 72
                text: qsTr("取消")
                background: Rectangle {
                    radius: 7
                    color: cancel.hovered ? AuroraTheme.hover : "white"
                    border.width: cancel.visualFocus ? 2 : 1
                    border.color: cancel.visualFocus ? AuroraTheme.blue : AuroraTheme.panelLine
                }
                onClicked: dialog.close()
            }
            AuroraButton {
                objectName: dialog.objectName + ":confirm"
                Layout.minimumWidth: 90
                primary: true
                text: dialog.action
                onClicked: {
                    dialog.confirmed = true
                    dialog.close()
                    dialog.accepted()
                }
            }
        }
    }
}
