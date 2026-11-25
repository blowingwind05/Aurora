// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import "../common" as Aurora

Column {
    id: field
    property var setting: null
    property int liveLimit: 0
    readonly property bool hasValue: setting !== null && typeof setting.value === "number"
    readonly property bool hasRange: hasValue && setting.maximum > setting.minimum && setting.step > 0
    readonly property int confirmed: hasValue ? setting.value : 0
    readonly property int displayed: slider.pressed && slider.pending >= 0 ? slider.pending : confirmed
    signal requested(int kbps)
    spacing: 3

    Item {
        width: parent.width
        height: title.implicitHeight
        StreamText {
            id: title
            text: qsTr("视频码率")
        }
        StreamText {
            anchors.right: parent.right
            text: field.hasValue ? qsTr("%1 Mbps").arg((field.displayed / 1000).toLocaleString(Qt.locale(), "g", 6))
                                 : qsTr("不可用")
            color: Aurora.AuroraTheme.muted
            font.pixelSize: 12
        }
    }

    Slider {
        id: slider
        objectName: "bitrateSlider"
        property int pending: -1
        width: parent.width
        height: 20
        padding: 0
        visible: field.hasRange
        enabled: visible && field.setting.writable === true
        from: field.hasRange ? field.setting.minimum : 0
        to: field.hasRange ? field.setting.maximum : 1
        stepSize: field.hasRange ? field.setting.step : 1
        snapMode: Slider.SnapAlways
        value: field.confirmed
        Accessible.name: title.text

        function commit() {
            if (enabled && pending >= 0 && pending !== field.confirmed)
                field.requested(pending)
            pending = -1
            value = Qt.binding(function() { return field.confirmed })
        }

        onMoved: {
            pending = Math.round(value)
            if (!pressed)
                Qt.callLater(commit)
        }
        onPressedChanged: if (!pressed) commit()

        background: Rectangle {
            x: slider.leftPadding
            width: slider.availableWidth
            height: 8
            y: (slider.height - height) / 2
            radius: 3
            color: "#EEEEEE"
            border.color: "#AEB1B5"
            Rectangle {
                x: 1
                y: 1
                width: Math.max(0, slider.position * (parent.width - 2))
                height: parent.height - 2
                radius: 3
                color: slider.enabled ? Aurora.AuroraTheme.blue : Aurora.AuroraTheme.disabled
            }
        }
        handle: Rectangle {
            implicitWidth: 15
            implicitHeight: 15
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: (slider.height - height) / 2
            radius: 8
            color: slider.enabled ? Aurora.AuroraTheme.blue : Aurora.AuroraTheme.disabled
            border.width: slider.visualFocus ? 2 : 0
            border.color: Aurora.AuroraTheme.ink
        }
    }

    StreamText {
        width: parent.width
        visible: field.hasRange && field.liveLimit > 0
        text: qsTr("%1 Mbps 以内即时生效").arg((field.liveLimit / 1000).toLocaleString(Qt.locale(), "g", 6))
        color: Aurora.AuroraTheme.muted
        font.pixelSize: 11
    }
}
