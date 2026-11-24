// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import "../qml"

AuroraMainWindow {
    id: shell
    visible: snapshot.windowVisible !== false
    deviceModel: devices
    hostState: snapshot.host || ({})
    deviceAccess: snapshot.deviceAccess || "loading"
    accountName: snapshot.accountName || ""
    accountSubtitle: snapshot.accountSubtitle || ""
    accountAvatar: snapshot.accountAvatar || ""
    refreshAvailable: snapshot.refreshAvailable === true
    refreshing: snapshot.refreshing === true
    refreshFeedback: snapshot.refreshFeedback || ""
    refreshFailed: snapshot.refreshFailed === true
    connectionPending: snapshot.connectionPending === true
    reduceMotion: snapshot.reduceMotion === true
    moreVersion: moreState.versionLabel || ""
    moreFeedbackAddress: moreState.feedbackAddress || ""
    checkingUpdates: pendingUpdate || moreState.checking === true

    readonly property var snapshot: auroraBridge.state
    readonly property var moreState: snapshot.more || ({})
    property bool pendingUpdate: false
    property int lastUpdateCompleted: 0
    property int lastMoreNotice: 0

    readonly property var generalSettings: snapshot.generalSettings || null
    property int nextPreferenceRequestId: 0
    property int pendingPreferenceRequestId: 0

    property int lastWindowActivation: 0
    readonly property bool avatarVisible: snapshot.managedWindow === true && visible && visibility !== Window.Minimized
    onAvatarVisibleChanged: {
        if (snapshot.managedWindow)
            auroraBridge.send(avatarVisible ? "avatar-visible" : "avatar-hidden", "", 0)
    }

    readonly property bool devicesForeground: snapshot.managedWindow === true && visible && active && visibility !== Window.Minimized && currentPage === "devices"

    destinationPage: currentPage === "settings" ? generalPage : currentPage === "account" ? tailscalePage : null

    readonly property var tailscaleAccount: snapshot.tailscaleAccount || ({})
    property Item tailscaleActionFocus: null
    readonly property int tailscaleNoticeVersion: tailscaleAccount.noticeVersion || 0

    onTailscaleNoticeVersionChanged: {
        if (tailscaleNoticeVersion && tailscaleAccount.notice)
            tailscaleToastTimer.restart()
    }

    TailscaleConfirmation {
        confirmation: shell.tailscaleAccount.confirmation || null
        returnFocusItem: shell.tailscaleActionFocus
        hostBusy: shell.tailscaleAccount.busy === true
        onDecision: function(id, approved) {
            auroraBridge.sendTailscale(approved ? "tailscale.confirm" : "tailscale.cancel", String(id), 0)
        }
    }

    Rectangle {
        anchors.centerIn: parent
        z: 10
        visible: tailscaleToastTimer.running
        width: Math.min(shell.width - 32, notice.implicitWidth + 30)
        height: notice.height + 20
        radius: 9
        color: "white"
        border.color: AuroraTheme.panelLine
        Text {
            id: notice
            x: 15
            y: 10
            width: parent.width - 30
            text: shell.tailscaleAccount.notice || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AuroraTheme.ink
            font.family: AuroraTheme.font
            font.pixelSize: 14
        }
        Timer { id: tailscaleToastTimer; interval: 3000 }
    }

    Component {
        id: tailscalePage
        TailscaleAccountPage {
            account: shell.tailscaleAccount
            avatarPhoto: shell.accountAvatar
            compact: shell.compact
            narrow: shell.narrow
            reduceMotion: shell.reduceMotion
            onActionRequested: function(action, target, source) {
                shell.tailscaleActionFocus = source
                auroraBridge.sendTailscale(action, target, shell.tailscaleAccount.revision || 0)
            }
        }
    }

    onDevicesForegroundChanged: {
        if (snapshot.managedWindow)
            auroraBridge.send(devicesForeground ? "devices-foreground" : "devices-background", "", 0)
    }

    onClosing: function(event) {
        if (!snapshot.managedWindow)
            return
        event.accepted = false
        auroraBridge.send("window-close", "", 0)
    }

    Component {
        id: generalPage

        GeneralSettingsPage {
            objectName: "liveGeneralSettings"
            compact: shell.compact
            narrow: shell.narrow
            preferences: shell.generalSettings
            updating: shell.pendingPreferenceRequestId !== 0
            closeFeedback: preferences ? preferences.closeFeedback : ""
            startupFeedback: preferences ? preferences.startupFeedback : ""
            sleepFeedback: preferences ? preferences.sleepFeedback : ""

            onCloseToTrayRequested: function(enabled) { shell.requestPreference("set-close-to-tray", enabled) }
            onStartupEnabledRequested: function(enabled) { shell.requestPreference("set-startup-enabled", enabled) }
            onSleepModeRequested: function(mode) { shell.requestPreference("set-sleep-mode", mode) }
        }
    }

    function requestPreference(action, value) {
        if (pendingPreferenceRequestId !== 0)
            return
        pendingPreferenceRequestId = ++nextPreferenceRequestId
        auroraBridge.sendPreference(action, pendingPreferenceRequestId, value)
    }
    ListModel { id: devices }

    // Keep matching delegates alive through automatic discovery and status changes.
    function syncDevices() {
        const rows = snapshot.devices || []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            let existing = i
            while (existing < devices.count && devices.get(existing).deviceId !== row.deviceId)
                ++existing
            if (existing === devices.count) {
                devices.insert(i, row)
            } else {
                if (existing !== i)
                    devices.move(existing, i, 1)
                devices.set(i, row)
            }
        }
        if (devices.count > rows.length)
            devices.remove(rows.length, devices.count - rows.length)
    }

    onSnapshotChanged: {
        syncDevices()
        const activation = snapshot.windowActivation || 0
        if (activation === lastWindowActivation)
            return
        lastWindowActivation = activation
        if (snapshot.windowPage)
            navigationRequested(snapshot.windowPage)
        if (visibility === Window.Minimized)
            showNormal()
        requestActivate()
    }

    onGeneralSettingsChanged: {
        if (generalSettings && generalSettings.acknowledgedRequestId === pendingPreferenceRequestId)
            pendingPreferenceRequestId = 0
    }

    onActiveChanged: {
        if (active && currentPage === "settings")
            requestPreference("read-general-settings", null)
    }

    onMoreStateChanged: {
        const completed = moreState.updateCompleted || 0
        if (completed !== lastUpdateCompleted) {
            lastUpdateCompleted = completed
            pendingUpdate = false
        }
        const noticeId = moreState.noticeId || 0
        if (noticeId !== lastMoreNotice) {
            lastMoreNotice = noticeId
            if (moreState.notice)
                showMoreNotice(moreState.notice)
        }
    }

    onMoreRequested: function(action) {
        if (action === "checkUpdates") {
            if (checkingUpdates || !snapshot.more)
                return
            pendingUpdate = true
        }
        auroraBridge.send("more." + action, "", 0)
    }

    onHostActionRequested: function(action) { auroraBridge.send(action, "", 0) }
    onRefreshRequested: auroraBridge.send("refresh", "", 0)
    onConnectRequested: function(deviceId) { auroraBridge.send("connect", deviceId, snapshot.deviceEpoch) }
    onNavigationRequested: function(destination) {
        if (destination === "host" || destination === "devices" ||
            destination === "feedback" || destination === "about") {
            currentPage = destination
        } else if (destination === "settings" && ("generalSettings" in snapshot)) {
            currentPage = destination
            requestPreference("read-general-settings", null)
        } else if (destination === "account") {
            currentPage = destination
            if (snapshot.tailscaleAccount)
                auroraBridge.sendTailscale("tailscale.enter", "", 0)
            else
                auroraBridge.send("account", "", 0)
        } else if (destination === "settings") {
            auroraBridge.send(destination, "", 0)
        }
    }
}
