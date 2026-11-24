// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import "../qml/stream" as Stream

Window {
    id: popup
    title: qsTr("Aurora 会话浮层")
    flags: Qt.Tool | Qt.FramelessWindowHint
    color: "transparent"
    width: panel.preferredWidth
    height: Math.max(1, panel.height)
    visible: false

    property bool nativeReady: false
    property bool wasActive: false
    onNativeReadyChanged: if (nativeReady && active) takeFocus()
    function takeFocus() {
        wasActive = true
        panel.focusInitial()
    }

    onActiveChanged: {
        if (!nativeReady)
            return
        if (active)
            takeFocus()
        else if (wasActive)
            Qt.callLater(function() { if (!popup.active) popup.close() })
    }

    QtObject {
        id: session
        readonly property string panelKind: auroraBridge.state.panelKind || "controls"
        readonly property bool fullScreen: auroraBridge.state.fullScreen === true
        readonly property bool canSetFullScreen: !actionPending
        readonly property var connectionMetrics: auroraBridge.state.connectionMetrics || ({})
        readonly property color statusColor: auroraBridge.state.poor ? "#946317" : "#244FC5"
        readonly property var displays: []
        readonly property bool compact: false
        property bool actionPending: false

        function closePanel(restoreFocus) {
            Qt.callLater(function() {
                if (!actionPending)
                    popup.close()
            })
        }

        function fullScreenRequested(enabled) {
            actionPending = true
            auroraBridge.requestFullScreen(enabled)
        }

        function nativeSettingsRequested() {
            actionPending = true
            auroraBridge.send("session-native-settings")
        }
    }

    Stream.StreamPanel {
        id: panel
        nativePopup: true
        heightLimit: auroraBridge.state.heightLimit || 0
        session: session
        visible: heightLimit > 0
    }
}
