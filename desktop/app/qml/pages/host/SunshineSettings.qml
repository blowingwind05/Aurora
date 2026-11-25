// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../../common"

Column {
    id: settings
    property var editor
    property int inset: 14
    property bool active: false
    readonly property var pending: editor.state.pairingRequests || []
    readonly property bool creating: editor.state.status === "setup"
    property string selectedPairing: ""

    Timer {
        interval: 3000
        repeat: true
        running: settings.active && settings.editor.ready
        onTriggered: {
            if (!settings.editor.state.busy)
                settings.editor.command("sunshinePairingRefresh")
        }
    }

    onPendingChanged: {
        if (pending.some(function(item) { return item.id === selectedPairing }))
            return
        selectedPairing = pending.length ? pending[0].id : ""
        if (pin)
            pin.clear()
    }
    signal advancedRequested()
    signal restartRequested()

    spacing: 12
    onActiveChanged: {
        if (!active) {
            password.clear()
            confirmation.clear()
            pin.clear()
        }
    }

    component CopyText: Text {
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: AuroraTheme.font
        font.pixelSize: 13
        color: AuroraTheme.muted
    }

    component SectionHeading: CopyText {
        font.pixelSize: 15
        font.weight: Font.DemiBold
        color: AuroraTheme.ink
        Accessible.role: Accessible.Heading
    }

    component Card: Rectangle {
        color: "white"
        radius: 11
        border.color: AuroraTheme.panelLine
    }

    Card {
        objectName: "sunshineCommon"
        width: parent.width
        height: common.height + 10
        visible: settings.editor.ready

        Column {
            id: common
            x: settings.inset
            width: parent.width - 2 * x

            RowLayout {
                width: parent.width
                height: 46
                SectionHeading {
                    Layout.fillWidth: true
                    text: qsTr("常用设置")
                }
                SunshineLink {
                    objectName: "sunshineOpenAdvanced"
                    text: qsTr("全部配置项")
                    icon.source: AuroraTheme.icon("chevron-right")
                    onClicked: settings.advancedRequested()
                }
            }
            SunshineField {
                width: parent.width
                settingKey: "sunshine_name"
                label: qsTr("被控电脑显示名")
                placeholder: qsTr("系统名称")
                editor: settings.editor
            }
            SunshineField {
                width: parent.width
                settingKey: "stream_audio"
                label: qsTr("将本机声音发送到主控端")
                kind: "toggle"
                editor: settings.editor
            }
            AuroraButton {
                id: restart
                objectName: "sunshineRestart"
                anchors.right: parent.right
                enabled: !settings.editor.state.busy && !settings.editor.continuation
                text: qsTr("重启 Sunshine")
                implicitWidth: implicitContentWidth + 20
                background: Rectangle {
                    radius: 7
                    color: restart.hovered ? AuroraTheme.hover : "white"
                    border.color: restart.visualFocus ? AuroraTheme.blue : AuroraTheme.panelLine
                    border.width: restart.visualFocus ? 2 : 1
                }
                onClicked: settings.restartRequested()
            }
        }
    }

    Card {
        objectName: "sunshineAccess"
        width: parent.width
        height: access.height + 24
        visible: settings.editor.state.status === "login" || settings.creating || settings.editor.state.status === "certificate"

        Column {
            id: access
            x: settings.inset
            y: 12
            width: parent.width - 2 * x
            spacing: 8
            readonly property bool certificate: settings.editor.state.status === "certificate"

            SectionHeading {
                width: parent.width
                text: access.certificate ? qsTr("无法读取 Sunshine 公开证书") :
                      settings.creating ? qsTr("创建管理账号") : qsTr("登录 Sunshine")
            }
            CopyText {
                width: parent.width
                visible: access.certificate && text.length > 0
                text: settings.editor.state.note || ""
            }
            SunshineInput {
                id: username
                visible: !access.certificate
                width: parent.width
                placeholderText: qsTr("管理用户名")
                Accessible.name: placeholderText
            }
            SunshineInput {
                id: password
                visible: !access.certificate
                width: parent.width
                placeholderText: qsTr("管理密码")
                Accessible.name: placeholderText
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            }
            SunshineInput {
                id: confirmation
                visible: settings.creating
                width: parent.width
                placeholderText: qsTr("再次输入密码")
                Accessible.name: placeholderText
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            }
            CheckBox {
                id: remember
                visible: !access.certificate && settings.editor.state.rememberAvailable === true
                width: parent.width
                text: qsTr("记住登录")
                Accessible.description: qsTr("保存在 Windows 凭据管理器")
                font.family: AuroraTheme.font
                font.pixelSize: 13
            }
            AuroraButton {
                objectName: "sunshineAccessAction"
                enabled: !settings.editor.state.busy
                text: access.certificate ? qsTr("导入公开证书指纹") : settings.creating ? qsTr("创建并登录") : qsTr("登录")
                primary: true
                onClicked: {
                    if (access.certificate) {
                        settings.editor.command("sunshineCertificate")
                    } else {
                        const data = { username: username.text, password: password.text,
                                       confirmation: confirmation.text, remember: remember.checked }
                        password.clear()
                        confirmation.clear()
                        settings.editor.command("sunshineLogin", data)
                    }
                }
            }
        }
    }

    Card {
        width: parent.width
        height: pairing.height + 24
        visible: settings.editor.ready && settings.pending.length > 0

        Column {
            id: pairing
            x: settings.inset
            y: 12
            width: parent.width - 2 * x
            spacing: 8

            SectionHeading { text: qsTr("Moonlight 配对请求") }
            AuroraSettingChoice {
                id: pairingChoice
                width: parent.width
                model: settings.pending.map(function(item) { return item.name })
                selectedIndex: settings.pending.findIndex(function(item) { return item.id === settings.selectedPairing })
                Accessible.name: qsTr("要确认的 Moonlight 设备")
                onSelectionRequested: function(index) {
                    settings.selectedPairing = settings.pending[index].id
                    pin.clear()
                }
            }
            SunshineInput {
                id: pin
                width: parent.width
                placeholderText: qsTr("主控端显示的四位 PIN")
                Accessible.name: placeholderText
                maximumLength: 4
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhSensitiveData
            }
            AuroraButton {
                enabled: !settings.editor.state.busy && pairingChoice.selectedIndex >= 0 && pairingChoice.selectedIndex < settings.pending.length
                primary: true
                text: qsTr("确认配对")
                onClicked: {
                    const code = pin.text
                    pin.clear()
                    settings.editor.command("sunshinePair", { pairingId: settings.selectedPairing, pin: code })
                }
            }
        }
    }

    Column {
        width: parent.width
        visible: !!settings.editor.state.status && settings.editor.state.status !== "certificate"
        spacing: 4
        CopyText {
            width: parent.width
            visible: text.length > 0
            text: settings.editor.state.status === "idle" || settings.editor.state.status === "loading"
                ? qsTr("正在读取本机 Sunshine 设置…") : (settings.editor.state.note || "")
        }
        AuroraButton {
            visible: settings.editor.state.status === "unavailable"
            enabled: !settings.editor.state.busy
            quiet: true
            hoverFill: "transparent"
            text: qsTr("重新读取 Sunshine 设置")
            implicitWidth: implicitContentWidth + 16
            onClicked: settings.editor.command("sunshineLoad")
        }
        AuroraButton {
            visible: settings.editor.state.forgetAvailable === true
            enabled: !settings.editor.state.busy
            quiet: true
            hoverFill: "transparent"
            text: qsTr("忘记已保存的登录")
            implicitWidth: implicitContentWidth + 16
            onClicked: settings.editor.command("sunshineForget")
        }
    }

    CopyText {
        width: parent.width
        visible: text.length > 0
        text: settings.editor.state.pairingFeedback || ""
    }
    AuroraButton {
        visible: settings.editor.state.reauthorizeAvailable === true
        enabled: !settings.editor.state.busy
        quiet: true
        hoverFill: "transparent"
        text: qsTr("允许当前账号的设备连接")
        implicitWidth: implicitContentWidth + 16
        onClicked: settings.editor.command("sunshineReauthorize", {
            pairingConfirmation: settings.editor.state.pairingConfirmation || ""
        })
    }

    SunshineFeedback {
        width: parent.width
        visible: height > 0
        editor: settings.editor
    }
}
