// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

AuroraButton {
    id: button

    property bool available: true
    property bool refreshing: false
    property bool reduceMotion: false
    property string idleLabel
    property string busyLabel
    property string lastFailure
    readonly property bool showingProgress: refreshing || (!reduceMotion && minimumProgress.running)
    signal refreshRequested()

    width: 34
    height: 34
    quiet: true
    hoverFill: "transparent"
    enabled: available && !showingProgress
    text: showingProgress ? busyLabel : idleLabel
    hint: !showingProgress && lastFailure.length ? text + "\n" + lastFailure : text
    Accessible.description: showingProgress ? "" : lastFailure
    display: AbstractButton.IconOnly
    icon.width: 20
    icon.height: 20
    icon.source: showingProgress ? "" : AuroraTheme.icon("refresh-cw")

    onClicked: if (enabled) {
        if (!reduceMotion)
            minimumProgress.restart()
        refreshRequested()
    }
    onRefreshingChanged: if (refreshing && !reduceMotion) minimumProgress.restart()
    onReduceMotionChanged: if (reduceMotion) minimumProgress.stop()

    Timer {
        id: minimumProgress
        interval: 200
    }

    Canvas {
        id: ring
        objectName: "refreshRing"
        anchors.centerIn: parent
        width: 18
        height: width
        visible: button.showingProgress
        Accessible.ignored: true

        onPaint: {
            var brush = getContext("2d")
            brush.clearRect(0, 0, width, height)
            brush.strokeStyle = AuroraTheme.blue
            brush.lineWidth = 2
            brush.lineCap = "round"
            brush.beginPath()
            brush.arc(width / 2, height / 2, 7, -Math.PI / 2, Math.PI)
            brush.stroke()
        }

        RotationAnimator on rotation {
            from: 0
            to: 360
            duration: 800
            loops: Animation.Infinite
            running: ring.visible && !button.reduceMotion
        }
    }
}
