// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQml 2.15
import "../logic/ErrorUtils.js" as ErrorUtils

// The errors shown to the user: the status bar counts them, the viewers'
// banners show the latest, and the recent errors dialog lists a short history.
// An error that repeats one in the history is counted there, not added.
QtObject {
    id: root

    // How many different errors the history keeps.
    property int historyLimit: 20
    // Newest first: category, severity, subject, message, count, and the
    // times it was first and last reported, in ms since the epoch.
    property ListModel entries: ListModel {}
    readonly property int count: entries.count
    // Whether the user has yet to see the newest entry, until the user
    // dismisses it or opens the history.
    property bool latestUnseen: false
    // How many entries arrived or repeated since the user last saw the
    // history: the status bar's "2 new issues". They stay new while the
    // history is open, and are seen when it closes; see markSeen().
    readonly property int unseenCount: {
        root.revision
        let unseen = 0
        for (let i = 0; i < root.entries.count; ++i) {
            if (root.entries.get(i).unseen) {
                ++unseen
            }
        }
        return unseen
    }
    // Bumped whenever an entry changes, for bindings to the newest one.
    property int revision: 0
    readonly property var latest: {
        root.revision
        return root.entries.count > 0 ? root.entries.get(0) : null
    }

    // The files the playback could not read, from the history: each
    // playback error's file and its message. The queue and the library mark
    // their rows; they stay marked while the history keeps the error.
    readonly property var unreadablePaths: {
        root.revision
        const paths = ({})
        for (let i = root.entries.count - 1; i >= 0; --i) {
            const entry = root.entries.get(i)
            if (entry.category === ErrorUtils.CategoryPlayback
                    && entry.severity === ErrorUtils.SeverityError
                    && entry.subject.length > 0) {
                paths[entry.subject] = entry.message
            }
        }
        return paths
    }

    // Asks for the history to open.
    signal historyRequested()
    // An error arrived or repeated, and is the newest entry now.
    signal reported()

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
                root.entries.setProperty(i, "unseen", true)
                if (i > 0) {
                    root.entries.move(i, 0, 1)
                }
                root.latestUnseen = true
                root.revision++
                root.reported()
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
            lastSeenMs: now,
            unseen: true
        })
        while (root.entries.count > Math.max(1, root.historyLimit)) {
            root.entries.remove(root.entries.count - 1)
        }
        root.latestUnseen = true
        root.revision++
        root.reported()
    }

    // Hide the banners until the next error; the history keeps it.
    function dismissLatest() {
        root.latestUnseen = false
    }

    // The user has seen the whole history: nothing in it is new any more.
    function markSeen() {
        root.latestUnseen = false
        for (let i = 0; i < root.entries.count; ++i) {
            if (root.entries.get(i).unseen) {
                root.entries.setProperty(i, "unseen", false)
            }
        }
        root.revision++
    }

    function remove(index) {
        if (index < 0 || index >= root.entries.count) {
            return
        }
        root.entries.remove(index)
        if (index === 0) {
            root.latestUnseen = false
        }
        root.revision++
    }

    function clear() {
        root.entries.clear()
        root.latestUnseen = false
        root.revision++
    }
}
