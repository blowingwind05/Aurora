// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import ".." as Aurora

Column {
    id: center
    property var session
    property bool showSettingPages: true
    property bool offerNativeSettings: false
    property string page: ""
    property string previousPage: ""
    readonly property var groups: [
        { id: "quality", label: qsTr("画质"), icon: Aurora.AuroraTheme.icon("monitor") },
        { id: "audio", label: qsTr("声音"), icon: Qt.resolvedUrl("icons/volume.svg") },
        { id: "input", label: qsTr("输入"), icon: Qt.resolvedUrl("icons/mouse.svg") },
        { id: "safety", label: qsTr("安全"), icon: Qt.resolvedUrl("icons/shield.svg") }
    ]
    signal pageChangedByUser()
    signal nativeSettingsRequested()

    function focusInitial() {
        if (page !== "") {
            back.forceActiveFocus(Qt.TabFocusReason)
            return
        }
        for (var i = 0; i < groups.length; ++i) {
            if (groups[i].id === previousPage) {
                categories.itemAt(i).forceActiveFocus(Qt.TabFocusReason)
                return
            }
        }
        if (showSettingPages)
            categories.itemAt(0).forceActiveFocus(Qt.TabFocusReason)
        else
            fullScreenButton.forceActiveFocus(Qt.TabFocusReason)
    }

    function goBack() {
        if (!page)
            return false
        previousPage = page
        page = ""
        pageChangedByUser()
        return true
    }

    Column {
        width: parent.width
        visible: center.page === ""

        Repeater {
            id: categories
            model: center.showSettingPages ? center.groups : []
            StreamButton {
                objectName: "controlPage:" + modelData.id
                width: parent.width
                height: 34
                cornerRadius: 6
                rowStyle: true
                trailingIcon: Aurora.AuroraTheme.icon("chevron-right")
                text: modelData.label
                icon.source: modelData.icon
                onClicked: {
                    center.page = modelData.id
                    center.pageChangedByUser()
                }
            }
        }

        StreamButton {
            objectName: "nativeSessionSettings"
            visible: center.offerNativeSettings
            width: parent.width
            height: 34
            cornerRadius: 6
            rowStyle: true
            text: qsTr("更多串流设置")
            icon.source: Aurora.AuroraTheme.icon("settings")
            trailingIcon: Aurora.AuroraTheme.icon("chevron-right")
            onClicked: center.nativeSettingsRequested()
        }

        Item {
            visible: center.showSettingPages || center.offerNativeSettings
            width: 1
            height: 5
        }
        Rectangle {
            visible: center.showSettingPages || center.offerNativeSettings
            width: parent.width - 12
            x: 6
            height: 1
            color: Aurora.AuroraTheme.divider
        }

        Item {
            visible: center.showSettingPages || center.offerNativeSettings
            width: 1
            height: 5
        }
        StreamButton {
            id: fullScreenButton
            objectName: "fullScreen"
            width: parent.width
            height: 34
            cornerRadius: 6
            rowStyle: true
            selected: center.session.fullScreen
            enabled: center.session.canSetFullScreen
            text: selected ? qsTr("退出全屏") : qsTr("全屏")
            icon.source: Qt.resolvedUrl(selected ? "icons/unfull.svg" : "icons/full.svg")
            onClicked: {
                var next = !center.session.fullScreen
                center.session.closePanel(false)
                center.session.fullScreenRequested(next)
            }
        }
    }

    Item {
        width: parent.width
        height: 38
        visible: center.page !== ""

        StreamButton {
            id: back
            objectName: "controlBack"
            x: 2
            width: 27
            height: 27
            cornerRadius: 6
            text: qsTr("返回控制中心")
            hint: text
            showLabel: false
            icon.source: Aurora.AuroraTheme.icon("chevron-right")
            contentItem.rotation: 180
            onClicked: center.goBack()
        }

        StreamText {
            x: 34
            height: back.height
            font.weight: Font.DemiBold
            text: {
                for (var i = 0; i < center.groups.length; ++i) {
                    if (center.groups[i].id === center.page)
                        return center.groups[i].label
                }
                return ""
            }
        }

        StreamText {
            height: back.height
            anchors.right: parent.right
            anchors.rightMargin: 8
            visible: center.page === "quality" || center.page === "audio"
            text: qsTr("下次连接生效")
            color: Aurora.AuroraTheme.muted
            font.pixelSize: 11
        }
    }

    Loader {
        id: pageBody
        x: 8
        width: parent.width - 16
        active: center.page !== ""
        sourceComponent: center.page === "quality" ? qualityPage
                       : center.page === "audio" ? audioPage
                       : center.page === "input" ? inputPage : safetyPage
    }

    Component {
        id: qualityPage
        Column {
            spacing: 10
            StreamChoices {
                width: parent.width
                objectName: "resolution"
                label: qsTr("分辨率")
                setting: center.session.preferences.resolution || null
                onRequested: function(value) { center.session.resolutionRequested(value) }
            }
            StreamChoices {
                width: parent.width
                objectName: "frameRate"
                label: qsTr("帧率")
                setting: center.session.preferences.frameRate || null
                onRequested: function(value) { center.session.frameRateRequested(value) }
            }
            StreamBitrate {
                width: parent.width
                setting: center.session.preferences.bitrate || null
                onRequested: function(kbps) { center.session.bitrateRequested(kbps) }
            }
            StreamChoices {
                width: parent.width
                objectName: "codec"
                label: qsTr("视频编码")
                setting: center.session.preferences.codec || null
                onRequested: function(value) { center.session.codecRequested(value) }
            }
            StreamToggle {
                width: parent.width
                objectName: "hdr"
                text: qsTr("启用 HDR（实验）")
                setting: center.session.preferences.hdr || null
                onChangeRequested: function(enabled) { center.session.hdrRequested(enabled) }
            }
            StreamToggle {
                width: parent.width
                objectName: "performanceOverlay"
                text: qsTr("显示性能统计")
                setting: center.session.preferences.performanceOverlay || null
                onChangeRequested: function(enabled) { center.session.performanceOverlayRequested(enabled) }
            }
        }
    }

    Component {
        id: audioPage
        Column {
            spacing: 10
            StreamChoices {
                width: parent.width
                objectName: "audioConfiguration"
                label: qsTr("音频配置")
                setting: center.session.preferences.audioConfiguration || null
                onRequested: function(value) { center.session.audioConfigurationRequested(value) }
            }
            StreamToggle {
                width: parent.width
                objectName: "remoteAudioMuted"
                text: qsTr("远端扬声器静音")
                setting: center.session.preferences.remoteAudioMuted || null
                onChangeRequested: function(muted) { center.session.remoteAudioMutedRequested(muted) }
            }
            StreamToggle {
                width: parent.width
                objectName: "muteOnFocusLoss"
                text: qsTr("窗口失去焦点时静音")
                setting: center.session.preferences.muteOnFocusLoss || null
                onChangeRequested: function(muted) { center.session.muteOnFocusLossRequested(muted) }
            }
        }
    }

    Component {
        id: inputPage
        Column {
            spacing: 10
            StreamChoices {
                width: parent.width
                objectName: "mouseMode"
                vertical: true
                setting: center.session.mouseMode
                onRequested: function(value) { center.session.mouseModeRequested(value) }
            }
            StreamText {
                width: parent.width
                visible: center.session.mouseCaptured && center.session.releaseMouseShortcut.length > 0
                text: qsTr("释放鼠标 %1").arg(center.session.releaseMouseShortcut)
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: Aurora.AuroraTheme.muted
            }
            StreamText {
                text: qsTr("下次连接生效")
                font.pixelSize: 11
                color: Aurora.AuroraTheme.muted
            }
            StreamToggle {
                width: parent.width
                objectName: "swapMouseButtons"
                text: qsTr("交换鼠标左右键")
                setting: center.session.preferences.swapMouseButtons || null
                onChangeRequested: function(enabled) { center.session.swapMouseButtonsRequested(enabled) }
            }
            StreamToggle {
                width: parent.width
                objectName: "reverseScroll"
                text: qsTr("反转滚轮方向")
                setting: center.session.preferences.reverseScroll || null
                onChangeRequested: function(enabled) { center.session.reverseScrollRequested(enabled) }
            }
        }
    }

    Component {
        id: safetyPage
        Column {
            spacing: 5
            StreamToggle {
                width: parent.width
                objectName: "privacyScreen"
                text: qsTr("隐私屏保")
                setting: center.session.privacyScreen
                onChangeRequested: function(enabled) { center.session.privacyScreenRequested(enabled) }
            }
            StreamText {
                width: parent.width
                text: center.session.privacyScreenMessage
                    || (center.session.privacyScreen ? "" : qsTr("远端尚未提供隐私屏保能力"))
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: Aurora.AuroraTheme.muted
                font.pixelSize: 11
            }
        }
    }
}
