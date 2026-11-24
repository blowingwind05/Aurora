// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

Item {
    id: page
    property var deviceModel: null
    property string accessState: "loading"
    property bool compact: false
    property bool narrow: false
    property bool refreshAvailable: true
    property bool refreshing: false
    property bool connectionPending: false
    property bool reduceMotion: false
    property string refreshFeedback: ""
    property bool refreshFailed: false
    property var enabledDeviceTools: ({})
    signal refreshRequested()
    signal connectRequested(string deviceId)
    signal toolRequested(string deviceId, string action)
    signal accountRequested()

    readonly property bool ready: accessState === "ready"
    readonly property int inset: compact ? 8 : narrow ? 13 : 21
    readonly property int verticalInset: narrow ? 16 : 20
    readonly property int columns: contentWidth >= 1450 ? 4 : contentWidth >= 1000 ? 3 : contentWidth > 580 ? 2 : 1
    readonly property real contentWidth: width - 2 * inset
    readonly property var defaultWallpapers: [
        "mountain-valley.webp", "forest-path.webp", "blossom-grove.webp", "ginkgo-lakeside.webp",
        "baikal-piano.webp", "baikal-accordion.webp", "sunflower-field.webp", "sunset-sky.webp"
    ]

    property bool feedbackVisible: false

    function updateFeedback() {
        feedbackVisible = ready && !refreshing && refreshFeedback.length > 0
        feedbackTimer.stop()
        if (feedbackVisible && !refreshFailed)
            feedbackTimer.start()
    }

    onRefreshFeedbackChanged: updateFeedback()
    onRefreshFailedChanged: updateFeedback()
    onRefreshingChanged: updateFeedback()
    onReadyChanged: updateFeedback()
    Component.onCompleted: updateFeedback()

    Timer {
        id: feedbackTimer
        interval: 3000
        onTriggered: page.feedbackVisible = false
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

        Text {
            objectName: "devicesPageTitle"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("我的设备")
            font.family: AuroraTheme.font
            font.pixelSize: page.compact ? 21 : 26
            font.weight: Font.Bold
            color: AuroraTheme.ink
            Accessible.role: Accessible.Heading
        }

        AuroraButton {
            id: refresh
            objectName: "refreshDevices"
            anchors.right: parent.right
            width: 34
            height: 34
            quiet: true
            hoverFill: "transparent"
            visible: page.ready
            enabled: page.refreshAvailable && !page.refreshing
            text: page.refreshing ? qsTr("正在刷新设备列表") : qsTr("刷新设备列表")
            hint: text
            display: AbstractButton.IconOnly
            icon.width: 20
            icon.height: 20
            icon.source: page.refreshing ? "" : AuroraTheme.icon("refresh-cw")
            onClicked: page.refreshRequested()

            Canvas {
                id: spinner
                width: 18
                height: width
                anchors.centerIn: parent
                visible: page.refreshing
                onPaint: {
                    const context = getContext("2d")
                    context.reset()
                    context.strokeStyle = AuroraTheme.blue
                    context.lineWidth = 2
                    context.lineCap = "round"
                    context.beginPath()
                    context.arc(width / 2, height / 2, 7, 0, Math.PI * 1.55)
                    context.stroke()
                }
                RotationAnimator {
                    target: spinner
                    from: 0
                    to: 360
                    duration: 850
                    loops: Animation.Infinite
                    running: spinner.visible && !page.reduceMotion
                }
            }
        }
    }

    Item {
        id: feedbackSlot
        anchors.top: heading.bottom
        anchors.topMargin: 15
        anchors.left: heading.left
        anchors.right: heading.right
        height: page.feedbackVisible ? feedback.implicitHeight + 8 : 0
        clip: true

        Behavior on height {
            enabled: !page.reduceMotion
            NumberAnimation { duration: 240; easing.type: Easing.InOutQuad }
        }

        Text {
            id: feedback
            objectName: "refreshFeedback"
            width: parent.width
            visible: parent.height > 0
            text: page.refreshFeedback
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: AuroraTheme.font
            font.pixelSize: 13
            color: page.refreshFailed ? "#A13F39" : AuroraTheme.muted
        }
    }

    Flickable {
        id: gallery
        objectName: "deviceGallery"
        anchors.top: feedbackSlot.bottom
        anchors.bottom: parent.bottom
        anchors.left: heading.left
        anchors.right: parent.right
        contentWidth: width
        // Leave space after the final card, inside the scrollable content.
        contentHeight: placeholder.visible ? placeholder.y + placeholder.height + page.verticalInset
                                            : cards.height + page.verticalInset
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ScrollBar.vertical: ScrollBar {
            id: scrollbar
            width: 7
            padding: 1
            bottomPadding: 2
            policy: gallery.contentHeight > gallery.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            background: null
            contentItem: Rectangle { color: "#A1AEC2"; radius: 3 }
        }

        Grid {
            id: cards
            objectName: "deviceCards"
            visible: page.ready && deviceRepeater.count > 0
            width: page.contentWidth
            columns: page.columns
            rowSpacing: 14
            columnSpacing: 14

            Repeater {
                id: deviceRepeater
                model: page.ready ? page.deviceModel : null
                delegate: DeviceCard {
                    required property int index
                    objectName: "device-" + deviceId
                    width: Math.floor((page.contentWidth - (page.columns - 1) * cards.columnSpacing) / page.columns)
                    defaultWallpaper: AuroraTheme.desktopAsset("Wallpapers/" +
                        page.defaultWallpapers[index % page.defaultWallpapers.length])
                    enabledTools: page.enabledDeviceTools[deviceId] || []
                    connectionPending: page.connectionPending
                    onConnectRequested: function(deviceId) { page.connectRequested(deviceId) }
                    onToolRequested: function(deviceId, action) { page.toolRequested(deviceId, action) }
                    onCopyRequested: function(address) {
                        clipboard.text = address
                        clipboard.selectAll()
                        clipboard.copy()
                        clipboard.clear()
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus)
                            return
                        if (y < gallery.contentY || height > gallery.height)
                            gallery.contentY = y
                        else if (y + height > gallery.contentY + gallery.height)
                            gallery.contentY = Math.max(0, y + height - gallery.height)
                    }
                }
            }
        }

        Rectangle {
            id: placeholder
            objectName: "devicePlaceholder"
            y: 24
            x: (page.contentWidth - width) / 2
            width: Math.min(438, page.contentWidth)
            height: placeholderContent.implicitHeight + 52
            visible: !cards.visible
            radius: 11
            color: "white"
            border.color: AuroraTheme.panelLine
            readonly property bool signedOut: page.accessState === "signedOut"

            Column {
                id: placeholderContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 18
                anchors.topMargin: 26
                spacing: 12

                AuroraGlyph {
                    width: 28
                    height: width
                    anchors.horizontalCenter: parent.horizontalCenter
                    icon.source: placeholder.signedOut ? AuroraTheme.tailscale : AuroraTheme.icon("monitor")
                }

                Column {
                    width: parent.width
                    spacing: 5
                    Text {
                        width: parent.width
                        text: placeholder.signedOut ? qsTr("登录后连接设备") : page.ready
                            ? qsTr("还没有发现其他设备") : qsTr("正在读取设备状态…")
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        color: AuroraTheme.ink
                        font.family: AuroraTheme.font
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        visible: page.ready || placeholder.signedOut
                        text: placeholder.signedOut ? qsTr("登录 Tailscale 验证身份，即可连接局域网和异地设备。")
                            : qsTr("在另一台设备上打开极光，然后刷新设备列表。")
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        color: AuroraTheme.muted
                        font.family: AuroraTheme.font
                        font.pixelSize: 13
                    }
                }

                AuroraButton {
                    objectName: "deviceAccountAction"
                    visible: page.ready || placeholder.signedOut
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: placeholder.signedOut ? qsTr("前往登录") : qsTr("检查 Tailscale")
                    primary: true
                    onClicked: page.accountRequested()
                }
            }
        }
    }

    TextEdit {
        id: clipboard
        visible: false
    }
}
