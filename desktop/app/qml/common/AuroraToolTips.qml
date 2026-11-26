// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick 2.15
import QtQuick.Window 2.15

QtObject {
    id: manager

    property var request: null
    property Window presenter: null
    property bool showing: false
    property var colors: ({})
    readonly property Item anchorItem: request ? request.anchorItem : null
    readonly property Window owner: anchorItem ? anchorItem.Window.window : null
    readonly property string message: request ? request.text : ""
    readonly property var environment: typeof auroraToolTipEnvironment === "undefined" ? null : auroraToolTipEnvironment

    function offer(source) {
        if (request === source)
            return
        dismiss()
        request = source
        delay.start()
    }

    function release(source) {
        if (source === request)
            dismiss()
    }

    function dismiss() {
        delay.stop()
        showing = false
        if (presenter) {
            presenter.destroy()
            presenter = null
        }
        request = null
    }

    property Connections windowChanges: Connections {
        target: manager.owner
        function onXChanged() { manager.dismiss() }
        function onYChanged() { manager.dismiss() }
        function onWidthChanged() { manager.dismiss() }
        function onHeightChanged() { manager.dismiss() }
        function onActiveChanged() { if (manager.owner && !manager.owner.active) manager.dismiss() }
    }

    property Connections input: Connections {
        target: manager.environment
        function onDismissRequested() { manager.dismiss() }
    }

    property Timer delay: Timer {
        interval: 550
        onTriggered: {
            if (!manager.owner || !manager.anchorItem || !manager.message.length)
                return
            manager.colors = manager.environment ? manager.environment.colors() : ({})
            if (!manager.presenter)
                manager.presenter = factory.createObject(manager)
            manager.showing = !!manager.presenter
        }
    }

    property Component factory: Component {
        Window {
            id: tip
            objectName: "auroraToolTipWindow"
            transientParent: manager.owner
            visible: manager.showing && !!manager.owner
            flags: Qt.ToolTip | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint |
                   Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus
            color: "transparent"

            readonly property bool systemColors: !!manager.colors.background
            readonly property int shadow: systemColors ? 0 : 6
            readonly property int edge: 4 + shadow
            readonly property int gap: 6
            readonly property real panelWidth: Math.ceil(Math.min(240,
                Math.max(1, (manager.owner ? manager.owner.width : 240) - 2 * edge), measure.implicitWidth + 16))
            readonly property real panelHeight: Math.max(18, label.implicitHeight) + 8
            readonly property point anchorOrigin: {
                const item = manager.anchorItem
                if (!item || !manager.owner)
                    return Qt.point(0, 0)
                return item.mapToItem(manager.owner.contentItem, 0, 0)
            }
            readonly property real aboveRoom: anchorOrigin.y - gap - edge
            readonly property real belowRoom: (manager.owner ? manager.owner.height : 0) - edge -
                (anchorOrigin.y + (manager.anchorItem ? manager.anchorItem.height : 0) + gap)
            readonly property bool above: aboveRoom >= Math.max(26, Math.ceil(wrapMeasure.implicitHeight) + 8)
                || aboveRoom >= belowRoom
            width: panelWidth + shadow * 2
            height: panelHeight + shadow * 2
            x: Math.round((manager.owner ? manager.owner.x : 0) +
                Math.max(edge, Math.min((manager.owner ? manager.owner.width : 0) - edge - panelWidth,
                    anchorOrigin.x + (manager.anchorItem ? manager.anchorItem.width : 0) / 2 - panelWidth / 2)) - shadow)
            y: Math.round((manager.owner ? manager.owner.y : 0) +
                (above ? anchorOrigin.y - gap - panelHeight
                       : anchorOrigin.y + (manager.anchorItem ? manager.anchorItem.height : 0) + gap) - shadow)

            Text {
                id: measure
                visible: false
                text: manager.message
                textFormat: Text.PlainText
                font: label.font
                lineHeightMode: Text.FixedHeight
                lineHeight: 18
            }

            Text {
                id: wrapMeasure
                visible: false
                width: Math.max(1, tip.panelWidth - 16)
                text: manager.message
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font: label.font
                lineHeightMode: Text.FixedHeight
                lineHeight: 18
            }

            Repeater {
                model: tip.shadow
                Rectangle {
                    required property int index
                    readonly property int spread: tip.shadow - index - 1
                    x: tip.shadow - spread
                    y: tip.shadow - spread + 1
                    width: tip.panelWidth + spread * 2
                    height: tip.panelHeight + spread * 2
                    radius: 6 + spread
                    color: Qt.rgba(0, 0, 0, 0.027)
                    Accessible.ignored: true
                }
            }

            Rectangle {
                id: panel
                objectName: "auroraToolTipPanel"
                x: tip.shadow
                y: tip.shadow
                width: tip.panelWidth
                height: tip.panelHeight
                radius: 6
                color: tip.systemColors ? manager.colors.background : "#202631"
                border.width: tip.systemColors ? 1 : 0
                border.color: tip.systemColors ? manager.colors.outline : "transparent"

                Text {
                    id: label
                    objectName: "auroraToolTipText"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    text: manager.message
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: Math.max(1, Math.floor(((tip.above ? tip.aboveRoom : tip.belowRoom) - 8) / 18))
                    elide: Text.ElideRight
                    font.family: AuroraTheme.font
                    font.pixelSize: 12
                    font.weight: Font.Normal
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 18
                    color: tip.systemColors ? manager.colors.text : "#F8FAFC"
                }
            }
        }
    }
}
