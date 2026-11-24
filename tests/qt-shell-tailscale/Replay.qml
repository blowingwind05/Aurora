// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import "../../desktop/qt-shell/bridge"

Live {
    id: shell

    function child(item, name) {
        if (item.objectName === name)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const match = child(item.children[i], name)
            if (match)
                return match
        }
        return null
    }

    function check(condition, message) {
        if (!condition)
            throw new Error(message)
    }

    function marker(action) { auroraBridge.sendTailscale(action, "", 0) }

    Timer {
        interval: 40
        repeat: true
        running: true
        property int step: 0

        onTriggered: {
            try {
                const account = shell.snapshot.tailscaleAccount
                if (!account)
                    return
                if (step === 0) {
                    shell.child(shell.contentItem, "deviceAccountAction").clicked()
                    check(shell.currentPage === "account", "signed-out CTA opens local Qt account page")
                    ++step
                    return
                }
                const page = shell.child(shell.contentItem, "tailscaleAccountPage")
                if (!page)
                    return
                const primary = shell.child(page, "tailscaleAuthorization")
                const refresh = shell.child(page, "refreshTailscale")
                const mode = shell.child(page, "tailscaleRuntimeField")
                const profiles = shell.child(page, "tailscaleProfiles")
                const logout = shell.child(page, "tailscaleLogout")
                const cancel = shell.child(shell.contentItem, "cancelTailscaleConfirmation")
                const approve = shell.child(shell.contentItem, "approveTailscaleConfirmation")
                const confirmId = account.confirmation ? account.confirmation.id : 0

                switch (step) {
                case 1:
                    if (account.revision !== 18) return
                    check(mode.count === 1 && mode.displayText === "内置 Tailscale", "missing system leaves embedded")
                    check(shell.child(page, "tailscaleRuntimeNotice").visible, "missing system notice")
                    refresh.clicked()
                    break
                case 2:
                    if (!account.refreshing) return
                    check(!refresh.enabled && !primary.enabled, "refresh locks both actions")
                    shell.marker("test.refreshing")
                    break
                case 3:
                    if (account.refreshing) return
                    check(primary.enabled && primary.text === "登录 Tailscale", "login action")
                    primary.clicked()
                    break
                case 4:
                    if (account.revision !== 19) return
                    check(primary.enabled && primary.text === "恢复连接", "resume action")
                    shell.width = 800
                    primary.clicked()
                    break
                case 5:
                    if (account.revision !== 20) return
                    check(!page.narrow && mode.count === 2 && mode.selectedIndex === 0, "wide mode options (" + shell.width + "; " + mode.count + "; " + mode.selectedIndex + ")")
                    check(logout.visible && !profiles.visible, "embedded account actions")
                    mode.selectionRequested(1)
                    break
                case 6:
                    if (confirmId !== 41 || !cancel || !cancel.visible) return
                    check(mode.selectedIndex === 0 && !mode.enabled, "protected mode selection")
                    check(cancel.focus, "confirmation focuses cancellation")
                    cancel.clicked()
                    break
                case 7:
                    if (confirmId || account.busy) return
                    check(mode.selectedIndex === 0, "cancel preserves active source")
                    mode.selectionRequested(1)
                    break
                case 8:
                    if (confirmId !== 42 || !approve || !approve.visible) return
                    approve.clicked()
                    break
                case 9:
                    if (!account.runtimeProgress) return
                    check(!mode.enabled && !refresh.enabled, "switch locks source and refresh")
                    shell.marker("test.switching")
                    break
                case 10:
                    if (account.revision !== 21) return
                    shell.width = 430
                    break
                case 11:
                    if (!page.compact) return
                    check(profiles.visible && logout.display === 0, "compact profile trigger and icon-only logout")
                    profiles.clicked()
                    break
                case 12:
                    const profile = shell.child(shell.contentItem, "tailscaleProfile-1")
                    if (!profile || !profile.visible) return
                    profile.clicked()
                    break
                case 13:
                    if (confirmId !== 43 || !approve || !approve.visible) return
                    approve.clicked()
                    break
                case 14:
                    if (!account.retryAvailable) return
                    check(shell.child(page, "tailscaleDisplayName").text === "联调账号", "failed switch preserves the account")
                    shell.child(page, "tailscaleRetry").clicked()
                    break
                case 15:
                    if (confirmId !== 44 || !approve || !approve.visible) return
                    approve.clicked()
                    break
                case 16:
                    if (account.revision !== 22) return
                    check(shell.child(page, "tailscaleDisplayName").text === "另一个账号", "requested profile became current")
                    logout.clicked()
                    break
                case 17:
                    if (confirmId !== 45 || !cancel || !cancel.visible) return
                    check(approve.destructive, "logout is destructive")
                    cancel.clicked()
                    break
                case 18:
                    if (confirmId || account.busy) return
                    check(logout.enabled, "cancel leaves the account connected")
                    logout.clicked()
                    break
                case 19:
                    if (confirmId !== 46 || !approve || !approve.visible) return
                    approve.clicked()
                    break
                case 20:
                    if (account.revision !== 23) return
                    check(!logout.visible && primary.enabled && primary.focus, "logout clears identity and focuses login")
                    shell.child(shell.contentItem, "navDevices").clicked()
                    check(shell.currentPage === "devices", "return to devices")
                    shell.marker("test.finished")
                    shell.close()
                    break
                }
                ++step
            } catch (error) {
                console.error(error)
                auroraBridge.sendTailscale("test.failed", String(error), step)
                shell.close()
            }
        }
    }
}