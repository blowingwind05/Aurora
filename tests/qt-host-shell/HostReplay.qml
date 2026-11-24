// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/qt-shell/bridge"

Live {
    id: shell

    TestEvent { id: input }
    TestResult { id: result }
    Image {
        id: hostIconProbe
        visible: false
    }

    Timer {
        interval: 35
        running: shell.active
        onTriggered: run()

        function verify(condition, message) {
            if (!condition)
                throw new Error(message || "控件状态与宿主快照不匹配")
        }

        function compare(actual, expected) {
            verify(actual === expected, "预期 " + expected + "，实际 " + actual)
        }

        function tryVerify(condition, message) {
            const deadline = Date.now() + 3500
            while (!condition() && Date.now() < deadline)
                result.wait(20)
            verify(condition(), message)
        }

        function tryCompare(object, property, expected) {
            tryVerify(function() { return object[property] === expected },
                      property + " 未更新为 " + expected)
        }

        function mouseClick(control) {
            verify(input.mouseClick(control, control.width / 2, control.height / 2,
                                    Qt.LeftButton, Qt.NoModifier, 10), control.objectName)
        }

        function keyClick(key) {
            verify(input.keyClick(key, Qt.NoModifier, 10), "未发送键盘事件")
        }

        function visualChild(parent, name) {
            if (parent.objectName === name)
                return parent
            const children = parent.children
            for (let i = 0; i < children.length; ++i) {
                const found = visualChild(children[i], name)
                if (found)
                    return found
            }
            return null
        }

        function item(name) {
            const child = visualChild(shell.contentItem, name)
            verify(child !== null, name)
            return child
        }

        function step(action) {
            auroraBridge.send(action, "", 0)
        }

        function run() {
            let stage = "初始状态"
            try {
                tryCompare(item("hostStatus"), "text", "未启用")
                mouseClick(item("navHost"))
                compare(shell.currentPage, "host")
                stage = "被控面板标题与图标"
                const hostIcon = item("hostComputerIcon")
                compare(hostIcon.width, 28)
                compare(hostIcon.height, 28)
                hostIconProbe.source = hostIcon.icon.source
                tryCompare(hostIconProbe, "status", Image.Ready)
                compare(item("hostPageTitle").font.weight, Font.Bold)
                compare(item("hostComputerTitle").font.weight, Font.Bold)
                const page = item("hostPage")
                const toggle = item("allowHost")
                const lan = item("hostLan")
                const tailnet = item("hostTailnet")
                const permissions = item("hostPermissions")
                const refresh = item("refreshHost")
                const dialog = result.findChild(page, "hostStopDialog")
                verify(dialog !== null, "hostStopDialog")
                verify(toggle.enabled && !page.allowed)
                verify(!lan.visible && !tailnet.visible && !permissions.visible)

                stage = "启动与局域网地址"
                mouseClick(toggle)
                tryCompare(item("hostStatus"), "text", "正在启动")
                verify(!toggle.enabled && !lan.visible)
                step("test-starting")
                tryCompare(item("hostStatus"), "text", "运行中")
                verify(page.allowed && toggle.enabled && lan.visible && !tailnet.visible)
                compare(lan.value, "192.0.2.18:47989")
                mouseClick(item("hostCopyLan"))

                stage = "宽窄布局与 Tailscale 地址"
                tryCompare(tailnet, "visible", true)
                tryVerify(function() { return tailnet.x > lan.x && tailnet.y === lan.y }, "宽窗地址未并排")
                compare(tailnet.value, "100.100.0.18:47989")
                shell.width = 320
                tryVerify(function() { return tailnet.y > lan.y && tailnet.x === lan.x }, "窄窗地址未堆叠")
                step("test-narrow")
                tryCompare(tailnet, "value", "[fd7a:115c:a1e0:ab12:4843:cd96:627f:caf4]:47989")
                verify(tailnet.width > 0 && tailnet.height >= 52, "窄窗地址高度不足")
                mouseClick(item("hostCopyTailnet"))

                stage = "权限与宿主设置入口"
                shell.width = 1000
                tryCompare(permissions, "visible", true)
                mouseClick(item("hostOpenPermissions"))
                tryCompare(item("hostFeedback"), "text", "已打开 macOS 隐私设置。")
                mouseClick(item("hostContinuePermissions"))
                tryCompare(page, "busy", true)
                verify(!item("hostContinuePermissions").enabled)
                step("test-permission-pending")
                tryCompare(page, "busy", false)
                verify(permissions.visible)
                step("test-permissions-returned")
                tryCompare(item("hostFeedback"), "text", "已完成权限检测。")
                verify(!permissions.visible && !tailnet.visible)
                mouseClick(refresh)
                tryCompare(refresh, "enabled", false)
                step("test-refreshing")
                tryCompare(refresh, "enabled", true)
                compare(item("hostFeedback").text, "检测完成，Sunshine 正在运行。")

                stage = "关闭确认与 Esc"
                shell.width = 320
                toggle.forceActiveFocus(Qt.TabFocusReason)
                keyClick(Qt.Key_Space)
                tryCompare(dialog, "opened", true)
                const cancel = item("hostCancelStop")
                tryCompare(cancel, "activeFocus", true)
                verify(page.allowed && lan.visible)
                keyClick(Qt.Key_Escape)
                tryCompare(dialog, "visible", false)
                tryCompare(toggle, "activeFocus", true)
                verify(page.allowed && lan.visible)

                stage = "取消和确认关闭"
                mouseClick(toggle)
                tryCompare(dialog, "opened", true)
                mouseClick(cancel)
                tryCompare(dialog, "visible", false)
                verify(page.allowed && lan.visible)
                step("test-cancelled")
                mouseClick(toggle)
                tryCompare(dialog, "opened", true)
                mouseClick(item("hostApplyStop"))
                tryCompare(item("hostStatus"), "text", "正在关闭")
                verify(!dialog.visible && !toggle.enabled && page.allowed && lan.visible)
                step("test-stopping")
                tryCompare(item("hostStatus"), "text", "未启用")
                verify(!page.allowed && !lan.visible && !item("sunshineCommon").visible)

                stage = "失败后重试"
                mouseClick(toggle)
                tryCompare(item("hostStatus"), "text", "未就绪")
                verify(page.allowed && !lan.visible)
                mouseClick(item("hostRetry"))
                tryCompare(item("hostStatus"), "text", "运行中")
                verify(page.allowed && lan.visible)
                mouseClick(item("navDevices"))
                compare(shell.currentPage, "devices")
                step("test-finished")
            } catch (error) {
                console.error(stage, error)
                auroraBridge.send("test-failed", stage + " — " + String(error), 0)
            }
            shell.close()
        }
    }
}
