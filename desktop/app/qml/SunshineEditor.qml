// SPDX-License-Identifier: GPL-3.0-only
import QtQuick 2.15

QtObject {
    id: editor
    property var state: ({})
    property var options: []
    property var drafts: ({})
    property var inFlight: null
    property int nextId: 0
    property string session: ""
    property var continuation: null
    property bool canDiscard: false
    property bool requestedInitialLoad: false

    readonly property var config: state.config || ({})
    readonly property bool ready: state.status === "ready"
    readonly property bool dirty: Object.keys(drafts).length > 0
    readonly property bool failed: Object.keys(drafts).some(function(key) { return !!drafts[key].error })
    readonly property bool saving: inFlight !== null || (dirty && !failed)
    signal request(string action, var data)
    signal confirmDiscard()

    function value(key) {
        if (Object.prototype.hasOwnProperty.call(drafts, key))
            return drafts[key].value
        return config.values && Object.prototype.hasOwnProperty.call(config.values, key) ? config.values[key] : ""
    }

    function errorFor(key) {
        return Object.prototype.hasOwnProperty.call(drafts, key) ? drafts[key].error : ""
    }

    function edit(key, value, text) {
        if (value === editor.value(key))
            return
        const changes = Object.assign({}, drafts)
        if (config.values && config.values[key] === value && (!inFlight || !Object.prototype.hasOwnProperty.call(inFlight.changes, key)))
            delete changes[key]
        else
            changes[key] = { value: value, due: Date.now() + (text ? 650 : 0), error: "" }
        drafts = changes
        schedule()
    }

    function command(action, data) {
        request(action, Object.assign({ session: state.session || "" }, data || {}))
    }

    function loadIfNeeded(active) {
        if (!state.status)
            requestedInitialLoad = false
        if (active && state.status === "idle" && !requestedInitialLoad) {
            requestedInitialLoad = true
            command("sunshineLoad")
        }
    }

    function schedule() {
        saveTimer.stop()
        if (inFlight || !ready || state.busy)
            return
        let next = Infinity
        const keys = Object.keys(drafts)
        for (let i = 0; i < keys.length; ++i) {
            const entry = drafts[keys[i]]
            if (!entry.error)
                next = Math.min(next, continuation ? 0 : entry.due)
        }
        if (next !== Infinity) {
            saveTimer.interval = Math.max(0, next - Date.now())
            saveTimer.start()
        }
    }

    function submit() {
        if (inFlight || !ready || state.busy)
            return
        const changes = Object.create(null)
        const keys = Object.keys(drafts)
        const now = Date.now()
        for (let i = 0; i < keys.length; ++i) {
            const key = keys[i]
            if (!drafts[key].error && (continuation || drafts[key].due <= now))
                changes[key] = drafts[key].value
        }
        if (!Object.keys(changes).length) {
            schedule()
            return
        }
        inFlight = { id: ++nextId, changes: changes }
        command("sunshineSave", inFlight)
    }

    function settle(saved, errors, generalError) {
        const submitted = inFlight.changes
        const pending = Object.assign({}, drafts)
        const keys = Object.keys(submitted)
        for (let i = 0; i < keys.length; ++i) {
            const key = keys[i]
            if (!pending[key] || pending[key].value !== submitted[key])
                continue
            if (saved.indexOf(key) >= 0)
                delete pending[key]
            else
                pending[key] = { value: pending[key].value, due: 0, error: errors[key] || generalError }
        }
        inFlight = null
        drafts = pending
    }

    function resetForSession() {
        // A new administration session must never inherit an automatic write.
        if (dirty) {
            const changes = Object.assign({}, drafts)
            const keys = Object.keys(changes)
            for (let i = 0; i < keys.length; ++i)
                changes[keys[i]] = { value: changes[keys[i]].value, due: 0, error: qsTr("Sunshine 会话已变化，请重新确认设置。") }
            drafts = changes
        }
        inFlight = null
        options = []
    }

    onStateChanged: {
        const current = state.session || ""
        if (session !== current) {
            if (session.length)
                resetForSession()
            session = current
        }
        if (ready && options.length === 0)
            options = config.options || []
        if (inFlight && state.result && state.result.id === inFlight.id)
            settle(state.result.saved || [], state.result.errors || {}, qsTr("Sunshine 未确认此设置。"))
        finishIfPossible()
        schedule()
    }

    function retry() {
        if (!ready || state.busy || inFlight)
            return
        const changes = Object.assign({}, drafts)
        const keys = Object.keys(changes)
        for (let i = 0; i < keys.length; ++i)
            changes[keys[i]] = { value: changes[keys[i]].value, due: 0, error: "" }
        drafts = changes
        schedule()
    }

    function finish(action, allowDiscard) {
        if (continuation)
            return
        continuation = action
        canDiscard = allowDiscard
        finishIfPossible()
        schedule()
    }

    function finishIfPossible() {
        if (!continuation || inFlight)
            return
        if (!dirty) {
            const next = continuation
            continuation = null
            next()
        } else if (!ready || (failed && Object.keys(drafts).every(function(key) { return !!drafts[key].error }))) {
            if (canDiscard)
                confirmDiscard()
            else
                continuation = null
        }
    }

    function discard() {
        drafts = ({})
        finishIfPossible()
    }

    property Timer saveTimer: Timer { onTriggered: editor.submit() }
    property Timer acknowledgementTimer: Timer {
        interval: 30000
        running: editor.inFlight !== null
        onTriggered: {
            editor.settle([], {}, qsTr("等待 Sunshine 回读超时。请重新检测后再试。"))
            editor.finishIfPossible()
            editor.schedule()
        }
    }
}
