// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

StreamPanel {
    id: menu

    nativePopup: true
    nativeSettingPages: true
    heightLimit: 430

    signal toggleFullscreenRequested()
    signal disconnectRequested()

    session: QtObject {
        readonly property string panelKind: "controls"
        readonly property bool floatingPanel: false
        readonly property bool tight: false
        readonly property int barHeight: 0
        readonly property int width: menu.width
        readonly property Item panelAnchor: null
        readonly property int panelFocusReason: Qt.MouseFocusReason
        readonly property bool fullScreen: false
        readonly property bool canSetFullScreen: true
        readonly property bool canDisconnect: true

        // The parent panel owns dismissal and focus restoration.
        function closePanel(restoreFocus) {}
        function fullScreenRequested(enabled) { menu.toggleFullscreenRequested() }
        function disconnectRequested() { menu.disconnectRequested() }
    }
}
