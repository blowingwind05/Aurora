// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

ColumnLayout {
    id: page
    property var editor
    property int inset: 14
    readonly property var categories: [qsTr("显示与编码"), qsTr("输入"), qsTr("网络")]
    property int category: 0
    readonly property var groups: filteredGroups(search.text, category, editor.options)
    spacing: 11

    function filteredGroups(searchText, categoryIndex, options) {
        const query = searchText.trim().toLowerCase()
        const result = []
        const byName = Object.create(null)
        for (let i = 0; i < options.length; ++i) {
            const option = options[i]
            const matches = query.length > 0
                ? (option.key + " " + option.label + " " + option.group).toLowerCase().indexOf(query) >= 0
                : option.category === categories[categoryIndex]
            if (!matches)
                continue
            if (!byName[option.group]) {
                byName[option.group] = { title: option.group, options: [] }
                result.push(byName[option.group])
            }
            byName[option.group].options.push(option)
        }
        return result
    }

    function selectCategory(index) {
        search.clear()
        category = index
        scroll.contentY = 0
    }

    GridLayout {
        id: filter
        Layout.fillWidth: true
        columns: width >= 475 ? 2 : 1
        columnSpacing: 10
        rowSpacing: 5

        Row {
            Layout.fillWidth: true
            spacing: Math.max(6, Math.min(24, filter.width * 0.025))
            Repeater {
                id: tabs
                model: page.categories
                delegate: AuroraButton {
                    id: tab
                    required property int index
                    required property string modelData
                    objectName: "sunshineTab:" + index
                    text: modelData
                    height: 40
                    implicitWidth: Math.max(40, implicitContentWidth + 8)
                    quiet: true
                    hoverFill: "transparent"
                    foreground: page.category === index ? AuroraTheme.blue : AuroraTheme.muted
                    font.pixelSize: 15
                    Accessible.role: Accessible.PageTab
                    Accessible.selected: page.category === index
                    activeFocusOnTab: page.category === index
                    onClicked: page.selectCategory(index)
                    Keys.onLeftPressed: {
                        const index = (tab.index + page.categories.length - 1) % page.categories.length
                        page.selectCategory(index)
                        tabs.itemAt(index).forceActiveFocus(Qt.TabFocusReason)
                    }
                    Keys.onRightPressed: {
                        const index = (tab.index + 1) % page.categories.length
                        page.selectCategory(index)
                        tabs.itemAt(index).forceActiveFocus(Qt.TabFocusReason)
                    }
                    Rectangle {
                        visible: page.category === tab.index
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 2
                        width: 18
                        height: 3
                        radius: 2
                        color: AuroraTheme.blue
                    }
                }
            }
        }
        SunshineInput {
            id: search
            objectName: "sunshineSearch"
            Layout.fillWidth: filter.columns === 1
            Layout.preferredWidth: filter.columns === 1 ? filter.width : 235
            placeholderText: qsTr("搜索设置名称或配置键")
            Accessible.name: placeholderText
            onTextChanged: scroll.contentY = 0
        }
    }

    Flickable {
        id: scroll
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: width
        contentHeight: sections.height + 8
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: sections
            width: scroll.width - 8
            spacing: 12

            Repeater {
                model: page.groups
                delegate: Rectangle {
                    required property var modelData
                    width: sections.width
                    height: fields.height + 5
                    radius: 11
                    border.color: AuroraTheme.panelLine
                    color: "white"

                    Column {
                        id: fields
                        x: page.inset
                        width: parent.width - 2 * x

                        Text {
                            width: parent.width
                            height: 46
                            verticalAlignment: Text.AlignVCenter
                            text: modelData.title
                            textFormat: Text.PlainText
                            color: AuroraTheme.ink
                            font.family: AuroraTheme.font
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            Accessible.role: Accessible.Heading
                        }
                        Repeater {
                            model: modelData.options
                            delegate: SunshineField {
                                required property var modelData
                                width: fields.width
                                editor: page.editor
                                settingKey: modelData.key
                                label: modelData.label
                                kind: modelData.kind
                                placeholder: modelData.default
                                choices: modelData.choices
                                managed: modelData.managed
                                showKey: true
                            }
                        }
                    }
                }
            }
            Text {
                visible: page.groups.length === 0
                width: parent.width
                text: page.editor.ready ? qsTr("没有匹配的设置。") : qsTr("正在读取本机 Sunshine 设置…")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: AuroraTheme.muted
                font.family: AuroraTheme.font
                font.pixelSize: 13
            }
        }
    }

    SunshineFeedback {
        Layout.fillWidth: true
        visible: height > 0
        editor: page.editor
    }
}
