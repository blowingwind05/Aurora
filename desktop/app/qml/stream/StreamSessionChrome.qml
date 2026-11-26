// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import "../common" as Aurora

Item {
    id: chrome

    enum Phase { Connecting, Connected, Failed }
    enum NetworkQuality { Normal, Poor, Bad }

    property string deviceName
    property int phase: StreamSessionChrome.Connecting
    property int networkQuality: StreamSessionChrome.Normal
    property bool reduceMotion: false
    property bool brandAtEnd: Aurora.AuroraTheme.mac
    property url brandSource: Aurora.AuroraTheme.desktopAsset("Aurora.png")
    property int leadingWindowInset: 0
    property int trailingWindowInset: 0
    property bool maximized: false
    property bool fullScreen: false
    property bool windowsOverlays: false
    property bool nativePanels: false
    property bool displayControls: true
    property int layoutWidth: width
    property Window windowControls: null
    property int panelFocusReason: Qt.MouseFocusReason
    readonly property var overlays: nativeOverlays.item
    readonly property bool floatingPanel: panelOpen && !!overlays && panelAnchor === overlays.floatingButton
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
    property bool videoSettingsApplyDuringSession: false
    property int liveVideoBitrateLimit: 0
    property string videoSettingsMessage
    property var mouseMode: null
    property var privacyScreen: null
    property string privacyScreenMessage
    property bool privacyAppearanceAvailable: false
    property bool mouseCaptured: false
    property string releaseMouseShortcut

    readonly property bool connected: phase === StreamSessionChrome.Connected
    readonly property bool compact: layoutWidth < 760
    readonly property bool tight: layoutWidth < 490
    readonly property int barHeight: 44
    readonly property int contentTop: fullScreen ? 0 : barHeight
    readonly property bool controlsVisible: !fullScreen || revealControls || edge.hovered
        || keyboardReveal || toolbar.activeFocus || toolbar.pointerInside
        || (!!overlays && overlays.edgeHovered) || (panelOpen && !floatingPanel)
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

    signal nativePanelRequested(string kind, bool keyboard)
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
    signal privacyModeRequested(string mode)
    signal privacyAppearanceRequested()

    function focusControls(reason) {
        if (!connected)
            return
        keyboardReveal = true
        toolbar.controlsButton.forceActiveFocus(reason === undefined ? Qt.ShortcutFocusReason : reason)
        keyboardReveal = false
    }

    function togglePanel(kind, anchor) {
        if (nativePanels) {
            nativePanelRequested(kind, !!anchor && anchor.visualFocus)
            return
        }
        if (panelKind === kind && panelAnchor === anchor) {
            closePanel(true)
            return
        }
        panelFocusReason = anchor && anchor.visualFocus ? Qt.TabFocusReason : Qt.MouseFocusReason
        panelAnchor = anchor
        panelKind = kind
        if (!overlays)
            popup.focusInitial()
    }

    function closePanel(restoreFocus) {
        if (!panelOpen)
            return
        var opener = panelAnchor
        panelKind = ""
        panelAnchor = null
        if (restoreFocus && opener && opener.enabled && opener.visible) {
            if (overlays && opener !== overlays.floatingButton && opener.Window.window)
                opener.Window.window.requestActivate()
            opener.forceActiveFocus(Qt.BacktabFocusReason)
        }
    }

    onConnectedChanged: if (!connected) closePanel(false)
    onFullScreenChanged: closePanel(false)
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
        visible: chrome.fullScreen && !chrome.windowsOverlays
        HoverHandler { id: edge }
    }

    StreamTitleBar {
        id: toolbar
        objectName: "streamTitleBar"
        parent: chrome.fullScreen && chrome.overlays ? chrome.overlays.toolbarContent : chrome
        width: chrome.width
        height: chrome.barHeight
        visible: chrome.controlsVisible
        session: chrome
    }

    MouseArea {
        anchors.fill: parent
        anchors.topMargin: chrome.barHeight
        visible: chrome.panelOpen && !chrome.overlays && !chrome.nativePanels
        acceptedButtons: Qt.AllButtons
        onClicked: chrome.closePanel(false)
        onWheel: function(wheel) { wheel.accepted = true }
    }

    Loader {
        id: nativeOverlays
        active: chrome.windowsOverlays
        sourceComponent: StreamWindowsOverlays {
            owner: chrome.Window.window
            session: chrome
            panel: popup
        }
    }

    StreamPanel {
        id: popup
        parent: chrome.overlays ? chrome.overlays.panelContent : chrome
        overlayWindow: !!chrome.overlays
        session: chrome
        visible: chrome.panelOpen && !chrome.nativePanels
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
