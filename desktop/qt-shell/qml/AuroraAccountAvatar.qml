// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

Item {
    id: avatar
    property url photo
    property string initial: ""
    property color fallbackBackground: "#DEECFA"
    property color fallbackForeground: AuroraTheme.statusBlue

    Image {
        id: picture
        objectName: "accountPhoto"
        anchors.fill: parent
        source: avatar.photo
        fillMode: Image.PreserveAspectFit
        visible: status === Image.Ready
    }

    Rectangle {
        anchors.fill: parent
        visible: !picture.visible && avatar.initial.length > 0
        radius: width / 2
        color: avatar.fallbackBackground
        Text {
            anchors.centerIn: parent
            text: avatar.initial
            color: avatar.fallbackForeground
            font.family: AuroraTheme.font
            font.pixelSize: avatar.width <= 24 ? 13 : 18
            font.weight: Font.DemiBold
        }
    }

    AuroraGlyph {
        anchors.centerIn: parent
        visible: !picture.visible && avatar.initial.length === 0
        width: 28
        height: width
        icon.source: AuroraTheme.tailscale
    }
}
