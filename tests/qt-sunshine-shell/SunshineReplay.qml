// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/qt-shell/bridge"

Live {
    id: shell

    TestEvent { id: input }
    TestResult { id: result }

    Timer {
        interval: 35
        running: shell.active
        onTriggered: run()

        function check(condition, message) {
            if (!condition)
                throw new Error(message)
        }

        function child(parent, name) {
            if (parent.objectName === name && parent.visible)
                return parent
            const children = parent.children
            for (let i = 0; i < children.length; ++i) {
                const found = child(children[i], name)
                if (found)
                    return found
            }
            return null
        }

        function eventually(condition, message) {
            const deadline = Date.now() + 5000
            while (!condition() && Date.now() < deadline)
                result.wait(20)
            check(condition(), message)
        }

        function item(name) {
            let target
            eventually(function() { target = child(shell.contentItem, name); return !!target }, name)
            return target
        }

        function click(control) {
            check(input.mouseClick(control, control.width / 2, control.height / 2,
                                   Qt.LeftButton, Qt.NoModifier, 10), control.objectName)
        }

        function key(code, modifiers) {
            check(input.keyClick(code, modifiers || Qt.NoModifier, 10), "keyboard")
        }

        function type(text) {
            for (let i = 0; i < text.length; ++i)
                check(input.keyClickChar(text[i], Qt.NoModifier, 10), "text input")
        }

        function replace(control, value) {
            click(control)
            key(Qt.Key_A, Qt.ControlModifier)
            type(value)
            check(control.text === value, control.objectName + " edited")
        }

        function step(action) {
            auroraBridge.send(action, "", 0)
        }

        function run() {
            let stage = "read"
            try {
                eventually(function() { return shell.hostState.sunshine && shell.hostState.sunshine.status === "idle" }, "initial snapshot")
                click(item("navHost"))
                const name = item("sunshineField:sunshine_name")
                check(name.text === "验收电脑", "initial service readback")

                stage = "debounced name and in-flight edit"
                replace(name, "Studio")
                step("test-name-edited")
                eventually(function() { return shell.hostState.sunshine.busy }, "busy snapshot")
                type("2")
                check(name.text === "Studio2", "allow editing during autosave")
                step("test-name-edited-again")
                eventually(function() {
                    return shell.hostState.sunshine.config.values.sunshine_name === "Studio2"
                }, "latest name readback")
                check(name.text === "Studio2", "earlier acknowledgement kept the newer draft")

                stage = "audio"
                click(item("sunshineField:stream_audio"))
                eventually(function() {
                    return shell.hostState.sunshine.config.values.stream_audio === "disabled"
                }, "audio readback")

                stage = "320px, tabs, search and invalid input"
                shell.width = 320
                eventually(function() { return shell.compact }, "compact layout")
                click(item("sunshineOpenAdvanced"))
                const page = item("hostPage")
                check(page.advanced && shell.currentPage === "host", "advanced stays in host navigation")
                const search = item("sunshineSearch")
                click(item("sunshineTab:2"))
                replace(search, "min_threads")
                const threads = item("sunshineField:min_threads")
                check(threads.width > 0 && threads.x + threads.width <= 320 && threads.text === "2", "compact control")
                replace(threads, "bad")
                eventually(function() {
                    return !!shell.hostState.sunshine.result.errors.min_threads
                }, "validation error")
                check(threads.text === "bad", "failed text retained")
                click(item("sunshineBack"))
                eventually(function() { return item("sunshineDiscardDialog:cancel").activeFocus }, "cancel focused")
                key(Qt.Key_Escape)
                check(page.advanced && threads.text === "bad", "cancel retains draft and page")
                shell.close()
                eventually(function() { return item("sunshineDiscardDialog:cancel").activeFocus }, "window closes through the same save gate")
                key(Qt.Key_Escape)
                check(shell.visible && page.advanced && threads.text === "bad", "close cancel keeps draft")
                step("test-kept-draft")
                replace(threads, "4")
                eventually(function() {
                    return shell.hostState.sunshine.config.values.min_threads === "4"
                }, "advanced readback")
                click(item("sunshineTab:0"))
                check(search.text === "", "category clears search")
                click(item("sunshineBack"))
                check(!page.advanced && shell.currentPage === "host", "back to the same host view")

                stage = "restart confirmation"
                shell.width = 1000
                click(item("sunshineRestart"))
                eventually(function() { return item("sunshineRestartDialog:cancel").activeFocus }, "restart cancel focused")
                click(item("sunshineRestartDialog:cancel"))
                step("test-restart-cancelled")
                click(item("sunshineRestart"))
                click(item("sunshineRestartDialog:confirm"))
                step("test-finished")
            } catch (error) {
                console.error(stage, error)
                auroraBridge.send("test-failed", stage + " — " + String(error), 0)
            }
            shell.close()
        }
    }
}
