// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

StreamSessionChrome {
    property bool networkPoor: false
    implicitHeight: barHeight
    phase: StreamSessionChrome.Connected
    networkQuality: networkPoor ? StreamSessionChrome.Poor : StreamSessionChrome.Normal
    nativePanels: true
    displayControls: false
    brandAtEnd: true
    displays: [{ id: "current", name: qsTr("显示器 1") }]
    activeDisplayId: "current"

    function focusNativeControls() {
        forceActiveFocus(Qt.OtherFocusReason)
        focusControls(Qt.TabFocusReason)
    }
}
