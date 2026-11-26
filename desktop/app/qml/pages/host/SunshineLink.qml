// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../../common"

AuroraButton {
    id: link
    property bool leadingIcon: false
    quiet: true
    hoverFill: "transparent"
    leftPadding: 0
    rightPadding: 0
    implicitWidth: labelRow.implicitWidth
    contentItem: Row {
        id: labelRow
        layoutDirection: link.leadingIcon ? Qt.RightToLeft : Qt.LeftToRight
        spacing: 5

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: link.text
            font: link.font
            color: link.foreground
        }
        AuroraGlyph {
            anchors.verticalCenter: parent.verticalCenter
            visible: link.icon.source.toString().length > 0
            implicitWidth: 16
            implicitHeight: 16
            width: 16
            height: 16
            icon.source: link.icon.source
            icon.color: link.foreground
        }
    }
}
