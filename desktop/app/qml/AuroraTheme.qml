// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick 2.15

QtObject {
    readonly property bool windows: Qt.platform.os === "windows"
    readonly property bool mac: Qt.platform.os === "osx"
    readonly property string font: windows ? "Microsoft YaHei UI" : mac ? "PingFang SC" : Qt.application.font.family
    readonly property color frame: "#EDF1F8"
    readonly property color workspace: "#F9FAFC"
    readonly property color ink: "#19283F"
    readonly property color muted: "#526176"
    readonly property color frameLine: "#C6D1E0"
    readonly property color panelLine: "#DBE2ED"
    readonly property color divider: "#E7EBF3"
    readonly property color blue: "#087CF5"
    readonly property color statusBlue: "#244FC5"
    readonly property color selected: "#E2E7EF"
    readonly property color hover: "#E8EDF4"
    readonly property color hoverInk: "#252A31"
    readonly property color disabled: "#7A8494"

    function icon(name) {
        return Qt.resolvedUrl("../../../docs/visual-design/assets/icons/lucide/" + name + ".svg")
    }

    function desktopAsset(path) {
        return Qt.resolvedUrl("../../Assets/" + path)
    }

    readonly property url tailscale: desktopAsset("tailscale-dot.svg")
    readonly property url github: Qt.resolvedUrl("../../../docs/visual-design/assets/icons/primer/mark-github-16.svg")
}
