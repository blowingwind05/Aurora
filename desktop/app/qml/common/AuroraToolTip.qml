// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15

QtObject {
    id: request

    property Item anchorItem
    property string text
    property bool active: false
    property bool focused: false
    readonly property bool eligible: (active || focused) && !!anchorItem && anchorItem.visible &&
        !!anchorItem.Window.window && anchorItem.Window.window.visible && text.length > 0

    function dismiss() { AuroraToolTips.release(request) }

    function update() {
        if (eligible)
            AuroraToolTips.offer(request)
        else
            dismiss()
    }

    onEligibleChanged: update()
    onActiveChanged: if (active) update()
    onFocusedChanged: if (focused) update()
    Component.onCompleted: update()
    Component.onDestruction: dismiss()
}
