// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../../desktop/qt-shell/bridge" as Bridge

Bridge.WindowsSessionPopup {
    id: window

    function findText(item, text) {
        if (item.text === text)
            return item
        for (var i = 0; i < item.children.length; ++i) {
            var found = findText(item.children[i], text)
            if (found)
                return found
        }
        return null
    }

    Timer {
        interval: 50
        repeat: true
        running: window.visible && auroraBridge.state.panelKind === "connection"
        property string printed: ""
        onTriggered: {
            var expected = auroraBridge.state.connectionMetrics.latency
            var metric = expected ? window.findText(window.contentItem, expected) : null
            if (metric && metric.visible && metric.text !== printed) {
                printed = metric.text
                console.warn("SESSION_REPLAY_LATENCY " + printed)
            }
        }
    }
}
