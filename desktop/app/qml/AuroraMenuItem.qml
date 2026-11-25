// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

MenuItem {
    id: item
    property int cornerRadius: 6
    height: 32
    padding: 4
    leftPadding: 8
    rightPadding: 8
    spacing: 8
    hoverEnabled: true
    font.family: AuroraTheme.font
    font.pixelSize: 13
    icon.width: 16
    icon.height: 16
    icon.color: enabled ? AuroraTheme.ink : "#8390A1"
    palette.text: icon.color
    palette.buttonText: icon.color
    palette.highlightedText: icon.color

    background: Rectangle {
        radius: item.cornerRadius
        color: item.enabled && (item.hovered || item.down) ? AuroraTheme.hover : "transparent"
        border.width: item.visualFocus ? 2 : 0
        border.color: AuroraTheme.blue
    }
}
