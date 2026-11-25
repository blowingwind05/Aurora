// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Layouts 1.15
import "../common" as Aurora

Column {
    id: group
    property string label
    property var setting: null
    property bool vertical: false
    signal requested(var value)
    spacing: 5

    StreamText {
        visible: group.label.length > 0
        text: group.label
    }

    GridLayout {
        width: parent.width
        columns: group.vertical ? 1 : Math.max(1, Math.min(4, choices.count, Math.floor(width / 58)))
        rowSpacing: 4
        columnSpacing: 4

        Repeater {
            id: choices
            model: group.setting && group.setting.options ? group.setting.options : []
            StreamButton {
                id: option
                objectName: group.objectName + ":" + modelData.value
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: optionText.advanceWidth + 26
                implicitHeight: group.vertical ? 34 : 25
                padding: 0
                font.pixelSize: group.vertical ? 13 : 12
                cornerRadius: group.vertical ? 6 : 5
                text: modelData.label
                hint: text
                Accessible.name: group.label.length > 0 ? group.label + "，" + text : text
                Accessible.selected: selected
                selected: group.setting.value === modelData.value
                enabled: group.setting.writable === true && modelData.enabled !== false
                rowStyle: group.vertical
                idleFill: group.vertical ? "transparent" : Aurora.AuroraTheme.workspace
                trailingIcon: group.vertical && selected
                    ? Qt.resolvedUrl("../../../../docs/visual-design/assets/icons/phosphor/check-bold.svg") : ""

                TextMetrics {
                    id: optionText
                    text: modelData.label
                    font: option.font
                }
                foreground: !enabled ? Aurora.AuroraTheme.disabled
                          : selected ? "#173D83" : Aurora.AuroraTheme.ink
                onClicked: if (!selected) group.requested(modelData.value)
            }
        }
    }

    StreamText {
        visible: choices.count === 0
        text: qsTr("暂无可用选项")
        color: Aurora.AuroraTheme.muted
        font.pixelSize: 11
    }
}
