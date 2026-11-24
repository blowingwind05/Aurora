// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../qml"

AuroraMainWindow {
    id: shell
    visible: true
    currentPage: "settings"
    destinationPage: generalPage

    property alias demoPreferences: demoState
    property bool acceptEdits: true
    property string lastRequest: ""

    QtObject {
        id: demoState
        property bool closeToTray: true
        property bool trayAvailable: true
        property bool startupEnabled: true
        property bool startupAvailable: true
        property string sleepMode: "System"
    }

    Component {
        id: generalPage

        GeneralSettingsPage {
            objectName: "generalSettingsPage"
            compact: shell.compact
            narrow: shell.narrow
            preferences: shell.demoPreferences
            startupFeedback: preferences.startupAvailable ? "" : "此系统或应用包无法登记开机自启动。"

            onCloseToTrayRequested: function(enabled) {
                shell.lastRequest = "close:" + enabled
                if (shell.acceptEdits)
                    preferences.closeToTray = enabled
            }
            onStartupEnabledRequested: function(enabled) {
                shell.lastRequest = "startup:" + enabled
                if (shell.acceptEdits)
                    preferences.startupEnabled = enabled
            }
            onSleepModeRequested: function(mode) {
                shell.lastRequest = "sleep:" + mode
                if (shell.acceptEdits)
                    preferences.sleepMode = mode
            }
        }
    }

    onNavigationRequested: function(destination) {
        if (destination === "devices" || destination === "settings")
            currentPage = destination
    }
}
