// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

Column {
    id: feedback
    property var editor
    spacing: 3
    Text {
        objectName: "sunshineFeedback"
        width: parent.width
        text: feedback.editor.failed ? qsTr("部分设置未保存。请检查对应字段。")
            : feedback.editor.saving ? qsTr("正在保存 Sunshine 设置…") : (feedback.editor.state.feedback || "")
        visible: text.length > 0
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: feedback.editor.failed ? "#9C2B2B" : AuroraTheme.muted
        font.family: AuroraTheme.font
        font.pixelSize: 12
    }
    AuroraButton {
        objectName: "sunshineRetrySave"
        visible: feedback.editor.failed && feedback.editor.ready
        enabled: !feedback.editor.state.busy && !feedback.editor.inFlight
        quiet: true
        hoverFill: "transparent"
        text: qsTr("重试保存")
        onClicked: feedback.editor.retry()
    }
}
