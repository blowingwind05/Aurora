// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../qml"

AuroraMainWindow {
    id: shell
    visible: true
    deviceModel: devices
    hostState: snapshot.host || ({})
    deviceAccess: snapshot.deviceAccess || "loading"
    accountName: snapshot.accountName || ""
    accountSubtitle: snapshot.accountSubtitle || ""
    refreshAvailable: snapshot.refreshAvailable === true
    refreshing: snapshot.refreshing === true
    refreshFeedback: snapshot.refreshFeedback || ""
    refreshFailed: snapshot.refreshFailed === true
    connectionPending: snapshot.connectionPending === true
    reduceMotion: snapshot.reduceMotion === true

    readonly property var snapshot: auroraBridge.state

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

    onSnapshotChanged: syncDevices()
    onHostActionRequested: function(action) { auroraBridge.send(action, "", 0) }
    onRefreshRequested: auroraBridge.send("refresh", "", 0)
    onConnectRequested: function(deviceId) { auroraBridge.send("connect", deviceId, snapshot.deviceEpoch) }
    onNavigationRequested: function(destination) {
        if (destination === "account" || destination === "settings")
            auroraBridge.send(destination, "", 0)
        else if (destination === "host" || destination === "devices")
            shell.currentPage = destination
    }
}