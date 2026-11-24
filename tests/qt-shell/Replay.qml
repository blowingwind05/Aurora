// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../../desktop/qt-shell/bridge"

Live {
    id: shell

    function child(item, name) {
        if (item.objectName === name)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = child(item.children[i], name)
            if (found)
                return found
        }
        return null
    }

    function check(condition, message) {
        if (!condition)
            throw new Error(message)
    }

    Timer {
        interval: 35
        running: true
        repeat: true
        property int step: 0
        property var connectButton: null

        onTriggered: {
            const refresh = shell.child(shell.contentItem, "refreshDevices")
            const account = shell.child(shell.contentItem, "navAccount")
            try {
                switch (step) {
                case 0:
                    if (shell.accountSubtitle !== "待确认")
                        return
                    check(shell.deviceAccess === "loading" && shell.deviceModel.count === 0, "loading")
                    auroraBridge.send("test-loading", "", 0)
                    break
                case 1:
                    if (shell.deviceAccess !== "ready")
                        return
                    check(shell.width === 1000 && shell.deviceModel.count === 2, "wide device list")
                    check(account.text === "联调账户" && account.subtitle === "GitHub", "signed-in account")
                    const row = shell.deviceModel.get(0)
                    check(row.lanAddress === "192.0.2.36" && row.tailscaleAddress === "100.100.0.36", "device addresses")
                    connectButton = shell.child(shell.contentItem, "connect-node:书房")
                    const offline = shell.child(shell.contentItem, "connect-node:offline")
                    check(connectButton.enabled && !offline.enabled && refresh.enabled, "available actions")
                    refresh.clicked()
                    break
                case 2:
                    if (!shell.refreshing)
                        return
                    check(!refresh.enabled, "busy refresh")
                    auroraBridge.send("test-refreshing", "", 0)
                    break
                case 3:
                    if (shell.refreshFeedback !== "已刷新，发现 2 台设备。")
                        return
                    check(refresh.enabled && !shell.refreshFailed, "refresh success")
                    auroraBridge.send("test-refreshed", "", 0)
                    break
                case 4:
                    if (!shell.refreshFailed)
                        return
                    check(shell.refreshFeedback === "局域网发现失败。共发现 2 台设备。", "refresh failure")
                    shell.width = 430
                    break
                case 5:
                    if (!shell.compact)
                        return
                    check(connectButton.enabled, "narrow connection action")
                    connectButton.clicked()
                    break
                case 6:
                    if (!shell.connectionPending)
                        return
                    check(!connectButton.enabled, "busy connection")
                    check(shell.deviceModel.get(1).displayName === "书房主机（更新）", "updated row")
                    check(shell.child(shell.contentItem, "connect-node:书房") === connectButton, "stable delegate")
                    auroraBridge.send("test-connecting", "", 0)
                    break
                case 7:
                    if (shell.deviceAccess !== "signedOut")
                        return
                    check(shell.deviceModel.count === 0, "cleared device list")
                    check(account.text === "Tailscale 账户" && account.subtitle === "未登录", "signed-out account")
                    const action = shell.child(shell.contentItem, "deviceAccountAction")
                    check(!refresh.visible && action.visible, "signed-out actions")
                    action.clicked()
                    auroraBridge.send("test-finished", "", 0)
                    shell.close()
                    break
                }
                ++step
            } catch (error) {
                console.error(error)
                auroraBridge.send("test-failed", String(error), step)
                shell.close()
            }
        }
    }
}