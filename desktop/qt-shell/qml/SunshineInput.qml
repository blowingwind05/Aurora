// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

TextField {
    id: field
    implicitHeight: 34
    leftPadding: 8
    rightPadding: 8
    font.family: AuroraTheme.font
    font.pixelSize: 13
    color: enabled ? AuroraTheme.ink : AuroraTheme.muted
    selectionColor: AuroraTheme.blue
    selectByMouse: true
    background: Rectangle {
        radius: 6
        color: field.enabled ? "white" : "#F3F5F8"
        border.color: field.activeFocus ? AuroraTheme.blue : "#CDD7E6"
        border.width: field.activeFocus ? 2 : 1
    }
}
