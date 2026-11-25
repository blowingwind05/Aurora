// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15

// Controls supplies the SVG tint and keeps the original asset alpha.
ToolButton {
    enabled: false
    Accessible.ignored: true
    display: AbstractButton.IconOnly
    padding: 0
    background: null
    icon.width: width
    icon.height: height
    icon.color: AuroraTheme.ink
}
