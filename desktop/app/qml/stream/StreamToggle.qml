// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import ".." as Aurora

StreamButton {
    id: toggle
    property var setting: null
    signal changeRequested(bool enabled)

    rowStyle: true
    implicitHeight: 24
    cornerRadius: 6
    enabled: setting !== null && typeof setting.value === "boolean" && setting.writable === true
    Accessible.role: Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: setting !== null && setting.value === true

    contentItem: Item {
        StreamText {
            width: parent.width - 44
            height: parent.height
            text: toggle.text
            color: toggle.foreground
            elide: Text.ElideRight
            Accessible.ignored: true
        }
        Rectangle {
            id: track
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 18
            radius: 9
            visible: toggle.setting !== null && typeof toggle.setting.value === "boolean"
            color: toggle.setting && toggle.setting.value === true ? Aurora.AuroraTheme.blue : "#C6CFDC"
            opacity: toggle.enabled ? 1 : 0.6

            Rectangle {
                x: toggle.setting && toggle.setting.value === true ? 17 : 3
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                radius: 6
                color: "white"
            }
        }
        StreamText {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: !track.visible
            text: qsTr("不可用")
            color: Aurora.AuroraTheme.muted
            font.pixelSize: 11
        }
    }

    onClicked: changeRequested(!setting.value)
}
