// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQml 2.15
import "../logic/ErrorUtils.js" as ErrorUtils

// The errors shown to the user: the latest in banners, and a short history.
// An error that repeats one in the history is counted there, not added.
QtObject {
    id: root

    // How many different errors the history keeps.
    property int historyLimit: 20
    // Newest first: category, severity, subject, message, count, and the
    // times it was first and last reported, in ms since the epoch.
    property ListModel entries: ListModel {}
    readonly property int count: entries.count
    // Whether the banners show the newest entry; the user can dismiss it.
    property bool latestShown: false
    property bool historyExpanded: false
    // Bumped whenever an entry changes, for bindings to the newest one.
    property int revision: 0
    readonly property var latest: {
        root.revision
        return root.entries.count > 0 ? root.entries.get(0) : null
    }

    function report(category, severity, subject, message) {
        const text = String(message || "").trim()
        if (text.length === 0) {
            return
        }
        const kind = Number(category) || 0
        const level = Number(severity) === ErrorUtils.SeverityWarning
            ? ErrorUtils.SeverityWarning
            : ErrorUtils.SeverityError
        const about = String(subject || "")
        // The diagnostics log keeps every report.
        console.warn("error [" + ErrorUtils.categoryLabel(kind) + "]: " + text
            + (about.length > 0 ? " (" + about + ")" : ""))
        const now = Date.now()
        for (let i = 0; i < root.entries.count; ++i) {
            const entry = root.entries.get(i)
            if (entry.category === kind && entry.subject === about && entry.message === text) {
                root.entries.setProperty(i, "count", entry.count + 1)
                root.entries.setProperty(i, "lastSeenMs", now)
                root.entries.setProperty(i, "severity", level)
                if (i > 0) {
                    root.entries.move(i, 0, 1)
                }
                root.latestShown = true
                root.revision++
                return
            }
        }
        root.entries.insert(0, {
            category: kind,
            severity: level,
            subject: about,
            message: text,
            count: 1,
            firstSeenMs: now,
            lastSeenMs: now
        })
        while (root.entries.count > Math.max(1, root.historyLimit)) {
            root.entries.remove(root.entries.count - 1)
        }
        root.latestShown = true
        root.revision++
    }

    // Hide the banners until the next error; the history keeps it.
    function dismissLatest() {
        root.latestShown = false
    }

    function remove(index) {
        if (index < 0 || index >= root.entries.count) {
            return
        }
        root.entries.remove(index)
        if (index === 0) {
            root.latestShown = false
        }
        if (root.entries.count === 0) {
            root.historyExpanded = false
        }
        root.revision++
    }

    function clear() {
        root.entries.clear()
        root.latestShown = false
        root.historyExpanded = false
        root.revision++
    }

    function toggleHistory() {
        root.historyExpanded = !root.historyExpanded && root.entries.count > 0
    }
}
