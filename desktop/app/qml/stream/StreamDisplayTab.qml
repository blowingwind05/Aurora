// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import ".." as Aurora

Item {
    id: tab

    property var display
    property bool selected: false
    property bool detailsOpen: false
    property bool sessionActive: false
    property bool showDivider: false
    readonly property bool hovered: hover.hovered || activate.hovered || closeDisplay.hovered
    readonly property alias button: activate
    signal activated()
    signal removalRequested()

    width: 126
    height: 37

    HoverHandler { id: hover }

    Canvas {
        id: silhouette
        x: tab.selected ? -11 : 0
        width: tab.width - 2 * x
        height: parent.height
        visible: tab.selected || tab.hovered
        onWidthChanged: requestPaint()
        onVisibleChanged: requestPaint()
        Connections {
            target: tab
            function onSelectedChanged() { silhouette.requestPaint() }
        }
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            var left = tab.selected ? 11 : 0
            var right = width - left
            ctx.beginPath()
            ctx.moveTo(0, height)
            if (tab.selected)
                ctx.arc(0, height - 11, 11, Math.PI / 2, 0, true)
            ctx.lineTo(left, 10)
            ctx.arc(left + 10, 10, 10, Math.PI, 1.5 * Math.PI, false)
            ctx.lineTo(right - 10, 0)
            ctx.arc(right - 10, 10, 10, -Math.PI / 2, 0, false)
            ctx.lineTo(right, height - (tab.selected ? 11 : 0))
            if (tab.selected)
                ctx.arc(width, height - 11, 11, Math.PI, Math.PI / 2, true)
            ctx.closePath()
            ctx.fillStyle = tab.selected ? "white" : Aurora.AuroraTheme.hover
            ctx.fill()
        }
    }

    Rectangle {
        x: parent.width + 0.5
        anchors.verticalCenter: parent.verticalCenter
        visible: tab.showDivider
        color: Aurora.AuroraTheme.frameLine
        width: 1
        height: 14
    }

    StreamButton {
        id: activate
        objectName: "displayTab:" + tab.display.id
        anchors.left: parent.left
        anchors.right: closeDisplay.visible ? closeDisplay.left : parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 27
        enabled: tab.sessionActive && (tab.selected || tab.display.ready === true)
        text: tab.display.shortName || tab.display.name
        hint: tab.display.name
        Accessible.name: tab.display.name
        Accessible.selected: tab.selected
        background: Rectangle {
            color: "transparent"
            radius: 6
            border.width: activate.visualFocus ? 2 : 0
            border.color: Aurora.AuroraTheme.blue
        }
        onClicked: tab.activated()

        contentItem: Row {
            leftPadding: 10
            spacing: 5

            Aurora.AuroraGlyph {
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                icon.source: Aurora.AuroraTheme.icon("monitor")
                icon.color: title.color
            }
            StreamText {
                id: title
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, Math.min(implicitWidth, parent.width - (tab.selected ? 47 : 30)))
                font.pixelSize: 12
                elide: Text.ElideRight
                text: activate.text
                color: !activate.enabled ? Aurora.AuroraTheme.disabled
                     : tab.selected ? Aurora.AuroraTheme.ink : Aurora.AuroraTheme.muted
                Accessible.ignored: true
            }
            Aurora.AuroraGlyph {
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                visible: tab.selected
                rotation: tab.detailsOpen ? 180 : 0
                icon.source: Aurora.AuroraTheme.icon("chevron-down")
            }
        }
    }

    StreamButton {
        id: closeDisplay
        objectName: "closeTab:" + tab.display.id
        anchors.right: parent.right
        anchors.rightMargin: 5
        anchors.verticalCenter: parent.verticalCenter
        width: 22
        height: 22
        visible: tab.display.removable === true
        enabled: tab.sessionActive && tab.display.removalPending !== true
        cornerRadius: 5
        text: qsTr("关闭 %1").arg(tab.display.name)
        showLabel: false
        hint: text
        icon.source: Aurora.AuroraTheme.icon("x")
        icon.width: 13
        icon.height: 13
        onClicked: tab.removalRequested()
    }
}
