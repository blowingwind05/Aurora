// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../common" as Aurora

Item {
    id: frame
    property alias source: picture.source
    property int radius
    property int decodeWidth: 2048
    property color backdrop: "white"
    property string placeholder
    property bool showBorder: false
    readonly property bool ready: picture.status === Image.Ready

    Rectangle {
        anchors.fill: parent
        radius: frame.radius
        color: Aurora.AuroraTheme.workspace
    }

    Image {
        id: picture
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        clip: true
        asynchronous: true
        sourceSize.width: frame.decodeWidth
        sourceSize.height: frame.decodeWidth
        Accessible.ignored: true
    }

    StreamText {
        anchors.fill: parent
        visible: !frame.ready && frame.placeholder.length > 0
        text: frame.placeholder
        horizontalAlignment: Text.AlignHCenter
        color: Aurora.AuroraTheme.muted
    }

    // Paint the outside corners so this also works with the software renderer.
    Canvas {
        id: corners
        anchors.fill: parent
        visible: frame.ready && frame.radius > 0
        readonly property color backdrop: frame.backdrop
        readonly property int radius: frame.radius
        onBackdropChanged: requestPaint()
        onRadiusChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onVisibleChanged: requestPaint()
        onPaint: function() {
            const painter = getContext("2d")
            const r = Math.min(radius, width / 2, height / 2)
            painter.clearRect(0, 0, width, height)
            painter.fillStyle = backdrop
            for (let i = 0; i < 4; ++i) {
                const right = i % 2
                const bottom = i >= 2
                painter.save()
                painter.translate(right ? width : 0, bottom ? height : 0)
                painter.scale(right ? -1 : 1, bottom ? -1 : 1)
                painter.beginPath()
                painter.moveTo(0, 0)
                painter.arc(r, r, r, -Math.PI / 2, -Math.PI, true)
                painter.closePath()
                painter.fill()
                painter.restore()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: frame.showBorder
        radius: frame.radius
        color: "transparent"
        border.color: Aurora.AuroraTheme.panelLine
    }
}
