// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../../common"

Item {
    id: page
    property var hostState: ({})
    property bool compact: false
    property bool narrow: false
    property bool reduceMotion: false
    signal actionRequested(string action)
    signal sunshineRequested(string action, var data)
    property bool advanced: false
    readonly property bool hasUnfinishedSettings: sunshineEditor.dirty || sunshineEditor.inFlight !== null

    function finishSettings(callback) { sunshineEditor.finish(callback, true) }

    SunshineEditor {
        id: sunshineEditor
        state: page.hostState.sunshine || ({})
        onStateChanged: loadIfNeeded(page.visible)
        onRequest: function(action, data) { page.sunshineRequested(action, data) }
        onConfirmDiscard: discardDialog.open()
    }

    readonly property int inset: compact ? 8 : narrow ? 13 : 21
    readonly property int panelInset: compact ? 8 : 14
    readonly property bool busy: hostState.busy === true
    readonly property bool allowed: hostState.allowed === true
    readonly property bool running: hostState.status === "running"
    readonly property bool canToggle: hostState.toggleAvailable === true && !busy

    function statusText(status) {
        switch (status) {
        case "running": return qsTr("运行中")
        case "starting": return qsTr("正在启动")
        case "stopping": return qsTr("正在关闭")
        case "preparing": return qsTr("正在就绪")
        case "notReady": return qsTr("未就绪")
        case "disabled": return qsTr("未启用")
        case "unknown": return qsTr("状态未知")
        default: return qsTr("正在检测")
        }
    }

    onCanToggleChanged: {
        if (!canToggle && stopDialog.visible)
            stopDialog.close()
    }
    onAllowedChanged: {
        if (!allowed && stopDialog.visible)
            stopDialog.close()
    }
    onVisibleChanged: {
        sunshineEditor.loadIfNeeded(visible)
        if (!visible) {
            advanced = false
            if (stopDialog.visible)
                stopDialog.close()
        }
    }

    component HostText: Text {
        font.family: AuroraTheme.font
        font.pixelSize: 13
        color: AuroraTheme.ink
        textFormat: Text.PlainText
    }

    component Panel: Rectangle {
        color: "white"
        radius: 11
        border.color: AuroraTheme.panelLine
    }

    component Address: Item {
        id: address
        property string label
        property string value
        property string copyLabel
        property string copyAction
        property alias copyObjectName: copyButton.objectName
        implicitHeight: Math.max(34, addressText.implicitHeight) + 18

        Column {
            id: addressText
            anchors.left: parent.left
            anchors.right: copyButton.left
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1

            HostText {
                text: address.label
                font.pixelSize: 12
                color: AuroraTheme.muted
            }
            HostText {
                width: parent.width
                text: address.value
                font.family: AuroraTheme.windows ? "Consolas" : AuroraTheme.mac ? "Menlo" : "monospace"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                wrapMode: Text.WrapAnywhere
            }
        }
        AuroraButton {
            id: copyButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 34
            height: 34
            quiet: true
            text: address.copyLabel
            hint: text
            icon.source: AuroraTheme.icon("copy")
            display: AbstractButton.IconOnly
            enabled: !page.busy
            onClicked: page.actionRequested(address.copyAction)
        }
    }

    Item {
        id: heading
        anchors.top: parent.top
        anchors.topMargin: page.narrow ? 16 : 20
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: page.inset
        anchors.rightMargin: page.inset
        height: 34

        HostText {
            objectName: "hostPageTitle"
            anchors.verticalCenter: parent.verticalCenter
            text: page.advanced ? qsTr("全部配置项") : qsTr("本机被控")
            font.pixelSize: page.compact ? 21 : 26
            font.weight: AuroraTheme.headingWeight
            Accessible.role: Accessible.Heading
        }
        AuroraRefreshButton {
            id: refresh
            objectName: "refreshHost"
            visible: !page.advanced
            anchors.right: parent.right
            available: page.hostState.refreshAvailable === true && !page.busy
            refreshing: page.hostState.refreshing === true
            reduceMotion: page.reduceMotion
            idleLabel: qsTr("重新检测本机被控")
            busyLabel: qsTr("正在检测本机被控")
            lastFailure: page.hostState.refreshFailed === true ? page.hostState.refreshFeedback || "" : ""
            onRefreshRequested: page.actionRequested("hostRefresh")
        }
    }

    AuroraRefreshFeedback {
        id: feedbackSlot
        visible: !page.advanced
        anchors.top: heading.bottom
        anchors.topMargin: 15
        anchors.left: heading.left
        anchors.right: heading.right
        pending: refresh.showingProgress
        message: page.hostState.refreshFeedback || ""
        failed: page.hostState.refreshFailed === true
        revision: page.hostState.refreshRevision || 0
        reduceMotion: page.reduceMotion
    }

    SunshineLink {
        objectName: "sunshineBack"
        visible: page.advanced
        anchors.right: heading.right
        anchors.rightMargin: advancedOptions.scrollGutter
        anchors.verticalCenter: heading.verticalCenter
        text: qsTr("本机被控")
        icon.source: AuroraTheme.icon("arrow-left")
        leadingIcon: true
        onClicked: sunshineEditor.finish(function() { page.advanced = false }, true)
    }

    SunshineAdvanced {
        id: advancedOptions
        anchors.top: heading.bottom
        anchors.topMargin: 6
        anchors.left: heading.left
        anchors.right: heading.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        visible: page.advanced
        inset: page.panelInset
        editor: sunshineEditor
    }

    Flickable {
        id: scroll
        visible: !page.advanced
        anchors.top: feedbackSlot.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: width
        contentHeight: panels.height + page.inset
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: panels
            x: page.inset
            width: parent.width - 2 * page.inset
            spacing: 12

            Panel {
                objectName: "hostSummary"
                width: parent.width
                height: summary.implicitHeight

                Column {
                    id: summary
                    x: page.panelInset
                    width: parent.width - 2 * x

                    RowLayout {
                        width: parent.width
                        height: 54
                        spacing: 10

                        AuroraGlyph {
                            objectName: "hostComputerIcon"
                            implicitWidth: 28
                            implicitHeight: 28
                            icon.source: AuroraTheme.desktopAsset("panel-top-tight.svg")
                        }
                        HostText {
                            objectName: "hostComputerTitle"
                            Layout.fillWidth: true
                            text: qsTr("这台电脑")
                            font.pixelSize: 15
                            font.weight: AuroraTheme.headingWeight
                            Accessible.role: Accessible.Heading
                        }
                        RowLayout {
                            spacing: 6

                            Rectangle {
                                Layout.preferredWidth: 7
                                Layout.preferredHeight: 7
                                radius: width / 2
                                color: page.running ? AuroraTheme.statusBlue : "transparent"
                                border.color: page.running ? AuroraTheme.statusBlue : AuroraTheme.muted
                            }
                            HostText {
                                objectName: "hostStatus"
                                text: page.statusText(page.hostState.status)
                                font.pixelSize: 12
                                color: page.running ? AuroraTheme.statusBlue : AuroraTheme.muted
                            }
                        }
                    }

                    Button {
                        id: allowSwitch
                        objectName: "allowHost"
                        width: parent.width
                        height: 45
                        visible: page.hostState.managed === true
                        enabled: page.canToggle
                        text: qsTr("允许本机被控")
                        Accessible.role: Accessible.CheckBox
                        Accessible.name: text
                        Accessible.checkable: true
                        Accessible.checked: page.allowed
                        hoverEnabled: true
                        padding: 0
                        background: null
                        contentItem: Item {
                            HostText {
                                anchors.verticalCenter: parent.verticalCenter
                                text: allowSwitch.text
                                Accessible.ignored: true
                            }
                            Rectangle {
                                width: 32
                                height: 18
                                radius: 9
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                color: page.allowed ? AuroraTheme.blue : "#F0F3F7"
                                border.color: page.allowed ? AuroraTheme.blue : "#8E9BB0"
                                opacity: allowSwitch.enabled ? 1 : 0.55

                                Rectangle {
                                    width: 12
                                    height: 12
                                    radius: 6
                                    x: page.allowed ? 17 : 3
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: page.allowed ? "white" : "#6D7C92"
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    visible: allowSwitch.visualFocus
                                    color: "transparent"
                                    border.color: AuroraTheme.blue
                                    border.width: 2
                                    radius: 12
                                }
                            }
                        }
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: {
                            if (!page.canToggle)
                                return
                            if (page.allowed)
                                stopDialog.open()
                            else
                                page.actionRequested("hostEnable")
                        }
                    }

                    GridLayout {
                        objectName: "hostAddresses"
                        width: parent.width
                        height: implicitHeight
                        columns: width < 420 ? 1 : 2
                        columnSpacing: 2 * page.panelInset
                        rowSpacing: 0
                        visible: lan.value.length > 0 || tail.value.length > 0

                        Address {
                            id: lan
                            objectName: "hostLan"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop
                            visible: value.length > 0
                            label: qsTr("局域网")
                            value: page.hostState.lanAddress || ""
                            copyLabel: qsTr("复制局域网地址")
                            copyAction: "hostCopyLan"
                            copyObjectName: "hostCopyLan"
                        }
                        Address {
                            id: tail
                            objectName: "hostTailnet"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop
                            visible: value.length > 0
                            label: "Tailscale"
                            value: page.hostState.tailscaleAddress || ""
                            copyLabel: qsTr("复制 Tailscale 地址")
                            copyAction: "hostCopyTailnet"
                            copyObjectName: "hostCopyTailnet"
                        }
                    }
                    HostText {
                        width: parent.width
                        topPadding: page.panelInset
                        bottomPadding: page.panelInset
                        visible: text.length > 0
                        text: page.hostState.hint || ""
                        color: AuroraTheme.muted
                        wrapMode: Text.Wrap
                    }
                    AuroraButton {
                        anchors.right: parent.right
                        objectName: "hostRetry"
                        visible: page.hostState.retryAvailable === true
                        enabled: page.hostState.refreshAvailable === true && !page.busy
                        primary: true
                        text: qsTr("重试启动")
                        onClicked: page.actionRequested("hostRetry")
                    }
                }
            }

            Panel {
                objectName: "hostPermissions"
                width: parent.width
                height: permissions.implicitHeight + 28
                visible: page.hostState.permissionsAvailable === true

                Column {
                    id: permissions
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: page.panelInset
                    anchors.rightMargin: page.panelInset
                    anchors.topMargin: 14
                    spacing: 7

                    HostText {
                        width: parent.width
                        text: qsTr("允许极光控制这台 Mac")
                        font.pixelSize: 15
                        font.weight: AuroraTheme.headingWeight
                        Accessible.role: Accessible.Heading
                    }
                    HostText {
                        width: parent.width
                        text: qsTr("请在 macOS 隐私设置中允许 Aurora Host 录制屏幕并使用辅助功能。授权后可重启被控组件。")
                        color: AuroraTheme.muted
                        wrapMode: Text.Wrap
                    }
                    SunshineLink {
                        objectName: "hostOpenPermissions"
                        enabled: !page.busy && !page.hostState.refreshing
                        text: qsTr("打开 macOS 隐私设置")
                        onClicked: page.actionRequested("hostOpenPermissions")
                    }
                    AuroraButton {
                        objectName: "hostContinuePermissions"
                        primary: true
                        enabled: page.hostState.refreshAvailable === true && !page.busy
                            && !page.hostState.refreshing && !page.hostState.permissionsRetrying && !sunshineEditor.continuation
                        text: page.hostState.permissionsRetrying ? qsTr("正在重试…") : qsTr("已授权，继续")
                        onClicked: sunshineEditor.finish(function() { page.actionRequested("hostContinuePermissions") }, false)
                    }
                    HostText {
                        objectName: "hostPermissionsFeedback"
                        width: parent.width
                        visible: text.length > 0
                        text: page.hostState.permissionsFeedback || ""
                        color: page.hostState.permissionsFailed ? "#9C2B2B" : AuroraTheme.muted
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StaticText
                    }
                }
            }

            SunshineSettings {
                width: parent.width
                visible: !!sunshineEditor.state.status
                active: page.visible && !page.advanced
                inset: page.panelInset
                editor: sunshineEditor
                onAdvancedRequested: page.advanced = true
                onRestartRequested: sunshineEditor.finish(function() { restartDialog.open() }, false)
            }

            HostText {
                objectName: "hostFeedback"
                width: parent.width
                visible: text.length > 0
                text: page.hostState.feedback || ""
                color: page.hostState.feedbackFailed ? "#9C2B2B" : AuroraTheme.muted
                wrapMode: Text.Wrap
            }
        }
    }

    SunshineConfirm {
        id: restartDialog
        objectName: "sunshineRestartDialog"
        heading: qsTr("要重启 Sunshine 吗？")
        message: qsTr("重启 Sunshine 会断开正在进行的远控会话。")
        action: qsTr("确认重启")
        onAccepted: sunshineEditor.command("sunshineRestart")
    }

    SunshineConfirm {
        id: discardDialog
        objectName: "sunshineDiscardDialog"
        heading: qsTr("要放弃未保存的设置吗？")
        message: qsTr("有 Sunshine 设置未能保存。取消后可继续编辑或重试。")
        action: qsTr("放弃改动")
        onAccepted: sunshineEditor.discard()
        onCanceled: sunshineEditor.continuation = null
    }

    Popup {
        id: stopDialog
        objectName: "hostStopDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(412, parent.width - 32)
        padding: parent.width <= 420 ? 16 : 22
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        onOpened: cancelStop.forceActiveFocus(Qt.TabFocusReason)
        onClosed: {
            if (page.visible && allowSwitch.enabled)
                allowSwitch.forceActiveFocus(Qt.TabFocusReason)
        }

        Overlay.modal: Rectangle { color: "#5919283F" }
        background: Rectangle {
            radius: 13
            color: "white"
            border.color: AuroraTheme.panelLine
        }
        contentItem: ColumnLayout {
            spacing: 10

            HostText {
                Layout.fillWidth: true
                text: qsTr("要关闭本机被控吗？")
                font.pixelSize: 21
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                Accessible.role: Accessible.Heading
            }
            HostText {
                Layout.fillWidth: true
                text: qsTr("关闭被控会断开正在进行的远控会话。")
                color: AuroraTheme.muted
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Layout.topMargin: 10
                spacing: 8

                AuroraButton {
                    id: cancelStop
                    objectName: "hostCancelStop"
                    Layout.minimumWidth: 72
                    text: qsTr("取消")
                    background: Rectangle {
                        radius: 7
                        color: cancelStop.hovered ? AuroraTheme.hover : "white"
                        border.color: cancelStop.visualFocus ? AuroraTheme.blue : AuroraTheme.panelLine
                        border.width: cancelStop.visualFocus ? 2 : 1
                    }
                    onClicked: stopDialog.close()
                }
                AuroraButton {
                    id: applyStop
                    objectName: "hostApplyStop"
                    Layout.minimumWidth: 76
                    text: qsTr("关闭被控")
                    foreground: "#9C2B2B"
                    background: Rectangle {
                        radius: 7
                        color: applyStop.hovered || applyStop.visualFocus ? "#F6D7D7" : "#FBE7E7"
                        border.width: applyStop.visualFocus ? 2 : 0
                        border.color: AuroraTheme.blue
                    }
                    onClicked: {
                        stopDialog.close()
                        if (page.canToggle && page.allowed)
                            sunshineEditor.finish(function() { page.actionRequested("hostDisable") }, true)
                    }
                }
            }
        }
    }
}
