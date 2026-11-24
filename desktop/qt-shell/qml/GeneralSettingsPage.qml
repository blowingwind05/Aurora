// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Templates 2.15 as T

Item {
    id: page
    property var preferences: null
    property bool compact: width <= 425
    property bool narrow: width <= 502
    property bool updating: false
    property string closeFeedback: ""
    property string startupFeedback: ""
    property string sleepFeedback: ""

    signal closeToTrayRequested(bool enabled)
    signal startupEnabledRequested(bool enabled)
    signal sleepModeRequested(string mode)

    readonly property int inset: compact ? 8 : narrow ? 13 : 21
    readonly property int blockInset: narrow || compact ? 16 : 20
    readonly property int rowInset: compact ? 8 : 14
    readonly property int fieldWidth: Math.min(235, Math.floor((cards.width - 2 * rowInset) * 0.5))
    readonly property bool closeToTray: preferences !== null && preferences.closeToTray
    readonly property bool startupEnabled: preferences !== null && preferences.startupEnabled
    readonly property var sleepModes: ["System", "OnAc", "Always"]

    function revealControl(control) {
        if (!control.activeFocus)
            return
        const top = control.mapToItem(settingsScroll.contentItem, 0, 0).y - 4
        const bottom = top + control.height + 8
        if (top < settingsScroll.contentY)
            settingsScroll.contentY = Math.max(0, top)
        else if (bottom > settingsScroll.contentY + settingsScroll.height)
            settingsScroll.contentY = Math.min(settingsScroll.contentHeight - settingsScroll.height,
                                               bottom - settingsScroll.height)
    }
    component SettingsCard: Rectangle {
        property string title
        default property alias contents: body.data
        width: parent.width
        height: body.implicitHeight + 4
        radius: 11
        color: "white"
        border.color: AuroraTheme.panelLine

        Column {
            id: body
            y: 1
            x: page.rowInset
            width: parent.width - 2 * x

            Text {
                width: parent.width
                height: 46
                text: body.parent.title
                font.family: AuroraTheme.font
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: AuroraTheme.ink
                verticalAlignment: Text.AlignVCenter
                Accessible.role: Accessible.Heading
            }
        }
    }

    component SettingRow: RowLayout {
        property string label
        width: parent.width
        height: Math.max(45, implicitHeight + 8)
        spacing: 10

        Text {
            text: parent.label
            color: AuroraTheme.ink
            font.family: AuroraTheme.font
            font.pixelSize: 13
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }

    component FieldNote: Text {
        width: parent.width
        visible: text.length > 0
        bottomPadding: 10
        color: AuroraTheme.muted
        font.family: AuroraTheme.font
        font.pixelSize: 12
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
    }

    Column {
        id: heading
        anchors.top: parent.top
        anchors.topMargin: page.blockInset
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: page.inset
        anchors.rightMargin: page.inset
        spacing: 6

        Text {
            height: 34
            text: qsTr("设置")
            font.family: AuroraTheme.font
            font.pixelSize: page.compact ? 21 : 26
            font.weight: Font.DemiBold
            color: AuroraTheme.ink
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.Heading
        }

        Item {
            width: Math.max(40, tab.implicitWidth)
            height: 40
            Text {
                id: tab
                anchors.centerIn: parent
                text: qsTr("通用")
                font.family: AuroraTheme.font
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: AuroraTheme.ink
                Accessible.role: Accessible.Heading
            }
            Rectangle {
                width: 18
                height: 3
                radius: 2
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 2
                color: AuroraTheme.blue
            }
        }
    }

    Flickable {
        id: settingsScroll
        objectName: "settingsScroll"
        anchors.top: heading.bottom
        anchors.topMargin: 11
        anchors.left: heading.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: page.blockInset
        contentWidth: width
        contentHeight: cards.height + 2
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ScrollBar.vertical: ScrollBar {
            width: 7
            padding: 1
            policy: settingsScroll.contentHeight > settingsScroll.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            background: Rectangle { color: "#F2F5F9"; radius: 3 }
            contentItem: Rectangle { color: "#A1AEC2"; radius: 3 }
        }

        Column {
            id: cards
            width: parent.width - page.inset
            spacing: 12

            SettingsCard {
                title: qsTr("Aurora 窗口")

                SettingRow {
                    id: startupRow
                    label: qsTr("开机自启动")

                    T.Switch {
                        id: startup
                        objectName: "startupSwitch"
                        onActiveFocusChanged: page.revealControl(startup)
                        implicitWidth: 32
                        implicitHeight: 34
                        focusPolicy: Qt.StrongFocus
                        Accessible.name: startupRow.label
                        enabled: !page.updating && page.preferences !== null && page.preferences.startupAvailable
                        checked: page.startupEnabled

                        onToggled: {
                            const requested = checked
                            checked = Qt.binding(function() { return page.startupEnabled })
                            if (requested !== page.startupEnabled)
                                page.startupEnabledRequested(requested)
                        }

                        indicator: Rectangle {
                            width: 32
                            height: 18
                            radius: height / 2
                            y: (startup.height - height) / 2
                            opacity: startup.enabled ? 1 : 0.55
                            color: startup.checked ? AuroraTheme.blue : "#F0F3F7"
                            border.color: startup.checked ? AuroraTheme.blue : "#8E9BB0"

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                x: startup.checked ? 16 : 4
                                width: 12
                                height: 12
                                radius: 6
                                color: startup.checked ? "white" : "#6D7C92"
                            }
                        }

                        background: Rectangle {
                            anchors.fill: startup.indicator
                            anchors.margins: -4
                            radius: height / 2
                            color: "transparent"
                            border.width: 2
                            border.color: AuroraTheme.blue
                            visible: startup.visualFocus
                        }
                    }
                }
                FieldNote { text: page.startupFeedback }

                SettingRow {
                    id: closeRow
                    visible: page.preferences === null || page.preferences.trayAvailable
                    label: qsTr("关闭窗口时")

                    AuroraSettingChoice {
                        id: closeChoice
                        objectName: "closeChoice"
                        onActiveFocusChanged: page.revealControl(closeChoice)
                        Layout.preferredWidth: page.fieldWidth
                        Accessible.name: closeRow.label
                        enabled: !page.updating && page.preferences !== null
                        model: [AuroraTheme.mac ? qsTr("隐藏到菜单栏") : qsTr("最小化到托盘"), qsTr("退出 Aurora")]
                        selectedIndex: page.preferences === null ? -1 : page.closeToTray ? 0 : 1
                        onSelectionRequested: function(index) { page.closeToTrayRequested(index === 0) }
                    }
                }
                FieldNote { text: closeRow.visible ? page.closeFeedback : "" }
            }

            SettingsCard {
                title: qsTr("电源")

                SettingRow {
                    id: sleepRow
                    label: qsTr("防止电脑自动休眠")

                    AuroraSettingChoice {
                        id: sleep
                        objectName: "sleepChoice"
                        onActiveFocusChanged: page.revealControl(sleep)
                        Layout.preferredWidth: page.fieldWidth
                        Accessible.name: sleepRow.label
                        enabled: !page.updating && page.preferences !== null && selectedIndex >= 0
                        model: [qsTr("关闭"), qsTr("仅接通电源时"), qsTr("始终")]
                        selectedIndex: page.preferences === null ? -1 : page.sleepModes.indexOf(page.preferences.sleepMode)
                        onSelectionRequested: function(index) { page.sleepModeRequested(page.sleepModes[index]) }
                    }
                }
                FieldNote {
                    text: page.sleepFeedback || qsTr("关闭时按系统电源设置休眠。防休眠仅在被控组件运行时生效。")
                }
            }
        }
    }
}
