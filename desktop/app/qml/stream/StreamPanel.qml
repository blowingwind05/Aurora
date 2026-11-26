// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "../common" as Aurora

FocusScope {
    id: panel
    objectName: "streamPanel"
    property var session
    property bool nativePopup: false
    property bool nativeSettingPages: false
    signal nativeSettingPageRequested(string page)
    property bool overlayWindow: false
    property int heightLimit: session.height - placementTop - gutter
    readonly property bool controls: session.panelKind === "controls"
    readonly property int gutter: 8
    readonly property int preferredWidth: controls ? 288 : session.panelKind === "connection" ? 292 : 248

    readonly property int placementTop: session.floatingPanel
        ? session.overlays.floatingInset + session.overlays.floatingSize + 6
        : session.barHeight + (controls || session.tight ? 2 : 3)
    readonly property real placementLeft: {
        var anchor = session.panelAnchor
        if (session.floatingPanel)
            return session.width - width - session.overlays.floatingInset
        if (controls && session.width < 530)
            return session.width - width - gutter
        if (!anchor || (!controls && session.tight) || (session.compact && session.panelKind === "displays"))
            return gutter
        var origin = anchor.mapToItem(session, 0, 0).x
        return Math.max(gutter, Math.min(session.width - width - gutter,
                                        controls ? origin + anchor.width - width : origin - (session.panelKind === "displays" ? 11 : 0)))
    }

    y: nativePopup || overlayWindow ? 0 : placementTop
    x: nativePopup || overlayWindow ? 0 : placementLeft
    width: nativePopup ? preferredWidth : Math.max(0, Math.min(preferredWidth, session.width - 2 * gutter))
    height: Math.max(0, Math.min(430, heightLimit, scroller.contentHeight + 12))

    function layoutNativeHome() {
        if (body.item)
            body.item.layoutNativeHome()
    }

    function focusInitial(reason) {
        if (!visible || !body.item)
            return
        var focusReason = reason === undefined ? session.panelFocusReason : reason
        scroller.contentY = 0
        if (controls)
            body.item.focusInitial(focusReason)
        else if (!body.item.focusInitial(focusReason))
            forceActiveFocus(focusReason)
    }

    function containsItem(item) {
        for (var current = item; current; current = current.parent) {
            if (current === panel)
                return true
        }
        return false
    }

    function keepFocusVisible(item) {
        if (!visible || !item || item === panel || !containsItem(item))
            return
        var top = item.mapToItem(scroller.contentItem, 0, 0).y
        var bottom = top + item.height
        if (top < scroller.contentY)
            scroller.contentY = top
        else if (bottom > scroller.contentY + scroller.height)
            scroller.contentY = bottom - scroller.height
    }

    function stepFocus(forward) {
        var current = Window.window.activeFocusItem || panel
        var next = current.nextItemInFocusChain(forward)
        while (next && next !== current) {
            if (containsItem(next) && next !== panel) {
                next.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                return
            }
            next = next.nextItemInFocusChain(forward)
        }
    }

    Keys.onEscapePressed: {
        if (!(controls && body.item && body.item.goBack(Qt.TabFocusReason)))
            session.closePanel(true)
    }
    Shortcut {
        sequence: "Tab"
        enabled: panel.visible
        onActivated: panel.stepFocus(true)
    }
    Shortcut {
        sequence: "Shift+Tab"
        enabled: panel.visible
        onActivated: panel.stepFocus(false)
    }

    Connections {
        target: panel.Window.window
        function onActiveFocusItemChanged() {
            panel.keepFocusVisible(panel.Window.window.activeFocusItem)
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 9
        color: "white"
        border.color: Aurora.AuroraTheme.panelLine
    }
    MouseArea { anchors.fill: parent }
    Flickable {
        id: scroller
        objectName: "streamPanelScroll"
        anchors.fill: parent
        anchors.margins: 6
        contentWidth: width
        contentHeight: body.item ? body.item.implicitHeight : 0
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Loader {
            id: body
            active: panel.visible
            width: parent.width
            sourceComponent: panel.controls ? controlCenter : detailsBody
        }
    }

    Component {
        id: controlCenter
        StreamControlCenter {
            session: panel.session
            showSettingPages: !panel.nativePopup || panel.nativeSettingPages
            offerNativeSettings: panel.nativePopup && !panel.nativeSettingPages
            nativeSettingPages: panel.nativeSettingPages
            onNativeSettingPageRequested: function(page) { panel.nativeSettingPageRequested(page) }
            onNativeSettingsRequested: panel.session.nativeSettingsRequested()
            onPageChangedByUser: function(reason) { panel.focusInitial(reason) }
        }
    }
    Component {
        id: detailsBody
        StreamDetails {
            session: panel.session
            connection: panel.session.panelKind === "connection"
        }
    }
}
