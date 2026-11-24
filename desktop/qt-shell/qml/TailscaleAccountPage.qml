// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

Item {
    id: page
    objectName: "tailscaleAccountPage"

    property var account: ({})
    property bool compact: false
    property bool narrow: false
    property bool reduceMotion: false
    property url avatarPhoto
    signal actionRequested(string action, string target, Item source)
    readonly property bool actionsAvailable: account.refreshAvailable === true && !account.busy

    readonly property var modes: {
        const values = []
        if (account.supportsEmbedded)
            values.push({key: "embedded", label: qsTr("内置 Tailscale")})
        if (account.hasSystemRuntime)
            values.push({key: "system", label: qsTr("系统 Tailscale")})
        return values
    }
    readonly property var profiles: account.profiles || []
    readonly property bool canPickProfiles: account.mode === "system" && profiles.length > 0
    readonly property int accountRevision: account.revision || 0
    onAccountRevisionChanged: if (profileMenu && profileMenu.opened) profileMenu.close()
    readonly property int focusVersion: account.focusVersion || 0
    onFocusVersionChanged: if (focusVersion && primaryAction && primaryAction.visible && primaryAction.enabled) primaryAction.forceActiveFocus()
    onCanPickProfilesChanged: if (!canPickProfiles && profileMenu.opened) profileMenu.close()
    onAccountChanged: if (profileMenu && account.busy && profileMenu.opened) profileMenu.close()

    readonly property int inset: compact ? 8 : narrow ? 13 : 21
    readonly property int verticalInset: narrow ? 16 : 20
    readonly property bool connected: account.state === "Connected"
    readonly property bool refreshing: account.refreshing === true
    readonly property color statusColor: connected ? AuroraTheme.statusBlue :
        ["NeedsLogin", "NeedsApproval", "Starting"].indexOf(account.state) >= 0 ? "#906016" : "#59667A"

    component Body: Text {
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: AuroraTheme.font
        font.pixelSize: 13
        color: AuroraTheme.muted
    }

    component Card: Control {
        leftPadding: 14
        rightPadding: 14
        topPadding: 12
        bottomPadding: 12
        implicitHeight: implicitContentHeight + topPadding + bottomPadding
        background: Rectangle {
            radius: 11
            color: "white"
            border.color: AuroraTheme.panelLine
        }
    }

    component ConnectionField: Column {
        property string label
        property string value
        spacing: 4
        Body { width: parent.width; text: parent.label }
        Body {
            width: parent.width
            text: parent.value
            color: AuroraTheme.ink
            font.weight: Font.Medium
        }
    }

    component OperationStatus: Column {
        id: operation
        property string progress: ""
        property string message: ""
        property bool retryAvailable: false
        visible: progress.length > 0 || message.length > 0 || retryAvailable
        spacing: 4

        RowLayout {
            width: parent.width
            visible: operation.progress.length > 0
            spacing: 6
            AuroraGlyph {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                icon.width: 14
                icon.height: 14
                icon.source: AuroraTheme.icon("refresh-cw")
                icon.color: AuroraTheme.blue
                RotationAnimator on rotation {
                    from: 0
                    to: 360
                    duration: 850
                    loops: Animation.Infinite
                    running: visible && !page.reduceMotion
                }
            }
            Body { Layout.fillWidth: true; text: operation.progress }
        }
        Body {
            width: parent.width
            text: operation.message
            visible: text.length > 0
            color: operation.retryAvailable ? "#A13F39" : AuroraTheme.muted
        }
        AuroraButton {
            id: retry
            objectName: "tailscaleRetry"
            text: qsTr("重试")
            visible: operation.retryAvailable
            quiet: true
            onClicked: page.actionRequested("tailscale.retry", "", retry)
        }
    }

    Item {
        id: heading
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: page.verticalInset
        anchors.leftMargin: page.inset
        anchors.rightMargin: page.inset
        height: 34

        Body {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Tailscale 账户")
            color: AuroraTheme.ink
            font.pixelSize: page.compact ? 21 : 26
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }

        AuroraButton {
            id: refresh
            objectName: "refreshTailscale"
            anchors.right: parent.right
            width: 34
            height: 34
            quiet: true
            hoverFill: "transparent"
            enabled: page.actionsAvailable && !page.refreshing
            text: page.refreshing ? qsTr("正在检查 Tailscale 状态") : qsTr("刷新 Tailscale 状态")
            hint: text
            display: AbstractButton.IconOnly
            icon.width: 20
            icon.height: 20
            icon.source: page.refreshing ? "" : AuroraTheme.icon("refresh-cw")
            onClicked: page.actionRequested("tailscale.refresh", "", refresh)

            Canvas {
                anchors.centerIn: parent
                width: 18
                height: 18
                visible: page.refreshing
                onPaint: {
                    const context = getContext("2d")
                    context.reset()
                    context.strokeStyle = AuroraTheme.blue
                    context.lineWidth = 2
                    context.lineCap = "round"
                    context.beginPath()
                    context.arc(9, 9, 7, 0, Math.PI * 1.55)
                    context.stroke()
                }
                RotationAnimator on rotation {
                    from: 0
                    to: 360
                    duration: 850
                    loops: Animation.Infinite
                    running: page.refreshing && !page.reduceMotion
                }
            }
        }
    }

    Flickable {
        anchors.top: heading.bottom
        anchors.topMargin: 15
        anchors.left: heading.left
        anchors.right: heading.right
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: cards.height + page.verticalInset
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { width: 7 }

        Column {
            id: cards
            width: parent.width
            spacing: 12

            Card {
                objectName: "tailscaleRuntimeCard"
                width: parent.width
                contentItem: GridLayout {
                    id: runtime
                    columns: page.narrow ? 1 : 2
                    rowSpacing: 8
                    columnSpacing: 12

                    Body {
                        text: qsTr("运行方式")
                        color: AuroraTheme.ink
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        Layout.fillWidth: true
                    }

                    AuroraSettingChoice {
                        id: modeField
                        objectName: "tailscaleRuntimeField"
                        Layout.fillWidth: page.narrow
                        Layout.preferredWidth: 235
                        enabled: page.actionsAvailable && page.modes.length > 0
                        Accessible.name: qsTr("运行方式")
                        model: page.modes.map(function(mode) { return mode.label })
                        selectedIndex: page.modes.findIndex(function(mode) { return mode.key === page.account.mode })
                        displayText: page.account.modeLabel || "—"
                        onSelectionRequested: function(index) {
                            page.actionRequested("tailscale.mode", page.modes[index].key, modeField)
                        }
                    }

                    Body {
                        objectName: "tailscaleRuntimeNotice"
                        Layout.columnSpan: runtime.columns
                        Layout.fillWidth: true
                        text: page.account.runtimeNotice || ""
                        color: "#906016"
                        visible: text.length > 0
                    }

                    OperationStatus {
                        Layout.columnSpan: runtime.columns
                        Layout.fillWidth: true
                        progress: page.account.runtimeProgress ? page.account.progress || "" : ""
                        message: page.account.runtimeFeedback ? page.account.feedback || "" : ""
                        retryAvailable: page.account.runtimeFeedback === true && page.account.retryAvailable === true
                    }
                }
            }

            Card {
                objectName: "tailscaleIdentityCard"
                width: parent.width
                topPadding: 13
                bottomPadding: 13
                contentItem: ColumnLayout {
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Item {
                            id: identityArea
                            Layout.fillWidth: true
                            implicitHeight: identityRow.implicitHeight

                            Rectangle {
                                anchors.fill: parent
                                radius: 7
                                color: profileMenu.opened ? AuroraTheme.selected :
                                    profileTrigger.hovered ? AuroraTheme.hover : "transparent"
                                border.color: AuroraTheme.blue
                                border.width: profileTrigger.visualFocus ? 2 : 0
                            }

                            RowLayout {
                                id: identityRow
                                anchors.fill: parent
                                spacing: 10

                                AuroraAccountAvatar {
                                    Layout.preferredWidth: 40
                                    Layout.preferredHeight: 40
                                    Layout.alignment: Qt.AlignTop
                                    photo: page.connected ? page.avatarPhoto : ""
                                    initial: page.account.initial || ""
                                }

                                Column {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    Flow {
                                        width: parent.width
                                        spacing: 6

                                        Body {
                                            objectName: "tailscaleDisplayName"
                                            width: Math.min(implicitWidth, parent.width)
                                            wrapMode: Text.NoWrap
                                            elide: Text.ElideRight
                                            text: page.account.displayName || "Tailscale"
                                            color: AuroraTheme.ink
                                            font.pixelSize: 16
                                            font.weight: Font.DemiBold
                                        }

                                        Row {
                                            height: nameStatus.implicitHeight + 3
                                            spacing: 4
                                            Rectangle {
                                                width: 6
                                                height: 6
                                                radius: 3
                                                anchors.verticalCenter: parent.verticalCenter
                                                border.color: page.statusColor
                                                color: page.connected ? page.statusColor : "transparent"
                                            }
                                            Body {
                                                id: nameStatus
                                                objectName: "tailscaleStatus"
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: page.account.status || ""
                                                color: page.statusColor
                                            }
                                        }
                                    }

                                    Body {
                                        objectName: "tailscaleLoginName"
                                        width: parent.width
                                        text: page.account.loginName || ""
                                        visible: text.length > 0
                                    }
                                }

                                AuroraGlyph {
                                    Layout.preferredWidth: 14
                                    Layout.preferredHeight: 14
                                    icon.width: 14
                                    icon.height: 14
                                    visible: page.canPickProfiles
                                    icon.source: AuroraTheme.icon("chevron-down")
                                    rotation: profileMenu.opened ? 180 : 0
                                }
                            }

                            AuroraButton {
                                id: profileTrigger
                                objectName: "tailscaleProfiles"
                                anchors.fill: parent
                                visible: page.canPickProfiles
                                enabled: page.actionsAvailable
                                Accessible.name: qsTr("选择系统 Tailscale 账号")
                                background: null
                                onClicked: profileMenu.opened ? profileMenu.close() : profileMenu.open()
                            }

                            TailscaleProfileMenu {
                                id: profileMenu
                                y: identityArea.height + 4
                                width: Math.min(cards.width - 28, Math.max(identityArea.width, 250))
                                maximumHeight: Math.max(110, Math.min(285, page.height - 26))
                                profiles: page.profiles
                                onClosed: if (profileTrigger.visible && profileTrigger.enabled) profileTrigger.forceActiveFocus()
                                onProfileRequested: function(key) { page.actionRequested("tailscale.profile", key, profileTrigger) }
                            }
                        }

                        AuroraButton {
                            id: logout
                            objectName: "tailscaleLogout"
                            Layout.alignment: Qt.AlignTop
                            visible: page.connected
                            enabled: page.actionsAvailable
                            quiet: true
                            text: qsTr("退出登录")
                            hint: page.narrow ? text : ""
                            display: page.narrow ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
                            icon.source: AuroraTheme.icon("log-out")
                            onClicked: page.actionRequested("tailscale.logout", "", logout)
                        }
                    }

                    GridLayout {
                        visible: page.connected
                        Layout.fillWidth: true
                        columns: page.narrow ? 1 : 2
                        columnSpacing: 12
                        rowSpacing: 8

                        ConnectionField {
                            objectName: "tailscaleNetwork"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            label: qsTr("所属网络")
                            value: page.account.tailnetName || ""
                        }
                        ConnectionField {
                            objectName: "tailscaleAddress"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            label: qsTr("本机 Tailscale 地址")
                            value: page.account.localAddress || ""
                        }
                    }

                    Body {
                        objectName: "tailscaleStatusDetail"
                        Layout.fillWidth: true
                        text: page.account.detail || ""
                        visible: !page.connected && text.length > 0
                    }

                    OperationStatus {
                        Layout.fillWidth: true
                        progress: !page.account.runtimeProgress ? page.account.progress || "" : ""
                        message: !page.account.runtimeFeedback ? page.account.feedback || "" : ""
                        retryAvailable: !page.account.runtimeFeedback && page.account.retryAvailable === true
                    }

                    AuroraButton {
                        id: primaryAction
                        objectName: "tailscaleAuthorization"
                        text: page.account.primaryAction || ""
                        primary: true
                        visible: text.length > 0
                        enabled: page.account.canOpenPrimaryAction === true && !page.refreshing
                        onClicked: page.actionRequested("tailscale.primary", "", primaryAction)
                        Component.onCompleted: if (visible && enabled) forceActiveFocus(Qt.TabFocusReason)
                    }
                }
            }

            AuroraButton {
                id: help
                objectName: "tailscaleHelp"
                text: qsTr("连接帮助")
                quiet: true
                checkable: true
                contentItem: Row {
                    spacing: 5
                    Body {
                        anchors.verticalCenter: parent.verticalCenter
                        text: help.text
                        color: help.foreground
                    }
                    AuroraGlyph {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 15
                        height: 15
                        icon.source: AuroraTheme.icon("chevron-down")
                        icon.color: help.foreground
                        rotation: help.checked ? 180 : 0
                    }
                }
            }

            Body {
                width: parent.width
                visible: help.checked
                text: qsTr("在两台设备上登录同一 Tailscale 账号，回到「我的设备」连接电脑。"
                           + "如果设备在线但无法连接，检查对端的 Sunshine 是否已运行。")
            }
        }
    }
}