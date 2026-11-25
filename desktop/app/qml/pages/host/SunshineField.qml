// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../../common"

Column {
    id: row
    property string settingKey
    property string label
    property string kind: "text"
    property string placeholder: ""
    property var choices: []
    property var choiceNames: []
    property bool showKey: false
    property bool managed: false
    property var editor
    readonly property string value: editor.value(settingKey)
    readonly property string error: editor.errorFor(settingKey)
    readonly property bool writable: editor.ready && !managed && !editor.continuation
    readonly property bool checked: ["on", "yes", "true", "enable", "enabled", "1"].indexOf(value.trim().toLowerCase()) >= 0

    function edit(value, text) { editor.edit(settingKey, value, text) }

    RowLayout {
        width: parent.width
        height: Math.max(45, fieldLabel.implicitHeight + 10)
        spacing: 10

        Column {
            id: fieldLabel
            Layout.fillWidth: true
            spacing: 2
            Text {
                width: parent.width
                text: row.label
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: AuroraTheme.ink
                font.family: AuroraTheme.font
                font.pixelSize: 13
            }
            Text {
                width: parent.width
                visible: row.showKey || row.managed
                text: row.managed ? row.settingKey + " · " + qsTr("由 Aurora 统一管理") : row.settingKey
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: AuroraTheme.muted
                font.family: AuroraTheme.font
                font.pixelSize: 11
            }
        }

        Loader {
            Layout.preferredWidth: row.kind === "toggle" ? 34 : Math.min(235, Math.floor(row.width * 0.48))
            Layout.preferredHeight: 34
            sourceComponent: row.kind === "toggle" ? toggleControl : row.kind === "choice" ? choiceControl : inputControl
        }
    }

    Text {
        width: parent.width
        visible: row.error.length > 0
        text: row.error
        textFormat: Text.PlainText
        Accessible.name: text
        wrapMode: Text.Wrap
        color: "#9C2B2B"
        font.family: AuroraTheme.font
        font.pixelSize: 12
    }

    Component {
        id: inputControl
        SunshineInput {
            objectName: "sunshineField:" + row.settingKey
            enabled: row.writable
            Accessible.name: row.label
            placeholderText: row.placeholder
            text: row.value
            onTextEdited: row.edit(text, true)
        }
    }

    Component {
        id: choiceControl
        AuroraSettingChoice {
            objectName: "sunshineField:" + row.settingKey
            readonly property var values: row.choices.indexOf(row.value) < 0 ? row.choices.concat([row.value]) : row.choices
            model: values.map(function(value, index) {
                return row.choiceNames[index] || value || qsTr("自动 / 默认")
            })
            selectedIndex: values.indexOf(row.value)
            Accessible.name: row.label
            enabled: row.writable
            onSelectionRequested: function(index) { row.edit(values[index], false) }
        }
    }

    Component {
        id: toggleControl
        Button {
            id: control
            objectName: "sunshineField:" + row.settingKey
            enabled: row.writable
            Accessible.name: row.label
            Accessible.role: Accessible.CheckBox
            Accessible.checkable: true
            Accessible.checked: row.checked
            padding: 0
            background: null
            contentItem: Item {
                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    height: 18
                    radius: 9
                    color: row.checked ? AuroraTheme.blue : "#F0F3F7"
                    border.color: row.checked ? AuroraTheme.blue : "#8E9BB0"
                    opacity: control.enabled ? 1 : 0.55
                    Rectangle {
                        x: row.checked ? 17 : 3
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        height: 12
                        radius: 6
                        color: row.checked ? "white" : "#6D7C92"
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: 11
                        color: "transparent"
                        border.width: 2
                        border.color: AuroraTheme.blue
                        visible: control.visualFocus
                    }
                }
            }
            onClicked: row.edit(row.checked ? "disabled" : "enabled", false)
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
        }
    }
}
