// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import ".." as Aurora

FocusScope {
    id: panel
    objectName: "streamPanel"
    property var session
    property bool nativePopup: false
    property int heightLimit: parent.height - y - gutter
    readonly property bool controls: session.panelKind === "controls"
    readonly property int gutter: 8
    readonly property int preferredWidth: controls ? 288 : session.panelKind === "connection" ? 292 : 248

    y: nativePopup ? 0 : session.barHeight + (controls || session.tight ? 2 : 3)
    x: {
        if (nativePopup)
            return 0
        var anchor = session.panelAnchor
        if (controls && parent.width < 530)
            return parent.width - width - gutter
        if (!anchor || (!controls && session.tight) || (session.compact && session.panelKind === "displays"))
            return gutter
        var origin = 0
        for (var item = anchor; item && item !== parent; item = item.parent)
            origin += item.x
        return Math.max(gutter, Math.min(parent.width - width - gutter,
                                        controls ? origin + anchor.width - width : origin - (session.panelKind === "displays" ? 11 : 0)))
    }
    width: nativePopup ? preferredWidth : Math.max(0, Math.min(preferredWidth, parent.width - 2 * gutter))
    height: Math.max(0, Math.min(430, heightLimit, scroller.contentHeight + 12))

    function focusInitial() {
        if (!visible || !body.item)
            return
        scroller.contentY = 0
        if (controls)
            body.item.focusInitial()
        else if (!body.item.focusInitial())
            forceActiveFocus(Qt.TabFocusReason)
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
        if (!(controls && body.item.goBack()))
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
        border.color: panel.activeFocus && panel.Window.window.activeFocusItem === panel
                      ? Aurora.AuroraTheme.blue : Aurora.AuroraTheme.panelLine
    }
    MouseArea { anchors.fill: parent }
    Flickable {
        id: scroller
        objectName: "streamPanelScroll"
        anchors.fill: parent
        anchors.margins: 6
        contentWidth: width
        contentHeight: body.height + (panel.controls ? 7 : 11)
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
            showSettingPages: !panel.nativePopup
            offerNativeSettings: panel.nativePopup
            onNativeSettingsRequested: panel.session.nativeSettingsRequested()
            onPageChangedByUser: panel.focusInitial()
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
