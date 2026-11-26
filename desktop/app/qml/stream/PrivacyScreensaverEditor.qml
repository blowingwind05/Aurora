// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../common" as Aurora

FocusScope {
    id: editor

    enum SaveState { Idle, Saving, Succeeded, Failed, Unconfirmed }

    // The caller owns placement, capability, dialogs, and device acknowledgements.
    property bool available: true
    property string disabledReason
    property bool screensaverActive: false
    property string importHint
    property bool reduceMotion: false

    readonly property string deviceName: session.deviceName
    readonly property string savedId: session.saved ? session.saved.id : ""
    readonly property var draft: session.draft
    readonly property int saveState: session.saveState
    readonly property bool dirty: !!draft && !sameStyle(draft, session.saved)
    readonly property bool largePreview: session.largePreview
    readonly property bool narrow: width < 550
    readonly property int inset: narrow ? 14 : 20
    readonly property bool editingAllowed: session.active && available && saveState !== PrivacyScreensaverEditor.Saving
    readonly property bool canSave: editingAllowed && dirty && session.importRequest === 0
        && saveState !== PrivacyScreensaverEditor.Unconfirmed
        && (preview.ready || draft.kind === "default" && !draft.previewUrl)

    readonly property var presets: [
        { id: "mountain-valley", name: qsTr("晴空山谷") },
        { id: "forest-path", name: qsTr("林间小径") },
        { id: "blossom-grove", name: qsTr("花树下") },
        { id: "ginkgo-lakeside", name: qsTr("银杏湖畔") },
        { id: "baikal-piano", name: qsTr("冰湖钢琴") },
        { id: "baikal-accordion", name: qsTr("冰湖手风琴") },
        { id: "sunflower-field", name: qsTr("向日葵花田") },
        { id: "sunset-sky", name: qsTr("逆光云海") }
    ]

    signal importRequested(int requestId)
    signal saveRequested(int requestId, var style)
    signal closeRequested()

    implicitWidth: 686
    implicitHeight: 426

    QtObject {
        id: session
        property bool active: false
        property string deviceName
        property var saved: null
        property var defaultStyle: null
        property var draft: null
        property bool largePreview: false
        property real editorScrollY: 0
        property int serial: 0
        property int importRequest: 0
        property var candidate: null
        property string importError
        property int saveRequest: 0
        property var submitted: null
        property int saveState: PrivacyScreensaverEditor.Idle
        property string saveMessage
    }

    function presetUrl(id) {
        return Aurora.AuroraTheme.desktopAsset("Wallpapers/" + id + ".webp")
    }

    function localPreviewUrl(value) {
        const source = value ? String(value) : ""
        if (source.startsWith("data:image/png;base64,") && source.length <= 12 * 1024 * 1024)
            return source
        if (/^qrc:\/[^/]/i.test(source))
            return source
        return /^file:\/\/\/[^/]/i.test(source) && !/%2f|%5c|\\/i.test(source) ? source : ""
    }

    function normalizeStyle(style) {
        if (!style || typeof style.id !== "string" || style.id.length === 0)
            return null
        if (style.kind === "preset") {
            for (let i = 0; i < presets.length; ++i) {
                const preset = presets[i]
                if (preset.id === style.id)
                    return { id: preset.id, kind: "preset", name: preset.name, previewUrl: String(presetUrl(preset.id)) }
            }
            return null
        }
        if (style.kind !== "custom" && style.kind !== "default")
            return null
        return { id: style.id, kind: style.kind, previewUrl: localPreviewUrl(style.previewUrl),
            name: style.name || (style.kind === "default" ? qsTr("内置默认") : qsTr("自定义图片")) }
    }

    function sameStyle(a, b) {
        return !!a && !!b && a.id === b.id && a.kind === b.kind
    }

    function beginEdit(name, savedStyle, defaultStyle, initialDraft) {
        endEdit()
        session.deviceName = name
        session.saved = normalizeStyle(savedStyle)
        session.defaultStyle = normalizeStyle(defaultStyle)
        session.draft = normalizeStyle(initialDraft) || session.saved || session.defaultStyle
        session.active = true
    }

    function endEdit() {
        session.active = false
        session.deviceName = ""
        session.saved = null
        session.defaultStyle = null
        session.draft = null
        session.largePreview = false
        session.editorScrollY = 0
        session.importRequest = 0
        session.candidate = null
        session.importError = ""
        session.saveRequest = 0
        session.submitted = null
        session.saveState = PrivacyScreensaverEditor.Idle
        session.saveMessage = ""
        body.contentY = 0
    }

    function focusEditor() {
        closeButton.forceActiveFocus(Qt.TabFocusReason)
    }

    function selectStyle(style) {
        const selected = normalizeStyle(style)
        if (!editingAllowed || !selected)
            return
        session.importRequest = 0
        session.candidate = null
        session.importError = ""
        session.draft = selected
        if (saveState !== PrivacyScreensaverEditor.Unconfirmed) {
            session.saveState = PrivacyScreensaverEditor.Idle
            session.saveMessage = ""
        }
    }

    function requestImport() {
        if (!editingAllowed || session.importRequest)
            return
        session.importError = ""
        session.importRequest = ++session.serial
        importRequested(session.importRequest)
    }

    function provideImport(requestId, style) {
        if (!editingAllowed || !requestId || requestId !== session.importRequest || session.candidate)
            return false
        const candidate = normalizeStyle(style)
        if (!candidate || candidate.kind !== "custom" || !candidate.previewUrl)
            return finishImport(requestId, qsTr("无法预览这张图片，请重新选择。"))
        session.candidate = candidate
        Qt.callLater(checkImport)
        return true
    }

    // An empty message means the platform picker was canceled.
    function finishImport(requestId, message) {
        if (!requestId || requestId !== session.importRequest)
            return false
        session.importRequest = 0
        session.candidate = null
        session.importError = message || ""
        return true
    }

    function checkImport() {
        const candidate = session.candidate
        if (!candidate || !editingAllowed || String(importProbe.source) !== candidate.previewUrl)
            return
        if (importProbe.status === Image.Ready)
            selectStyle(candidate)
        else if (importProbe.status === Image.Error)
            finishImport(session.importRequest, qsTr("无法预览这张图片，请重新选择。"))
    }

    function requestSave() {
        if (!canSave)
            return
        session.submitted = normalizeStyle(draft)
        session.saveRequest = ++session.serial
        session.saveMessage = ""
        session.saveState = PrivacyScreensaverEditor.Saving
        saveRequested(session.saveRequest, normalizeStyle(session.submitted))
    }

    function completeSave(requestId, outcome, message, confirmedStyle) {
        if (!requestId || requestId !== session.saveRequest)
            return false
        if (outcome === PrivacyScreensaverEditor.Unconfirmed) {
            session.saveState = outcome
            session.saveMessage = message || qsTr("保存结果待确认")
            return true
        }
        if (outcome === PrivacyScreensaverEditor.Succeeded) {
            const confirmed = confirmedStyle ? normalizeStyle(confirmedStyle) : session.submitted
            if (!confirmed)
                return false
            if (confirmed.kind === "custom" && session.submitted.kind === "custom" && !confirmed.previewUrl) {
                confirmed.previewUrl = session.submitted.previewUrl
                if (confirmedStyle && !confirmedStyle.name)
                    confirmed.name = session.submitted.name
            }
            session.saved = confirmed
            if (sameStyle(draft, session.submitted))
                session.draft = confirmed
            session.saveMessage = qsTr("已保存到 %1").arg(deviceName)
        } else if (outcome === PrivacyScreensaverEditor.Failed) {
            session.saveMessage = message || qsTr("%1 未能保存样式，请重试。").arg(deviceName)
        } else {
            return false
        }
        session.saveState = outcome
        session.saveRequest = 0
        session.submitted = null
        return true
    }

    // Reconcile a lost acknowledgement with a fresh read from this device.
    function reconcileSaved(requestId, style) {
        const confirmed = normalizeStyle(style)
        if (!requestId || requestId !== session.saveRequest || !confirmed
            || saveState !== PrivacyScreensaverEditor.Unconfirmed)
            return false
        if (sameStyle(confirmed, session.submitted))
            return completeSave(requestId, PrivacyScreensaverEditor.Succeeded, "", confirmed)
        session.saved = confirmed
        return completeSave(requestId, PrivacyScreensaverEditor.Failed,
                            qsTr("已核对设备上的样式，可重新保存当前草稿。"))
    }

    function restoreEditorScroll() {
        if (session.active && !largePreview)
            body.contentY = Math.min(session.editorScrollY, Math.max(0, body.contentHeight - body.height))
    }

    function togglePreview() {
        if (largePreview) {
            session.largePreview = false
            Qt.callLater(restoreEditorScroll)
        } else {
            session.editorScrollY = Math.max(0, body.contentY)
            session.largePreview = true
            body.contentY = 0
        }
    }

    Keys.onEscapePressed: function(event) {
        event.accepted = true
        if (largePreview)
            togglePreview()
        else if (saveState !== PrivacyScreensaverEditor.Saving)
            closeRequested()
    }

    onEditingAllowedChanged: {
        if (editingAllowed)
            checkImport()
        else if (!available && session.importRequest)
            finishImport(session.importRequest, "")
    }

    Image {
        id: importProbe
        visible: false
        asynchronous: true
        cache: false
        sourceSize.width: 1024
        sourceSize.height: 1024
        source: session.candidate ? session.candidate.previewUrl : ""
        onStatusChanged: Qt.callLater(editor.checkImport)
    }

    Rectangle {
        anchors.fill: parent
        color: "white"
        radius: 13
        border.color: Aurora.AuroraTheme.panelLine
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: editor.inset
            Layout.rightMargin: editor.inset
            Layout.topMargin: 16
            Layout.bottomMargin: 12
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                StreamText {
                    Layout.fillWidth: true
                    text: qsTr("设置隐私屏保")
                    font.pixelSize: 21
                    font.weight: Aurora.AuroraTheme.headingWeight
                    Accessible.role: Accessible.Heading
                }
                StreamText {
                    objectName: "privacyEditorDevice"
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("被控设备 · %1").arg(editor.deviceName)
                    color: Aurora.AuroraTheme.muted
                    elide: Text.ElideRight
                }
            }

            Aurora.AuroraButton {
                id: closeButton
                objectName: "privacyEditorClose"
                Layout.alignment: Qt.AlignTop
                implicitWidth: 30
                implicitHeight: 30
                text: qsTr("关闭")
                display: AbstractButton.IconOnly
                icon.source: Aurora.AuroraTheme.icon("x")
                enabled: session.active && editor.saveState !== PrivacyScreensaverEditor.Saving
                onClicked: editor.closeRequested()
            }
        }

        Flickable {
            id: body
            objectName: "privacyEditorScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: sections.implicitHeight + 16
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Grid {
                id: sections
                x: editor.inset
                y: 4
                width: Math.max(0, body.width - 2 * editor.inset)
                columns: editor.narrow || editor.largePreview ? 1 : 2
                spacing: editor.narrow ? 14 : 16

                Column {
                    id: previewSection
                    objectName: "privacyEditorPreviewSection"
                    width: sections.columns === 1 ? sections.width
                        : Math.floor((sections.width - sections.spacing) * 5 / 9)
                    spacing: 7

                    RowLayout {
                        id: previewHeading
                        width: parent.width
                        spacing: 8

                        StreamText {
                            text: qsTr("本地预览")
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            Accessible.role: Accessible.Heading
                        }
                        StreamText {
                            objectName: "privacyEditorDraftName"
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: editor.draft ? editor.draft.name : ""
                            color: Aurora.AuroraTheme.muted
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }
                    }

                    PrivacyWallpaperFrame {
                        id: preview
                        objectName: "privacyEditorPreview"
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: editor.largePreview ? Math.min(parent.width,
                            Math.max(0, body.height - previewHeading.height - usageNote.height - 36) * 16 / 9) : parent.width
                        height: width * 9 / 16
                        radius: 11
                        source: editor.draft ? editor.localPreviewUrl(editor.draft.previewUrl) : ""
                        placeholder: qsTr("暂无本地预览")
                        showBorder: true
                    }

                    StreamText {
                        id: usageNote
                        width: parent.width
                        text: editor.screensaverActive ? qsTr("被控端当前已遮挡。保存确认后将换用这张图片。")
                            : qsTr("保存样式后，下次开启隐私屏保时使用。")
                        color: Aurora.AuroraTheme.muted
                        wrapMode: Text.Wrap
                    }
                }

                Column {
                    id: picker
                    objectName: "privacyEditorPicker"
                    visible: !editor.largePreview
                    width: sections.columns === 1 ? sections.width
                        : Math.max(0, sections.width - sections.spacing - previewSection.width)
                    spacing: 6

                    RowLayout {
                        width: parent.width
                        spacing: 6

                        StreamText {
                            Layout.fillWidth: true
                            text: qsTr("内置壁纸")
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            Accessible.role: Accessible.Heading
                        }

                        Aurora.AuroraButton {
                            objectName: "privacyEditorImport"
                            implicitHeight: 24
                            font.pixelSize: 12
                            implicitWidth: Math.max(34, implicitContentWidth + 12)
                            foreground: enabled ? Aurora.AuroraTheme.statusBlue : Aurora.AuroraTheme.disabled
                            text: qsTr("导入图片")
                            enabled: editor.editingAllowed && session.importRequest === 0
                            onClicked: editor.requestImport()
                        }
                    }

                    Grid {
                        id: cards
                        width: parent.width
                        columns: 3
                        spacing: 6

                        Repeater {
                            model: editor.presets

                            Aurora.AuroraButton {
                                id: card
                                required property var modelData
                                objectName: "privacyPreset:" + modelData.id
                                width: Math.floor((cards.width - 2 * cards.spacing) / 3)
                                height: thumbnail.height + 25
                                text: modelData.name
                                selected: !!editor.draft && editor.draft.kind === "preset" && editor.draft.id === modelData.id
                                enabled: editor.editingAllowed
                                Accessible.checkable: true
                                Accessible.checked: selected

                                onClicked: editor.selectStyle({ id: modelData.id, kind: "preset" })
                                onActiveFocusChanged: {
                                    if (activeFocus) {
                                        const top = mapToItem(body.contentItem, 0, 0).y
                                        if (top < body.contentY)
                                            body.contentY = top
                                        else if (top + height > body.contentY + body.height)
                                            body.contentY = top + height - body.height
                                    }
                                }

                                background: Rectangle {
                                    radius: 11
                                    color: card.selected ? "#EDF4FD"
                                        : card.enabled && card.hovered ? Aurora.AuroraTheme.hover : "#F5F7FA"
                                    border.width: card.visualFocus ? 2 : card.selected ? 1 : 0
                                    border.color: Aurora.AuroraTheme.blue
                                }

                                contentItem: Item {
                                    PrivacyWallpaperFrame {
                                        id: thumbnail
                                        objectName: "privacyThumbnail:" + card.modelData.id
                                        x: 4
                                        y: 4
                                        width: Math.max(0, parent.width - 8)
                                        height: width * 9 / 16
                                        radius: 5
                                        backdrop: card.background.color
                                        source: editor.presetUrl(card.modelData.id)
                                        decodeWidth: 256
                                    }

                                    Rectangle {
                                        visible: card.selected
                                        anchors.right: thumbnail.right
                                        anchors.top: thumbnail.top
                                        anchors.margins: 3
                                        width: 16
                                        height: 16
                                        radius: 8
                                        color: Aurora.AuroraTheme.blue
                                        Aurora.AuroraGlyph {
                                            anchors.centerIn: parent
                                            width: 11
                                            height: 11
                                            icon.source: Aurora.AuroraTheme.iconAsset("phosphor/check-bold.svg")
                                            icon.color: "white"
                                        }
                                    }

                                    StreamText {
                                        x: 6
                                        y: thumbnail.y + thumbnail.height + 3
                                        width: Math.max(0, parent.width - 12)
                                        text: card.text
                                        font.pixelSize: 11
                                        color: card.selected ? Aurora.AuroraTheme.statusBlue : Aurora.AuroraTheme.ink
                                        elide: Text.ElideRight
                                        Accessible.ignored: true
                                    }
                                }
                            }
                        }
                    }

                    StreamText {
                        objectName: "privacyEditorImportFeedback"
                        textFormat: Text.PlainText
                        width: parent.width
                        visible: text.length > 0
                        text: session.importError || (session.candidate ? qsTr("正在读取图片…") : editor.importHint)
                        color: session.importError ? "#A82539" : Aurora.AuroraTheme.muted
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        StreamText {
            objectName: "privacyEditorStatus"
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.leftMargin: editor.inset
            Layout.rightMargin: editor.inset
            Layout.topMargin: 5
            Layout.bottomMargin: 9
            visible: text.length > 0
            text: {
                const reason = editor.available ? "" : editor.disabledReason || qsTr("被控端暂不支持自定义屏保")
                return session.saveMessage && reason ? session.saveMessage + "\n" + reason : session.saveMessage || reason
            }
            color: editor.saveState === PrivacyScreensaverEditor.Failed ? "#A82539" : Aurora.AuroraTheme.muted
            wrapMode: Text.Wrap
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Aurora.AuroraTheme.divider
        }

        RowLayout {
            objectName: "privacyEditorFooter"
            Layout.fillWidth: true
            Layout.leftMargin: editor.inset
            Layout.rightMargin: editor.inset
            Layout.topMargin: 11
            Layout.bottomMargin: 13
            spacing: 8

            Aurora.AuroraButton {
                objectName: "privacyEditorReset"
                font.pixelSize: 12
                implicitWidth: Math.max(34, implicitContentWidth + 12)
                foreground: enabled ? Aurora.AuroraTheme.statusBlue : Aurora.AuroraTheme.disabled
                visible: !editor.largePreview
                text: qsTr("恢复默认")
                enabled: editor.editingAllowed && !!session.defaultStyle && !editor.sameStyle(editor.draft, session.defaultStyle)
                onClicked: editor.selectStyle(session.defaultStyle)
            }
            Item { Layout.fillWidth: true }

            Aurora.AuroraButton {
                id: previewButton
                objectName: "privacyEditorTogglePreview"
                implicitWidth: Math.max(76, implicitContentWidth + 22)
                text: editor.largePreview ? qsTr("返回设置") : qsTr("大图预览")
                enabled: editor.saveState !== PrivacyScreensaverEditor.Saving && (editor.largePreview || preview.ready)
                onClicked: editor.togglePreview()
                background: Rectangle {
                    radius: 7
                    color: previewButton.enabled && previewButton.hovered ? Aurora.AuroraTheme.hover : "white"
                    border.width: previewButton.visualFocus ? 2 : 1
                    border.color: previewButton.visualFocus ? Aurora.AuroraTheme.blue : Aurora.AuroraTheme.frameLine
                }
            }

            Aurora.AuroraButton {
                id: saveButton
                objectName: "privacyEditorSave"
                visible: !editor.largePreview
                primary: true
                enabled: editor.canSave
                opacity: enabled || editor.saveState === PrivacyScreensaverEditor.Saving ? 1 : 0.55
                implicitWidth: Math.max(76, implicitContentWidth + 22)
                text: editor.saveState === PrivacyScreensaverEditor.Saving ? qsTr("正在保存…")
                    : editor.saveState === PrivacyScreensaverEditor.Failed ? qsTr("重试保存") : qsTr("保存样式")
                onClicked: editor.requestSave()

                contentItem: Item {
                    implicitWidth: saveLabel.implicitWidth + (spinner.visible ? spinner.width + 5 : 0)
                    implicitHeight: 22

                    Row {
                        anchors.centerIn: parent
                        spacing: 5
                        BusyIndicator {
                            id: spinner
                            width: 15
                            height: 15
                            padding: 0
                            anchors.verticalCenter: parent.verticalCenter
                            visible: editor.saveState === PrivacyScreensaverEditor.Saving && !editor.reduceMotion
                            running: visible
                            palette.dark: "white"
                            Accessible.ignored: true
                        }
                        StreamText {
                            id: saveLabel
                            text: saveButton.text
                            color: "white"
                            font: saveButton.font
                            Accessible.ignored: true
                        }
                    }
                }
            }
        }
    }
}
