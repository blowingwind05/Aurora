// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import "../../common"

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
    readonly property bool canPickProfiles: account.mode === "system" &&
        profiles.some(function(profile) { return !profile.current })
    readonly property int accountRevision: account.revision || 0
    onAccountRevisionChanged: if (profileMenu && profileMenu.opened) profileMenu.close()
    readonly property int focusVersion: account.focusVersion || 0
    onFocusVersionChanged: if (focusVersion) Qt.callLater(function() {
        if (primaryAction && primaryAction.visible && primaryAction.enabled)
            primaryAction.forceActiveFocus()
    })
    onCanPickProfilesChanged: if (!canPickProfiles && profileMenu && profileMenu.opened) profileMenu.close()
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
            font.weight: AuroraTheme.headingWeight
        }
    }

    component LoadingRing: Canvas {
        id: ring
        onPaint: {
            var brush = getContext("2d")
            brush.reset()
            var center = width / 2
            var radius = center - 1
            brush.lineWidth = 2
            brush.lineCap = "round"
            brush.strokeStyle = "#D6E4FA"
            brush.beginPath()
            brush.arc(center, center, radius, 0, Math.PI * 2)
            brush.stroke()
            brush.strokeStyle = AuroraTheme.blue
            brush.beginPath()
            brush.arc(center, center, radius, 0, Math.PI * 0.82)
            brush.stroke()
        }
        RotationAnimator on rotation {
            from: 0
            to: 360
            duration: 850
            loops: Animation.Infinite
            running: ring.visible && !page.reduceMotion
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
            LoadingRing {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
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
            font.weight: AuroraTheme.headingWeight
            Accessible.role: Accessible.Heading
        }

        AuroraRefreshButton {
            id: refresh
            objectName: "refreshTailscale"
            anchors.right: parent.right
            available: page.actionsAvailable && page.account.refreshEnabled !== false
            refreshing: page.refreshing
            reduceMotion: page.reduceMotion
            idleLabel: qsTr("刷新 Tailscale 状态")
            busyLabel: qsTr("正在检查 Tailscale 状态")
            lastFailure: page.account.refreshFailed === true ? page.account.refreshFeedback || "" : ""
            onRefreshRequested: page.actionRequested("tailscale.refresh", "", refresh)
        }
    }

    AuroraRefreshFeedback {
        id: feedbackSlot
        anchors.top: heading.bottom
        anchors.topMargin: 15
        anchors.left: heading.left
        anchors.right: heading.right
        pending: refresh.showingProgress
        message: page.account.refreshFeedback || ""
        failed: page.account.refreshFailed === true
        revision: page.account.refreshRevision || 0
        reduceMotion: page.reduceMotion
    }

    Flickable {
        anchors.top: feedbackSlot.bottom
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
                        font.weight: AuroraTheme.headingWeight
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
                        color: AuroraTheme.muted
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

                    Item {
                        Layout.fillWidth: true
                        implicitHeight: Math.max(identitySlot.height, logout.visible ? logout.height : 0)

                        Item {
                            id: identitySlot
                            anchors.left: parent.left
                            anchors.right: logout.visible ? logout.left : parent.right
                            anchors.rightMargin: logout.visible ? 8 : 0
                            height: identityArea.height + (statusBelow ? statusBadge.height + 5 : 0)
                            readonly property int avatarExtent: 40 + (page.narrow ? 8 : 10)
                            readonly property real identityWidth: avatarExtent + Math.ceil(Math.max(
                                nameWidth.advanceWidth, loginWidth.advanceWidth, switchLabel.implicitWidth))
                            readonly property real statusStart: (page.canPickProfiles ? identityWidth
                                : avatarExtent + Math.ceil(nameWidth.advanceWidth)) + 8
                            readonly property bool statusBelow: statusBadge.visible &&
                                statusStart + statusBadge.implicitWidth > width

                            TextMetrics { id: nameWidth; text: displayName.text; font: displayName.font }
                            TextMetrics { id: loginWidth; text: loginName.text; font: loginName.font }

                            Item {
                                id: identityArea
                                width: Math.min(identitySlot.width, identitySlot.identityWidth)
                                height: Math.max(40, profileText.implicitHeight)

                                AuroraAccountAvatar {
                                    width: 40
                                    height: 40
                                    anchors.verticalCenter: parent.verticalCenter
                                    photo: page.connected ? page.avatarPhoto : ""
                                    initial: page.account.initial || ""
                                }

                                Column {
                                    id: profileText
                                    x: identitySlot.avatarExtent
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.max(0, identityArea.width - x)
                                    spacing: 5

                                    Body {
                                        id: displayName
                                        objectName: "tailscaleDisplayName"
                                        width: parent.width
                                        wrapMode: Text.NoWrap
                                        elide: Text.ElideRight
                                        text: page.account.displayName || "Tailscale"
                                        color: AuroraTheme.ink
                                        font.pixelSize: 16
                                        font.weight: AuroraTheme.headingWeight
                                    }
                                    Body {
                                        id: loginName
                                        objectName: "tailscaleLoginName"
                                        width: parent.width
                                        wrapMode: Text.NoWrap
                                        elide: Text.ElideRight
                                        text: page.account.loginName || ""
                                        visible: text.length > 0
                                        font.pixelSize: 12
                                        font.weight: Font.Normal
                                    }
                                    Body {
                                        id: switchLabel
                                        width: parent.width
                                        visible: page.narrow && page.canPickProfiles
                                        text: visible ? qsTr("切换账号") : ""
                                        color: profileTrigger.enabled ? AuroraTheme.statusBlue : AuroraTheme.muted
                                        font.pixelSize: 12
                                    }
                                }

                                AuroraButton {
                                    id: profileTrigger
                                    objectName: "tailscaleProfiles"
                                    anchors.fill: parent
                                    visible: page.canPickProfiles
                                    enabled: page.actionsAvailable
                                    focusPolicy: Qt.StrongFocus
                                    Accessible.name: qsTr("切换系统 Tailscale 账号")
                                    Accessible.description: [displayName.text, loginName.text].filter(Boolean).join("，")
                                    hint: displayName.truncated || loginName.truncated
                                        ? qsTr("切换账号") + "\n" + Accessible.description : qsTr("切换账号")
                                    HoverHandler { enabled: profileTrigger.enabled; cursorShape: Qt.PointingHandCursor }
                                    background: Rectangle {
                                        radius: 7
                                        color: profileMenu.opened ? AuroraTheme.selected :
                                            profileTrigger.hovered && profileTrigger.enabled ? AuroraTheme.hover : "transparent"
                                        border.color: AuroraTheme.blue
                                        border.width: profileTrigger.visualFocus ? 2 : 0
                                    }
                                    z: -1
                                    onClicked: profileMenu.opened ? profileMenu.close() : profileMenu.open()
                                }

                                TailscaleProfileMenu {
                                    id: profileMenu
                                    property real triggerY: 0
                                    y: triggerY + identityArea.height + height + 10 <= page.Window.height
                                        ? identityArea.height + 4 : -height - 4
                                    width: Math.min(cards.width - 28, 340)
                                    maximumHeight: Math.max(96, Math.min(285, page.Window.height - 12))
                                    profiles: page.profiles
                                    onAboutToShow: triggerY = identityArea.mapToItem(null, 0, 0).y
                                    onClosed: if (profileTrigger.visible && profileTrigger.enabled) profileTrigger.forceActiveFocus()
                                    onProfileRequested: function(key) { page.actionRequested("tailscale.profile", key, profileTrigger) }
                                }
                            }

                            Row {
                                id: statusBadge
                                x: identitySlot.statusBelow ? 0 : identitySlot.statusStart
                                y: identitySlot.statusBelow ? identityArea.height + 5 : profileText.y
                                visible: nameStatus.text.length > 0
                                height: displayName.implicitHeight
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
                                    y: displayName.baselineOffset - baselineOffset
                                    text: page.account.status || ""
                                    color: page.statusColor
                                }
                            }
                        }

                        AuroraButton {
                            id: logout
                            objectName: "tailscaleLogout"
                            anchors.right: parent.right
                            y: Math.max(0, (identityArea.height - height) / 2)
                            width: page.narrow ? 34 : logoutContent.implicitWidth + 8
                            visible: page.connected
                            enabled: page.actionsAvailable
                            quiet: true
                            hoverFill: "transparent"
                            text: qsTr("退出登录")
                            hint: page.narrow ? text : ""

                            contentItem: Item {
                                Row {
                                    id: logoutContent
                                    anchors.centerIn: parent
                                    spacing: 5

                                    AuroraGlyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 16
                                        height: 16
                                        icon.source: AuroraTheme.icon("log-out")
                                        icon.color: logout.foreground
                                    }
                                    Body {
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: !page.narrow
                                        text: logout.text
                                        color: logout.foreground
                                    }
                                }
                            }

                            onClicked: page.actionRequested("tailscale.logout", "", logout)
                        }
                    }

                    GridLayout {
                        visible: page.connected
                        Layout.fillWidth: true
                        Layout.topMargin: page.narrow ? 4 : 8
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
                        Layout.alignment: Qt.AlignRight
                        text: page.account.primaryAction || ""
                        font.weight: AuroraTheme.headingWeight
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
                hoverFill: "transparent"
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