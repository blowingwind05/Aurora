// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import ".." as Aurora

Item {
    id: chrome

    enum Phase { Connecting, Connected, Failed }
    enum NetworkQuality { Normal, Poor, Bad }

    property string deviceName
    property int phase: StreamSessionChrome.Connecting
    property int networkQuality: StreamSessionChrome.Normal
    property bool reduceMotion: false
    property bool brandAtEnd: Aurora.AuroraTheme.mac
    property url brandSource: Qt.resolvedUrl("../../../../docs/visual-design/assets/brand/Aurora-transparent-512.png")
    property int leadingWindowInset: 0
    property int trailingWindowInset: 0
    property bool maximized: false
    property bool fullScreen: false
    property bool revealControls: false
    property bool keyboardReveal: false
    property bool canSetFullScreen: false
    property bool canCancel: false
    property bool canDisconnect: false

    property var connectionMetrics: ({})
    property var displays: []
    property string activeDisplayId
    property bool canCreateDisplay: false
    property bool creatingDisplay: false
    property string createDisplayUnavailableReason
    property string displayActionError

    property var preferences: ({})
    property var mouseMode: null
    property var privacyScreen: null
    property string privacyScreenMessage
    property bool mouseCaptured: false
    property string releaseMouseShortcut

    readonly property bool connected: phase === StreamSessionChrome.Connected
    readonly property bool compact: width < 760
    readonly property bool tight: width < 490
    readonly property int barHeight: 44
    readonly property int contentTop: fullScreen ? 0 : barHeight
    readonly property bool controlsVisible: !fullScreen || revealControls || edge.hovered
        || keyboardReveal || toolbar.activeFocus || toolbar.pointerInside || panelOpen
    readonly property bool panelOpen: panelKind !== ""
    property string panelKind: ""
    property Item panelAnchor: null

    readonly property string statusText: phase === StreamSessionChrome.Connecting ? qsTr("连接中")
        : phase === StreamSessionChrome.Failed ? qsTr("连接失败")
        : networkQuality === StreamSessionChrome.Poor ? qsTr("网络较差")
        : networkQuality === StreamSessionChrome.Bad ? qsTr("网络很差") : qsTr("已连接")
    readonly property color statusColor: phase === StreamSessionChrome.Failed ? "#A82539"
        : networkQuality === StreamSessionChrome.Bad ? "#BF3444"
        : networkQuality === StreamSessionChrome.Poor ? "#A56814" : Aurora.AuroraTheme.statusBlue

    signal cancelRequested()
    signal disconnectRequested()
    signal displayRequested(string displayId)
    signal createDisplayRequested()
    signal removeDisplayRequested(string displayId)
    signal fullScreenRequested(bool enabled)
    signal resolutionRequested(string resolution)
    signal frameRateRequested(int fps)
    signal bitrateRequested(int kbps)
    signal codecRequested(string codec)
    signal hdrRequested(bool enabled)
    signal performanceOverlayRequested(bool enabled)
    signal audioConfigurationRequested(string configuration)
    signal remoteAudioMutedRequested(bool muted)
    signal muteOnFocusLossRequested(bool muted)
    signal mouseModeRequested(string mode)
    signal swapMouseButtonsRequested(bool enabled)
    signal reverseScrollRequested(bool enabled)
    signal privacyScreenRequested(bool enabled)

    function focusControls() {
        if (!connected)
            return
        keyboardReveal = true
        toolbar.controlsButton.forceActiveFocus(Qt.ShortcutFocusReason)
        keyboardReveal = false
    }

    function togglePanel(kind, anchor) {
        if (panelKind === kind && panelAnchor === anchor) {
            closePanel(true)
            return
        }
        panelAnchor = anchor
        panelKind = kind
        popup.focusInitial()
    }

    function closePanel(restoreFocus) {
        if (!panelOpen)
            return
        var opener = panelAnchor
        panelKind = ""
        panelAnchor = null
        if (restoreFocus && opener && opener.enabled && opener.visible)
            opener.forceActiveFocus(Qt.BacktabFocusReason)
    }

    onConnectedChanged: if (!connected) closePanel(false)
    onCompactChanged: {
        if (panelKind === "displays") {
            panelAnchor = toolbar.displayEntry()
            if (!panelAnchor || !panelAnchor.enabled)
                closePanel(false)
        }
    }
    function refreshDisplayPanel() {
        if (panelKind !== "displays")
            return
        panelAnchor = toolbar.displayEntry()
        if (displays.length === 0 || !panelAnchor)
            closePanel(false)
        else if (!popup.activeFocus)
            popup.focusInitial()
    }

    onDisplaysChanged: Qt.callLater(refreshDisplayPanel)
    onActiveDisplayIdChanged: {
        if (panelKind === "displays" && !compact)
            closePanel(false)
    }

    Item {
        width: parent.width
        height: 2
        visible: chrome.fullScreen
        HoverHandler { id: edge }
    }

    StreamTitleBar {
        id: toolbar
        objectName: "streamTitleBar"
        width: parent.width
        height: chrome.barHeight
        visible: chrome.controlsVisible
        session: chrome
    }

    MouseArea {
        anchors.fill: parent
        anchors.topMargin: chrome.barHeight
        visible: chrome.panelOpen
        acceptedButtons: Qt.AllButtons
        onClicked: chrome.closePanel(false)
        onWheel: function(wheel) { wheel.accepted = true }
    }

    StreamPanel {
        id: popup
        session: chrome
        visible: chrome.panelOpen
    }

    Rectangle {
        id: displayError
        visible: chrome.displayActionError.length > 0 && chrome.controlsVisible && !chrome.panelOpen
        y: chrome.barHeight + 2
        x: Math.max(8, Math.min(chrome.width - width - 8,
            chrome.compact ? 8 : toolbar.addButton.mapToItem(chrome, 0, 0).x))
        width: Math.min(258, chrome.width - 16)
        height: errorText.implicitHeight + 16
        radius: 9
        color: "white"
        border.color: Aurora.AuroraTheme.panelLine

        StreamText {
            id: errorText
            x: 10
            y: 8
            width: parent.width - 20
            wrapMode: Text.Wrap
            text: chrome.displayActionError
            color: "#A82539"
        }
    }
}
