// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "../../common"

Popup {
    id: dialog
    objectName: "tailscaleConfirmation"
    property var confirmation: null
    property int answeredId: 0
    property Item returnFocusItem: null
    property Item priorFocus: null
    property bool hostBusy: false
    signal decision(int id, bool approved)

    readonly property bool waiting: !!confirmation && confirmation.id === answeredId
    anchors.centerIn: Overlay.overlay
    width: Math.min(430, parent ? parent.width - 24 : 430)
    padding: 20
    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    visible: confirmation !== null

    function respond(approved) {
        if (!confirmation || waiting)
            return
        answeredId = confirmation.id
        decision(confirmation.id, approved)
    }

    onAboutToShow: priorFocus = returnFocusItem
    onOpened: cancel.forceActiveFocus()
    onClosed: if (!hostBusy) restoreFocus()
    onHostBusyChanged: if (!hostBusy && !visible) restoreFocus()

    function restoreFocus() {
        if (priorFocus && priorFocus.visible && priorFocus.enabled)
            priorFocus.forceActiveFocus()
        priorFocus = null
    }

    Overlay.modal: Rectangle { color: "#520F1B2D" }
    background: Rectangle {
        radius: 12
        color: "white"
        border.color: AuroraTheme.panelLine
    }

    contentItem: Column {
        spacing: 12
        Keys.onEscapePressed: dialog.respond(false)

        Text {
            width: parent.width
            text: dialog.confirmation ? dialog.confirmation.title : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AuroraTheme.ink
            font.family: AuroraTheme.font
            font.pixelSize: 18
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }

        Text {
            width: parent.width
            text: dialog.confirmation ? dialog.confirmation.body : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AuroraTheme.muted
            font.family: AuroraTheme.font
            font.pixelSize: 13
        }

        Row {
            anchors.right: parent.right
            spacing: 8

            AuroraButton {
                id: cancel
                objectName: "cancelTailscaleConfirmation"
                text: qsTr("取消")
                width: Math.max(64, implicitWidth)
                enabled: !dialog.waiting
                KeyNavigation.tab: approve
                KeyNavigation.backtab: approve
                onClicked: dialog.respond(false)
            }
            AuroraButton {
                id: approve
                objectName: "approveTailscaleConfirmation"
                readonly property bool destructive: !!dialog.confirmation && dialog.confirmation.destructive
                text: dialog.confirmation ? dialog.confirmation.label : ""
                primary: !destructive
                width: Math.max(76, implicitWidth)
                foreground: destructive ? "#A13F39" : "white"
                enabled: !dialog.waiting
                KeyNavigation.tab: cancel
                KeyNavigation.backtab: cancel
                onClicked: dialog.respond(true)

                background: Rectangle {
                    radius: 7
                    color: approve.destructive ? (approve.hovered ? "#F9DEDE" : "#FCEAEA") : AuroraTheme.blue
                    border.color: AuroraTheme.blue
                    border.width: approve.visualFocus ? 2 : 0
                }
            }
        }
    }
}