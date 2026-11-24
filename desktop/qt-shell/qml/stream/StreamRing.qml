// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import ".." as Aurora

Item {
    id: ring
    property bool spinning: false
    property bool reduceMotion: false

    Canvas {
        id: arc
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.lineWidth = 1.5
            ctx.lineCap = "round"
            ctx.strokeStyle = Aurora.AuroraTheme.blue
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, Math.min(width, height) / 2 - 1,
                    -Math.PI / 2, Math.PI, false)
            ctx.stroke()
        }

        RotationAnimator on rotation {
            from: 0
            to: 360
            duration: 850
            loops: Animation.Infinite
            running: ring.visible && ring.spinning && !ring.reduceMotion
        }
    }
}
