// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15
import QtTest 1.2
import "../../desktop/app/qml/stream" as Stream
import "../../desktop/app/qml/common" as Aurora

Item {
    id: fixture
    width: 750
    height: 560

    Stream.PrivacyScreensaverEditor {
        id: editor
        width: 686
        height: 460
        reduceMotion: true
    }

    SignalSpy { id: imports; target: editor; signalName: "importRequested" }
    SignalSpy { id: saves; target: editor; signalName: "saveRequested" }
    SignalSpy { id: closes; target: editor; signalName: "closeRequested" }

    TestCase {
        name: "PrivacyScreensaverEditor"
        when: windowShown

        function preset(id) { return { kind: "preset", id: id } }

        function custom(id, url) {
            return { kind: "custom", id: id, name: "本机图片.webp", previewUrl: url }
        }

        function control(name) {
            var item = findChild(editor, name)
            verify(item !== null, name)
            return item
        }

        function click(name) {
            var button = control(name)
            verify(button.enabled && button.visible, name + " is available")
            mouseClick(button)
        }

        function init() {
            imports.clear()
            saves.clear()
            closes.clear()
            editor.available = true
            editor.disabledReason = ""
            editor.width = 686
            editor.height = 460
            editor.beginEdit("书房主机", preset("forest-path"), preset("mountain-valley"))
            tryCompare(control("privacyEditorPreview"), "ready", true)
        }

        function test_presetsAndDraft() {
            var expected = ["mountain-valley", "forest-path", "blossom-grove", "ginkgo-lakeside",
                            "baikal-piano", "baikal-accordion", "sunflower-field", "sunset-sky"]
            compare(editor.presets.length, expected.length)
            compare(control("privacyEditorDevice").text, "被控设备 · 书房主机")
            compare(editor.savedId, "forest-path")
            compare(editor.draft.id, editor.savedId)
            verify(!control("privacyEditorSave").enabled)
            for (var i = 0; i < expected.length; ++i) {
                var id = expected[i]
                compare(editor.presets[i].id, id)
                verify(control("privacyPreset:" + id).visible)
                tryCompare(control("privacyThumbnail:" + id), "ready", true)
                verify(String(editor.presetUrl(id)).endsWith("/Wallpapers/" + id + ".webp"))
            }

            click("privacyPreset:blossom-grove")
            compare(editor.draft.id, "blossom-grove")
            verify(control("privacyPreset:blossom-grove").selected)
            verify(!control("privacyPreset:forest-path").selected)
            click("privacyEditorReset")
            compare(editor.draft.id, "mountain-valley")
            compare(editor.savedId, "forest-path")
            compare(saves.count, 0)
            click("privacyPreset:forest-path")
            verify(!editor.dirty)
            verify(!control("privacyEditorSave").enabled)
        }

        function test_importChecksAndRaces() {
            compare(editor.localPreviewUrl("file://not-local.invalid/example.webp"), "")
            compare(editor.localPreviewUrl("file:////not-local.invalid/example.webp"), "")
            click("privacyEditorImport")
            compare(imports.count, 1)
            var id = imports.signalArguments[0][0]
            verify(editor.provideImport(id, custom("untrusted", "https://example.invalid/picture.webp")))
            compare(editor.draft.id, "forest-path")
            verify(control("privacyEditorImportFeedback").text.length > 0)

            click("privacyEditorImport")
            id = imports.signalArguments[1][0]
            verify(editor.provideImport(id, custom("bad", Aurora.AuroraTheme.desktopAsset("Wallpapers/missing.webp"))))
            tryVerify(function() { return control("privacyEditorImportFeedback").text.indexOf("无法预览") >= 0 })
            compare(editor.draft.id, "forest-path")

            click("privacyEditorImport")
            id = imports.signalArguments[2][0]
            verify(editor.provideImport(id, custom("custom-a", editor.presetUrl("ginkgo-lakeside"))))
            tryCompare(control("privacyEditorDraftName"), "text", "本机图片.webp")
            tryCompare(control("privacyEditorPreview"), "ready", true)
            compare(editor.draft.id, "custom-a")
            compare(editor.savedId, "forest-path")
            compare(saves.count, 0)

            click("privacyEditorImport")
            id = imports.signalArguments[3][0]
            editor.beginEdit("卧室主机", preset("sunset-sky"), preset("mountain-valley"))
            verify(!editor.provideImport(id, custom("stale", editor.presetUrl("ginkgo-lakeside"))))
            compare(editor.draft.id, "sunset-sky")
        }

        function test_localPreviewAndClose() {
            click("privacyEditorTogglePreview")
            verify(editor.largePreview)
            verify(!control("privacyEditorPicker").visible)
            verify(!control("privacyEditorSave").visible)
            compare(saves.count, 0)
            control("privacyEditorTogglePreview").forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Escape)
            verify(!editor.largePreview)
            compare(closes.count, 0)
            editor.focusEditor()
            keyClick(Qt.Key_Escape)
            compare(closes.count, 1)
            compare(editor.savedId, "forest-path")
        }

        function test_saveTransitionsAndConfirmation() {
            click("privacyPreset:baikal-piano")
            tryVerify(function() { return control("privacyEditorSave").enabled })
            click("privacyEditorSave")
            compare(saves.count, 1)
            var id = saves.signalArguments[0][0]
            compare(saves.signalArguments[0][1].id, "baikal-piano")
            compare(editor.saveState, Stream.PrivacyScreensaverEditor.Saving)
            compare(editor.savedId, "forest-path")
            verify(!control("privacyEditorClose").enabled)
            verify(!control("privacyEditorImport").enabled)
            editor.requestSave()
            compare(saves.count, 1)
            verify(editor.completeSave(id, Stream.PrivacyScreensaverEditor.Failed, "保存失败"))
            compare(control("privacyEditorStatus").text, "保存失败")
            compare(control("privacyEditorSave").text, "重试保存")
            compare(editor.savedId, "forest-path")
            compare(editor.draft.id, "baikal-piano")

            click("privacyEditorSave")
            var retryId = saves.signalArguments[1][0]
            verify(id !== retryId)
            verify(!editor.completeSave(id, Stream.PrivacyScreensaverEditor.Succeeded))
            verify(editor.completeSave(retryId, Stream.PrivacyScreensaverEditor.Unconfirmed))
            compare(control("privacyEditorStatus").text, "保存结果待确认")
            verify(control("privacyEditorClose").enabled)
            verify(!control("privacyEditorSave").enabled)
            click("privacyPreset:baikal-accordion")
            verify(editor.reconcileSaved(retryId, preset("baikal-piano")))
            compare(editor.savedId, "baikal-piano")
            compare(editor.draft.id, "baikal-accordion")
            compare(control("privacyEditorStatus").text, "已保存到 书房主机")
            verify(editor.dirty)
        }

        function test_importSavedWithCanonicalId() {
            click("privacyEditorImport")
            var token = imports.signalArguments[0][0]
            var source = editor.presetUrl("sunflower-field")
            verify(editor.provideImport(token, custom("local-id", source)))
            tryVerify(function() { return control("privacyEditorSave").enabled })
            click("privacyEditorSave")
            compare(saves.signalArguments[0][1].id, "local-id")
            compare(saves.signalArguments[0][1].previewUrl, String(source))
            verify(editor.completeSave(saves.signalArguments[0][0], Stream.PrivacyScreensaverEditor.Succeeded, "",
                                       { kind: "custom", id: "host-id" }))
            compare(editor.savedId, "host-id")
            compare(editor.draft.id, "host-id")
            compare(editor.draft.previewUrl, String(source))
            compare(editor.draft.name, "本机图片.webp")
            verify(!editor.dirty)
        }

        function test_restoreNarrowScrollAfterLargePreview() {
            editor.width = 386
            editor.height = 320
            var scroller = control("privacyEditorScroll")
            var card = control("privacyPreset:sunset-sky")
            card.forceActiveFocus(Qt.TabFocusReason)
            tryVerify(function() { return scroller.contentY > 0 })
            scroller.contentY = Math.floor((scroller.contentHeight - scroller.height) * 0.62)
            var before = scroller.contentY
            click("privacyEditorTogglePreview")
            tryCompare(scroller, "contentY", 0)
            click("privacyEditorTogglePreview")
            tryCompare(scroller, "contentY", before)
        }

        function test_discardUnsubmittedAndIgnoreLateSave() {
            click("privacyPreset:mountain-valley")
            tryVerify(function() { return control("privacyEditorSave").enabled })
            click("privacyEditorSave")
            var id = saves.signalArguments[0][0]
            editor.endEdit()
            editor.beginEdit("卧室主机", preset("baikal-accordion"), preset("mountain-valley"), preset("sunflower-field"))
            verify(!editor.completeSave(id, Stream.PrivacyScreensaverEditor.Succeeded))
            compare(editor.savedId, "baikal-accordion")
            compare(editor.draft.id, "sunflower-field")
            compare(control("privacyEditorDevice").text, "被控设备 · 卧室主机")
        }

        function test_disabledAndRevealedFooterOnNarrow() {
            editor.width = 386
            editor.height = 320
            editor.available = false
            editor.disabledReason = "被控端暂不支持自定义屏保。"
            compare(control("privacyEditorStatus").text, editor.disabledReason)
            verify(!control("privacyPreset:mountain-valley").enabled)
            verify(!control("privacyEditorImport").enabled)
            verify(!control("privacyEditorReset").enabled)
            verify(!control("privacyEditorSave").enabled)
            var scroller = control("privacyEditorScroll")
            var previewSection = control("privacyEditorPreviewSection")
            var picker = control("privacyEditorPicker")
            var footer = control("privacyEditorFooter")
            verify(picker.y >= previewSection.y + previewSection.height)
            verify(scroller.contentHeight > scroller.height)
            verify(footer.mapToItem(editor, 0, footer.height).y <= editor.height)
            var footerY = footer.mapToItem(editor, 0, 0).y
            scroller.contentY = scroller.contentHeight - scroller.height
            compare(footer.mapToItem(editor, 0, 0).y, footerY)
        }
    }
}
