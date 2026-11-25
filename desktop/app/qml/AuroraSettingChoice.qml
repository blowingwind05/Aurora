// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Templates 2.15 as T

T.ComboBox {
    id: field
    property int selectedIndex: -1
    signal selectionRequested(int index)

    currentIndex: selectedIndex
    displayText: selectedIndex >= 0 && selectedIndex < count ? textAt(selectedIndex) : "—"
    implicitHeight: 34
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    leftPadding: 9
    rightPadding: 28
    font.family: AuroraTheme.font
    font.pixelSize: 13

    onActivated: function(index) {
        // Continue to show the last value confirmed by the host after a user edit.
        currentIndex = Qt.binding(function() { return field.selectedIndex })
        if (index !== selectedIndex)
            selectionRequested(index)
    }

    contentItem: Text {
        text: field.displayText
        textFormat: Text.PlainText
        font: field.font
        color: field.enabled ? AuroraTheme.ink : AuroraTheme.muted
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: AuroraGlyph {
        x: field.width - width - 8
        y: (field.height - height) / 2
        width: 14
        height: 14
        icon.source: AuroraTheme.icon("chevron-down")
        icon.color: AuroraTheme.muted
        rotation: field.popup.visible ? 180 : 0
    }

    background: Rectangle {
        radius: 6
        color: field.enabled ? "white" : "#F3F5F8"
        border.color: field.popup.visible ? AuroraTheme.blue
            : field.enabled && field.hovered ? "#A8B8CE" : "#CDD7E6"

        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: parent.radius + 4
            color: "transparent"
            border.width: 2
            border.color: AuroraTheme.blue
            visible: field.visualFocus
        }
    }

    delegate: T.ItemDelegate {
        id: option
        required property int index
        required property string modelData
        readonly property bool selected: index === field.selectedIndex

        width: ListView.view.width
        height: 32
        text: modelData
        highlighted: field.highlightedIndex === index
        hoverEnabled: true
        leftPadding: 8
        rightPadding: 28

        contentItem: Text {
            text: option.text
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font: field.font
            color: option.selected ? "#2350B4" : AuroraTheme.ink
            verticalAlignment: Text.AlignVCenter
        }

        AuroraGlyph {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            visible: option.selected
            icon.source: Qt.resolvedUrl("../../../docs/visual-design/assets/icons/phosphor/check-bold.svg")
            icon.color: "#2350B4"
        }

        background: Rectangle {
            radius: 5
            color: option.selected ? AuroraTheme.selected
                : option.hovered || option.highlighted ? AuroraTheme.hover : "transparent"
            border.width: option.visualFocus ? 2 : 0
            border.color: AuroraTheme.blue
        }
    }

    popup: T.Popup {
        id: choices
        objectName: "settingOptions"
        property real anchorTop: 0
        modal: false
        padding: 4
        margins: 6
        width: field.width
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                         field.Window.height - 2 * margins)
        y: anchorTop + field.height + height + 4 + margins <= field.Window.height
            ? field.height + 4 : -height - 4

        onAboutToShow: anchorTop = field.mapToItem(null, 0, 0).y

        contentItem: ListView {
            implicitHeight: contentHeight
            clip: true
            model: field.delegateModel
            currentIndex: field.highlightedIndex
            highlightMoveDuration: 0
        }

        background: Rectangle {
            radius: 8
            color: "white"
            border.color: AuroraTheme.panelLine
        }
    }
}
