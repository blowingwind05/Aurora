// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

Item {
    id: slot

    property bool available: true
    property bool pending: false
    property bool reduceMotion: false
    property string message
    property bool failed: false
    property bool retainFailure: false
    property var revision: 0

    property bool expanded: false
    property string displayedMessage
    property bool displayedFailure: false
    readonly property real targetHeight: expanded ? feedbackLine.height + 8 : 0
    property real animatedHeight: targetHeight
    property real animatedOpacity: expanded ? 1 : 0

    height: reduceMotion ? targetHeight : animatedHeight
    clip: true

    function updateResult() {
        expiry.stop()
        if (!available || pending || !message.length) {
            expanded = false
            return
        }
        displayedMessage = message
        displayedFailure = failed
        expanded = true
        if (!failed || !retainFailure)
            expiry.start()
    }

    onAvailableChanged: Qt.callLater(updateResult)
    onPendingChanged: Qt.callLater(updateResult)
    onMessageChanged: Qt.callLater(updateResult)
    onFailedChanged: Qt.callLater(updateResult)
    onRetainFailureChanged: Qt.callLater(updateResult)
    onRevisionChanged: Qt.callLater(updateResult)
    Component.onCompleted: Qt.callLater(updateResult)

    Timer {
        id: expiry
        interval: 3000
        onTriggered: slot.expanded = false
    }

    Behavior on animatedHeight {
        enabled: !slot.reduceMotion
        NumberAnimation {
            duration: 240
            easing.type: Easing.OutCubic
        }
    }

    Behavior on animatedOpacity {
        enabled: !slot.reduceMotion
        SequentialAnimation {
            PauseAnimation { duration: slot.expanded ? 40 : 0 }
            NumberAnimation { duration: 160; easing.type: Easing.InOutQuad }
        }
    }

    Row {
        id: feedbackLine
        width: parent.width
        height: Math.max(18, feedback.implicitHeight)
        spacing: 7
        visible: slot.height > 0
        opacity: slot.reduceMotion ? (slot.expanded ? 1 : 0) : slot.animatedOpacity

        Rectangle {
            width: 6
            height: width
            radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            color: slot.displayedFailure ? "#A13F39" : AuroraTheme.statusBlue
            Accessible.ignored: true
        }
        Text {
            id: feedback
            objectName: "refreshFeedback"
            width: Math.max(0, feedbackLine.width - 13)
            anchors.verticalCenter: parent.verticalCenter
            text: slot.displayedMessage
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: AuroraTheme.font
            font.pixelSize: 12
            color: slot.displayedFailure ? "#A13F39" : AuroraTheme.muted
        }
    }
}
