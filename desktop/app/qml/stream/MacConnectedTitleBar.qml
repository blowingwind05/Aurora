// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

StreamSessionChrome {
    property bool networkPoor: false
    implicitHeight: barHeight
    phase: StreamSessionChrome.Connected
    networkQuality: networkPoor ? StreamSessionChrome.Poor : StreamSessionChrome.Normal
    nativePanels: true
    property var displaySnapshot: ({})
    property bool displayRequestBusy: false
    displayControls: !!displaySnapshot.displays
    brandAtEnd: true
    displays: !displayControls ? [{ id: "current", name: qsTr("显示器 1") }]
        : displaySnapshot.displays.map(function(entry) {
            return { id: entry.id, name: entry.name, ready: entry.selectable && displaySnapshot.canSelect === true && !displayRequestBusy,
                aspectBlocked: entry.aspectBlocked,
                kind: entry.kind === "virtual" ? qsTr("虚拟显示器") : qsTr("系统显示器"),
                resolution: entry.resolution, refreshRate: entry.refreshRate,
                removable: entry.removable, removalPending: displayRequestBusy }
        })
    activeDisplayId: displaySnapshot.activeDisplayId || "current"
    canCreateDisplay: displaySnapshot.canCreate === true && !displayRequestBusy

    function focusNativeControls() {
        forceActiveFocus(Qt.OtherFocusReason)
        focusControls(Qt.TabFocusReason)
    }
}
