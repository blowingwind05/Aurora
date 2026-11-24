// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

Menu {
    id: menu
    property int panelRadius: 9
    padding: 5
    margins: 6
    width: 180

    background: Rectangle {
        radius: menu.panelRadius
        color: "white"
        border.color: AuroraTheme.panelLine
    }
}
