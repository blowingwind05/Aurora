// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

PrivacyScreensaverEditor {
    // The Mac host applies changes the next time the screensaver is enabled.
    screensaverActive: false
    importHint: qsTr("JPG、PNG 或 WebP")

    function finishMacSave(requestId, result, message) {
        const outcome = result === "saved" ? PrivacyScreensaverEditor.Succeeded
            : result === "unconfirmed" ? PrivacyScreensaverEditor.Unconfirmed : PrivacyScreensaverEditor.Failed
        completeSave(requestId, outcome, message)
    }
}
