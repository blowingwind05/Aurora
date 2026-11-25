// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Layouts 1.15
import "../common" as Aurora

Column {
    id: details
    property var session
    property bool connection: false
    readonly property var activeDisplay: {
        for (var i = 0; i < session.displays.length; ++i) {
            if (session.displays[i].id === session.activeDisplayId)
                return session.displays[i]
        }
        return null
    }
    readonly property var fields: {
        var metrics = session.connectionMetrics
        var display = activeDisplay
        var rows = connection ? [
            [qsTr("画面分辨率"), metrics.resolution],
            [qsTr("接收帧率"), metrics.frameRate],
            [qsTr("接收码率"), metrics.bitrate],
            [qsTr("网络丢帧"), metrics.networkLoss],
            [qsTr("视频编码"), metrics.codec],
            [qsTr("平均解码耗时"), metrics.decodeTime]
        ] : display ? [
            [qsTr("桌面分辨率"), display.resolution],
            [qsTr("刷新率"), display.refreshRate],
            [qsTr("系统缩放"), display.scale]
        ] : []
        return rows.filter(function(row) { return typeof row[1] === "string" && row[1].length > 0 })
    }
    readonly property bool hasHeader: connection ? !!session.connectionMetrics.latency : activeDisplay !== null
    spacing: 7

    function focusInitial() {
        for (var i = 0; i < displayOptions.count; ++i) {
            var button = displayOptions.itemAt(i).entry
            if (button.enabled && button.selected) {
                button.forceActiveFocus(Qt.TabFocusReason)
                return true
            }
        }
        return false
    }

    Column {
        width: parent.width
        visible: !details.connection && details.session.compact
        spacing: 2

        Repeater {
            id: displayOptions
            model: parent.visible ? details.session.displays : []
            Item {
                readonly property alias entry: selectDisplay
                width: parent.width
                height: 32

                StreamButton {
                    id: selectDisplay
                    objectName: "displayOption:" + modelData.id
                    anchors.left: parent.left
                    anchors.right: closeOption.visible ? closeOption.left : parent.right
                    height: parent.height
                    text: modelData.name
                    selected: modelData.id === details.session.activeDisplayId
                    Accessible.selected: selected
                    enabled: selected || modelData.ready === true
                    rowStyle: true
                    cornerRadius: 5
                    font.pixelSize: 12
                    icon.source: Aurora.AuroraTheme.icon("monitor")
                    onClicked: if (!selected) details.session.displayRequested(modelData.id)
                }

                StreamButton {
                    id: closeOption
                    objectName: "closeOption:" + modelData.id
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24
                    showLabel: false
                    cornerRadius: 5
                    visible: modelData.removable === true
                    enabled: modelData.removalPending !== true
                    text: qsTr("关闭 %1").arg(modelData.name)
                    hint: text
                    icon.source: Aurora.AuroraTheme.icon("x")
                    onClicked: details.session.removeDisplayRequested(modelData.id)
                }
            }
        }
    }

    RowLayout {
        x: 8
        width: parent.width - 16
        visible: details.hasHeader
        height: 31
        spacing: 6

        Aurora.AuroraGlyph {
            Layout.preferredWidth: 15
            Layout.preferredHeight: 15
            icon.source: details.connection ? Qt.resolvedUrl("icons/signal.svg") : Aurora.AuroraTheme.icon("monitor")
            icon.color: details.session.statusColor
        }
        StreamText {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.Wrap
            text: details.connection ? qsTr("网络延迟") : details.activeDisplay ? details.activeDisplay.name : ""
        }
        StreamText {
            text: details.connection ? details.session.connectionMetrics.latency || ""
                                     : details.activeDisplay ? details.activeDisplay.kind || "" : ""
            color: details.connection ? Aurora.AuroraTheme.ink : Aurora.AuroraTheme.muted
            font.weight: details.connection ? Font.DemiBold : Font.Normal
            font.pixelSize: details.connection ? 15 : 12
        }
    }

    Rectangle {
        x: 8
        width: parent.width - 16
        height: 1
        visible: details.hasHeader && details.fields.length > 0
        color: Aurora.AuroraTheme.divider
    }

    Repeater {
        model: details.fields
        RowLayout {
            x: 8
            width: parent.width - 16
            spacing: 8
            StreamText {
                text: modelData[0]
                font.pixelSize: 12
                color: Aurora.AuroraTheme.muted
            }
            StreamText {
                objectName: "metric:" + modelData[0]
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignRight
                text: modelData[1]
                font.pixelSize: 12
            }
        }
    }

    StreamText {
        x: 8
        width: parent.width - 16
        visible: details.fields.length === 0 && (details.connection ? !details.hasHeader : !details.activeDisplay)
        text: details.connection ? qsTr("暂无连接参数") : qsTr("暂无显示器详情")
        color: Aurora.AuroraTheme.muted
        font.pixelSize: 12
    }
}
