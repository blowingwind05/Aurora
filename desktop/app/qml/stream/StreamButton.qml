// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import "../common" as Aurora

Aurora.AuroraButton {
    id: button
    property int cornerRadius: 7
    property color idleFill: "transparent"
    property bool showLabel: true
    property bool rowStyle: false
    property url trailingIcon
    spacing: rowStyle ? 9 : 5

    implicitHeight: 32
    focusPolicy: Qt.StrongFocus
    foreground: !enabled ? Aurora.AuroraTheme.disabled
                          : hovered ? Aurora.AuroraTheme.hoverInk : Aurora.AuroraTheme.ink
    Accessible.description: hint === text ? "" : hint

    contentItem: Item {
        readonly property int leading: button.rowStyle ? 9 : 0
        readonly property int trailing: button.rowStyle ? 9 + (button.trailingIcon.toString() ? 16 : 0) : 4
        readonly property int iconSpace: glyph.visible ? glyph.width + (label.visible ? button.spacing : 0) : 0
        implicitWidth: leading + iconSpace + (label.visible ? label.implicitWidth : 0) + trailing
        implicitHeight: 16

        Item {
            id: line
            width: Math.max(0, Math.min(parent.width - parent.leading - parent.trailing,
                                       parent.iconSpace + (label.visible ? label.implicitWidth : 0)))
            x: button.rowStyle ? parent.leading : (parent.width - width) / 2
            height: parent.height

            Aurora.AuroraGlyph {
                id: glyph
                width: button.icon.width
                height: button.icon.height
                anchors.verticalCenter: parent.verticalCenter
                visible: button.icon.source.toString() !== ""
                icon.source: button.icon.source
                icon.color: button.foreground
            }
            StreamText {
                id: label
                x: line.parent.iconSpace
                width: Math.max(0, line.width - x)
                height: parent.height
                visible: button.showLabel
                text: button.showLabel ? button.text : ""
                font: button.font
                color: button.foreground
                elide: Text.ElideRight
                Accessible.ignored: true
            }
        }

        Aurora.AuroraGlyph {
            width: 13
            height: 13
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            visible: button.trailingIcon.toString() !== ""
            icon.source: button.trailingIcon
            icon.color: Aurora.AuroraTheme.muted
        }
    }

    background: Rectangle {
        radius: button.cornerRadius
        color: button.selected ? Aurora.AuroraTheme.selected
             : button.enabled && (button.hovered || button.down) ? button.hoverFill : button.idleFill
        border.width: button.visualFocus ? 2 : 0
        border.color: Aurora.AuroraTheme.blue
    }
}
