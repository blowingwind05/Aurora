// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import "../common" as Aurora

Item {
    id: overlays
    property var owner
    property var session
    property var panel
    readonly property Item panelContent: panelWindow.contentItem
    readonly property Item toolbarContent: toolbarWindow.contentItem
    readonly property Item floatingButton: floating
    readonly property bool edgeHovered: edge.hovered
    readonly property bool windowUsable: !!owner && owner.visible && owner.visibility !== Window.Minimized
    readonly property int floatingInset: 12
    readonly property int floatingSize: 42

    onWindowUsableChanged: if (!windowUsable && session) session.closePanel(false)

    Window {
        id: toolbarWindow
        objectName: "streamToolbarOverlay"
        transientParent: overlays.owner
        flags: Qt.Tool | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint | Qt.WindowDoesNotAcceptFocus
        color: "transparent"
        x: overlays.owner ? overlays.owner.x : 0
        y: overlays.owner ? overlays.owner.y : 0
        width: overlays.session.width
        height: overlays.session.controlsVisible ? overlays.session.barHeight : 2
        visible: overlays.windowUsable && overlays.session.fullScreen
        HoverHandler { id: edge }
    }

    Window {
        id: floatingWindow
        objectName: "streamFloatingOverlay"
        transientParent: overlays.owner
        flags: Qt.Tool | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint | Qt.WindowDoesNotAcceptFocus
        color: "transparent"
        x: overlays.owner ? overlays.owner.x + overlays.session.width - width - overlays.floatingInset : 0
        y: overlays.owner ? overlays.owner.y + overlays.floatingInset : 0
        width: overlays.floatingSize
        height: overlays.floatingSize
        visible: overlays.windowUsable && overlays.session.fullScreen && overlays.session.connected
            && (!overlays.session.controlsVisible || overlays.session.floatingPanel)

        StreamButton {
            id: floating
            objectName: "floatingControlCenter"
            anchors.fill: parent
            text: qsTr("控制中心")
            showLabel: false
            hint: text
            cornerRadius: 13
            idleFill: "#F7F9FD"
            selected: overlays.session.floatingPanel
            icon.source: Aurora.AuroraTheme.iconAsset("aurora/stream-control-center.svg")
            onClicked: overlays.session.togglePanel("controls", floating)
        }
    }

    Window {
        id: panelWindow
        objectName: "streamPanelOverlay"
        transientParent: overlays.owner
        flags: Qt.Popup | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint
        color: "transparent"
        x: overlays.owner && overlays.panel ? overlays.owner.x + overlays.panel.placementLeft : 0
        y: overlays.owner && overlays.panel ? overlays.owner.y + overlays.panel.placementTop : 0
        width: overlays.panel ? Math.max(1, overlays.panel.width) : 1
        height: overlays.panel ? Math.max(1, overlays.panel.height) : 1
        visible: overlays.windowUsable && overlays.session.panelOpen
        onVisibleChanged: {
            if (visible) {
                Qt.callLater(function() {
                    if (panelWindow.visible)
                        overlays.panel.focusInitial()
                })
            } else if (overlays.windowUsable) {
                overlays.session.closePanel(false)
            }
        }
        onClosing: overlays.session.closePanel(false)
    }
}
