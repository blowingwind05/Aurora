// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

Button {
    id: control
    property bool selected: false
    property bool primary: false
    property bool quiet: false
    property string hint: ""
    property color hoverFill: AuroraTheme.hover
    property color foreground: !enabled ? AuroraTheme.disabled : primary ? "white"
        : selected || (quiet && (hovered || visualFocus)) ? AuroraTheme.blue
        : quiet ? AuroraTheme.muted : hovered ? AuroraTheme.hoverInk : AuroraTheme.ink

    flat: false
    hoverEnabled: true
    padding: 0
    leftInset: 0
    rightInset: 0
    topInset: 0
    bottomInset: 0
    implicitWidth: Math.max(34, implicitContentWidth + (primary ? 22 : 0))
    implicitHeight: 34
    spacing: 5
    font.family: AuroraTheme.font
    font.pixelSize: 13
    font.weight: primary ? Font.DemiBold : Font.Normal
    palette.buttonText: foreground
    icon.color: foreground
    icon.width: 16
    icon.height: 16

    Accessible.name: text
    ToolTip.text: hint
    ToolTip.delay: 550
    ToolTip.visible: hint.length > 0 && (hovered || visualFocus)

    background: Rectangle {
        radius: 7
        color: control.primary ? (control.hovered || control.down ? "#076DDA" : AuroraTheme.blue)
            : control.selected ? AuroraTheme.selected
            : control.enabled && (control.hovered || control.down) ? control.hoverFill : "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: AuroraTheme.blue
    }

    Keys.onReturnPressed: control.clicked()
    Keys.onEnterPressed: control.clicked()
}
