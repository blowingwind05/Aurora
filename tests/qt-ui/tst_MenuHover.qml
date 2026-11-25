// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/app/qml/common" as Aurora

Item {
    id: window
    width: 360
    height: 200

    Aurora.AuroraMenu {
        id: menu
        x: 50
        y: 40

        Aurora.AuroraMenuItem {
            id: feedback
            text: "反馈"
        }
        Aurora.AuroraMenuItem {
            text: "关于"
        }
    }

    TestCase {
        name: "MenuHover"
        when: windowShown

        function test_currentItemDoesNotRetainHoverFill() {
            menu.open()
            tryCompare(menu, "opened", true)
            mouseMove(window, 330, 160)
            feedback.highlighted = true
            compare(feedback.background.color, Qt.rgba(0, 0, 0, 0))

            mouseMove(feedback, 30, 12)
            tryCompare(feedback, "hovered", true)
            compare(feedback.background.color, Aurora.AuroraTheme.hover)

            mouseMove(window, 330, 160)
            tryCompare(feedback, "hovered", false)
            compare(feedback.highlighted, true)
            compare(feedback.background.color, Qt.rgba(0, 0, 0, 0))
            menu.close()
        }
    }
}
