// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/app/qml/pages/host" as Host

Item {
    id: panel
    width: 360
    height: 100

    Text { id: heading; x: 14; text: "允许极光控制这台 Mac" }

    Host.SunshineLink {
        id: link
        x: heading.x
        y: 32
        text: "打开 macOS 隐私设置"
    }

    SignalSpy { id: clicks; target: link; signalName: "clicked" }

    TestCase {
        name: "SunshineLink"
        when: windowShown

        function test_visibleLabelStartsAtThePanelInset() {
            const label = link.contentItem.children[0]
            compare(label.text, link.text)
            compare(label.mapToItem(panel, 0, 0).x, heading.x)
            mouseClick(link)
            compare(clicks.count, 1)
        }
    }
}
