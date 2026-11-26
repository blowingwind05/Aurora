// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

Item {
    id: stage

    property alias heading: body.heading
    property alias detail: body.detail
    property alias connecting: body.connecting
    property alias failed: body.failed
    property alias offerQuit: body.offerQuit
    property alias reduceMotion: body.reduceMotion
    property string deviceName
    property int trafficLightsInset: 80

    signal cancelRequested()
    signal retryRequested()
    signal quitPreviousRequested()

    StreamSessionChrome {
        id: chrome
        width: parent.width
        height: barHeight
        deviceName: stage.deviceName
        phase: stage.failed ? StreamSessionChrome.Failed : StreamSessionChrome.Connecting
        brandAtEnd: true
        leadingWindowInset: stage.trafficLightsInset
        reduceMotion: stage.reduceMotion
        canCancel: !stage.failed
        onCancelRequested: stage.cancelRequested()
    }

    ConnectionStage {
        id: body
        anchors { left: parent.left; right: parent.right; top: chrome.bottom; bottom: parent.bottom }
        onRetryRequested: stage.retryRequested()
        onQuitPreviousRequested: stage.quitPreviousRequested()
    }

    Shortcut {
        sequence: "Escape"
        enabled: stage.visible && !stage.failed
        onActivated: stage.cancelRequested()
    }
}
