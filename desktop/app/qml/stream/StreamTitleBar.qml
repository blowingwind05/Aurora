// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import "../common" as Aurora

FocusScope {
    id: bar
    property var session
    property alias controlsButton: controls
    property alias addButton: add
    readonly property int foot: 11
    readonly property real tabWidth: Math.max(106, Math.min(152,
        (tabBudget - 2 * foot - tabGap * (session.displays.length - 1)) / Math.max(1, session.displays.length)))
    readonly property real tabBudget: Math.max(0, Math.min(context.width - context.fixedSpace - device.width,
        Math.max(132, Math.min(434, width - 536)) - addSlot.width - displayGroup.spacing))
    readonly property int tabGap: 2
    readonly property real tabStripWidth: session.displays.length === 0 ? 0
        : 2 * foot + (tabWidth + tabGap) * session.displays.length - tabGap

    function revealTab(index) {
        if (index < 0 || !tabs.visible)
            return
        var left = index * (tabWidth + tabGap)
        var right = left + 2 * foot + tabWidth
        tabs.contentX = right > tabs.contentX + tabs.width
            ? Math.min(left, right - tabs.width) : Math.min(left, tabs.contentX)
    }

    function revealSelected() {
        for (var i = 0; i < session.displays.length; ++i) {
            if (session.displays[i].id === session.activeDisplayId) {
                revealTab(i)
                return
            }
        }
    }

    function displayEntry() {
        if (session.compact)
            return compactDisplays
        for (var i = 0; i < session.displays.length; ++i) {
            if (session.displays[i].id === session.activeDisplayId)
                return displayTabs.itemAt(i).button
        }
        return null
    }

    Connections {
        target: bar.session
        function onActiveDisplayIdChanged() { Qt.callLater(bar.revealSelected) }
        function onDisplaysChanged() { Qt.callLater(bar.revealSelected) }
    }

    Rectangle {
        anchors.fill: parent
        color: Aurora.AuroraTheme.frame
        radius: bar.session.fullScreen || bar.session.maximized ? 0 : 13

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 14
            color: parent.color
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: bar.session.panelOpen || (!!bar.session.windowControls && !bar.session.fullScreen)
        onPressed: if (bar.session.panelOpen) bar.session.closePanel(false)
        onDoubleClicked: {
            if (bar.session.windowControls && !bar.session.fullScreen)
                bar.session.windowControls.toggleMaximized()
        }
    }
    DragHandler {
        target: null
        enabled: !!bar.session.windowControls && !bar.session.fullScreen && !bar.session.maximized
        onActiveChanged: {
            if (active) {
                bar.session.closePanel(false)
                bar.session.windowControls.startSystemMove()
            }
        }
    }

    HoverHandler { id: barHover }
    readonly property bool pointerInside: barHover.hovered

    Row {
        id: caption
        visible: !!bar.session.windowControls && !bar.session.fullScreen
        anchors.right: parent.right
        anchors.rightMargin: 5
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Repeater {
            model: [
                { glyph: "\uE921", label: qsTr("最小化") },
                { glyph: bar.session.maximized ? "\uE923" : "\uE922",
                  label: bar.session.maximized ? qsTr("还原") : qsTr("最大化") },
                { glyph: "\uE8BB", label: qsTr("关闭") }
            ]

            StreamButton {
                id: captionButton
                width: bar.session.compact ? 28 : 34
                height: 32
                text: modelData.label
                hint: text
                hoverFill: index === 2 ? "#C42B1C" : Aurora.AuroraTheme.hover
                foreground: index === 2 && (hovered || down) ? "white" : Aurora.AuroraTheme.ink
                contentItem: Text {
                    text: modelData.glyph
                    font.family: "Segoe MDL2 Assets"
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    renderType: Text.NativeRendering
                    color: captionButton.foreground
                    Accessible.ignored: true
                }
                onClicked: {
                    if (index === 0)
                        bar.session.windowControls.showMinimized()
                    else if (index === 1)
                        bar.session.windowControls.toggleMaximized()
                    else
                        bar.session.windowControls.close()
                }
            }
        }
    }

    Row {
        id: tools
        anchors.right: parent.right
        anchors.rightMargin: bar.session.fullScreen ? 10 : bar.session.trailingWindowInset + 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        StreamButton {
            objectName: "endSession"
            width: bar.session.tight ? 32 : 58
            height: 32
            visible: bar.session.phase === StreamSessionChrome.Connecting
            enabled: bar.session.canCancel
            text: qsTr("取消")
            showLabel: !bar.session.tight
            hint: bar.session.tight ? text : ""
            hoverFill: "#F6E4E7"
            foreground: !enabled ? Aurora.AuroraTheme.disabled
                : hovered || down ? "#9A3241" : Aurora.AuroraTheme.ink
            icon.source: Aurora.AuroraTheme.icon("x")
            onClicked: bar.session.cancelRequested()
        }

        StreamButton {
            id: controls
            objectName: "controlCenter"
            width: bar.session.compact ? 32 : 94
            height: 32
            text: qsTr("控制中心")
            showLabel: !bar.session.compact
            hint: bar.session.compact ? text : ""
            selected: bar.session.panelKind === "controls"
            enabled: bar.session.connected
            icon.source: Aurora.AuroraTheme.iconAsset("aurora/stream-control-center.svg")
            onClicked: bar.session.togglePanel("controls", controls)
        }

        Image {
            width: 28
            height: 28
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.session.brandAtEnd
            source: bar.session.brandSource
            fillMode: Image.PreserveAspectFit
        }
    }

    Image {
        id: brand
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        width: 28
        height: 28
        visible: !bar.session.brandAtEnd
        source: bar.session.brandSource
        fillMode: Image.PreserveAspectFit
    }

    Row {
        id: context
        anchors.left: parent.left
        anchors.leftMargin: brand.visible ? brand.width + 16
            : bar.session.fullScreen ? 12 : bar.session.leadingWindowInset || 10
        anchors.right: tools.left
        anchors.rightMargin: 8
        height: parent.height
        spacing: bar.session.tight ? 4 : 8

        readonly property int fixedSpace: status.width + addSlot.width + spacing * 2 + displayGroup.spacing
        readonly property int displaySpace: bar.session.compact ? compactDisplays.width
            : (bar.session.displays.length > 0 ? bar.foot * 2 + 106 : 0)

        StreamText {
            id: device
            objectName: "streamDeviceName"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, Math.min(implicitWidth, context.width - context.fixedSpace - context.displaySpace,
                                       bar.session.compact ? context.width : Math.max(58, Math.min(140, bar.width - 680))))
            text: bar.session.deviceName
            font.pixelSize: 14
            font.weight: Font.DemiBold
            elide: Text.ElideRight

            HoverHandler { id: nameHover }
            Aurora.AuroraToolTip {
                anchorItem: device
                text: device.text
                active: nameHover.hovered && device.truncated
            }
        }

        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: bar.session.tight ? 25 : 94
            height: 28

            StreamButton {
                id: status
                objectName: "connectionStatus"
                anchors.fill: parent
                cornerRadius: 6
                text: bar.session.statusText
                enabled: bar.session.connected
                selected: bar.session.panelKind === "connection"
                Accessible.name: bar.session.connected ? qsTr("%1，查看连接详情").arg(text) : text
                hint: bar.session.tight ? text : ""
                onClicked: bar.session.togglePanel("connection", status)

                contentItem: Row {
                    spacing: 6
                    leftPadding: bar.session.tight ? (width - 10) / 2 : 6

                    Item {
                        width: bar.session.phase === StreamSessionChrome.Connecting || bar.session.tight ? 10 : 6
                        height: 10
                        anchors.verticalCenter: parent.verticalCenter
                        StreamRing {
                            anchors.fill: parent
                            visible: bar.session.phase === StreamSessionChrome.Connecting
                            spinning: true
                            reduceMotion: bar.session.reduceMotion
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 6
                            height: 6
                            radius: 3
                            visible: bar.session.phase !== StreamSessionChrome.Connecting
                            color: bar.session.statusColor
                        }
                    }
                    StreamText {
                        visible: !bar.session.tight
                        anchors.verticalCenter: parent.verticalCenter
                        font.pixelSize: 12
                        color: bar.session.phase === StreamSessionChrome.Failed
                            || bar.session.networkQuality !== StreamSessionChrome.Normal && bar.session.connected
                            ? bar.session.statusColor : Aurora.AuroraTheme.muted
                        text: status.text
                        Accessible.ignored: true
                    }
                    Aurora.AuroraGlyph {
                        objectName: "connectionDetailsChevron"
                        width: 14
                        height: 14
                        anchors.verticalCenter: parent.verticalCenter
                        visible: bar.session.connected && !bar.session.tight
                        rotation: status.selected ? 180 : 0
                        icon.source: Aurora.AuroraTheme.icon("chevron-down")
                        icon.color: Aurora.AuroraTheme.muted
                    }
                }
            }

            Item {
                id: inactiveStatus
                anchors.fill: parent
                visible: bar.session.tight && !bar.session.connected

                HoverHandler { id: inactiveStatusHover }
                Aurora.AuroraToolTip {
                    anchorItem: inactiveStatus
                    text: bar.session.statusText
                    active: inactiveStatusHover.hovered
                }
            }
        }

        Row {
            id: displayGroup
            height: bar.height
            spacing: 3

            StreamButton {
                id: compactDisplays
                objectName: "compactDisplays"
                anchors.verticalCenter: parent.verticalCenter
                visible: bar.session.compact
                width: bar.session.tight ? 25 : 28
                height: 28
                text: qsTr("显示器列表与详情")
                showLabel: false
                hint: text
                icon.width: 14
                icon.height: 14
                selected: bar.session.panelKind === "displays"
                enabled: bar.session.connected && bar.session.displays.length > 0
                icon.source: Aurora.AuroraTheme.icon("monitor")
                onClicked: bar.session.togglePanel("displays", compactDisplays)
            }

            Flickable {
                id: tabs
                objectName: "displayStrip"
                visible: !bar.session.compact && bar.tabStripWidth > 0
                width: Math.min(bar.tabStripWidth, bar.tabBudget)
                height: bar.height
                contentWidth: bar.tabStripWidth
                contentHeight: height
                clip: true
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                onWidthChanged: Qt.callLater(bar.revealSelected)
                Behavior on width {
                    enabled: tabs.visible && !bar.session.reduceMotion
                        && tabs.width < bar.tabBudget
                    NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                }

                Row {
                    id: tabsRow
                    x: bar.foot
                    anchors.bottom: parent.bottom
                    spacing: bar.tabGap
                    Repeater {
                        id: displayTabs
                        model: bar.session.displays

                        StreamDisplayTab {
                            id: tab
                            width: bar.tabWidth
                            readonly property var nextTab: tabsRow.children.length > index + 1 ? displayTabs.itemAt(index + 1) : null
                            display: modelData
                            selected: display.id === bar.session.activeDisplayId
                            sessionActive: bar.session.connected
                            detailsOpen: selected && bar.session.panelKind === "displays"
                            showDivider: !selected && !hovered
                                && (!nextTab || (!nextTab.selected && !nextTab.hovered))
                            onActivated: {
                                if (selected)
                                    bar.session.togglePanel("displays", button)
                                else {
                                    bar.session.closePanel(false)
                                    bar.session.displayRequested(display.id)
                                }
                            }
                            onRemovalRequested: bar.session.removeDisplayRequested(display.id)
                            Connections {
                                target: tab.button
                                function onActiveFocusChanged() {
                                    if (tab.button.activeFocus)
                                        bar.revealTab(index)
                                }
                            }
                        }
                    }
                }

                WheelHandler {
                    target: null
                    enabled: tabs.contentWidth > tabs.width
                    onWheel: function(wheel) {
                        var delta = wheel.pixelDelta.x || wheel.pixelDelta.y
                            || wheel.angleDelta.x || wheel.angleDelta.y
                        tabs.contentX = Math.max(0, Math.min(tabs.contentWidth - tabs.width, tabs.contentX - delta))
                        wheel.accepted = true
                    }
                }
            }

            Item {
                id: addSlot
                width: bar.session.tight ? 25 : 28
                height: bar.height

                StreamButton {
                    id: add
                    objectName: "addDisplay"
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: bar.session.compact ? 8 : 4.5
                    width: parent.width
                    height: 28
                    cornerRadius: 6
                    enabled: bar.session.connected && bar.session.canCreateDisplay && !bar.session.creatingDisplay
                    text: qsTr("新增虚拟显示器")
                    showLabel: false
                    hint: text
                    icon.source: bar.session.creatingDisplay ? "" : Qt.resolvedUrl("icons/plus.svg")
                    onClicked: {
                        bar.session.closePanel(false)
                        bar.session.createDisplayRequested()
                    }

                    StreamRing {
                        width: 15
                        height: 15
                        anchors.centerIn: parent
                        visible: bar.session.creatingDisplay
                        spinning: true
                        reduceMotion: bar.session.reduceMotion
                    }
                }

                MouseArea {
                    id: unavailableDisplay
                    anchors.fill: add
                    acceptedButtons: Qt.NoButton
                    hoverEnabled: !add.enabled
                    enabled: !add.enabled
                    Aurora.AuroraToolTip {
                        anchorItem: add
                        active: unavailableDisplay.containsMouse
                        text: bar.session.creatingDisplay ? qsTr("正在新增虚拟显示器…")
                            : bar.session.createDisplayUnavailableReason || qsTr("当前无法新增虚拟显示器")
                    }
                }
            }
        }

    }

    Keys.onEscapePressed: function(event) {
        if (bar.session.fullScreen && bar.session.canSetFullScreen)
            bar.session.fullScreenRequested(false)
        else
            event.accepted = false
    }
}
