// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import FerrousUi 1.0
import "../components" as Components

// Shown when the settings, the queue, or Last.fm's scrobbles could not be
// saved before quitting.
Components.CompactDialogWindow {
    id: root

    // What could not be saved and why, as BridgeClient's
    // quitSaveFailureMap gives it: "settings", "queue" and "scrobbles",
    // "cause", a code put in words here, and "detail", the system's text.
    // The window's own timeout gives "notAnswered" as its cause.
    property var failure: ({})

    readonly property string leadText: {
        const failure = root.failure || {}
        if (failure.cause === "notAnswered") {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t confirm that its settings and the queue were saved.")
        }
        const settings = failure.settings === true
        const queue = failure.queue === true
        const scrobbles = failure.scrobbles === true
        if (settings && queue && scrobbles) {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t save its settings, the queue or unsent Last.fm scrobbles.")
        }
        if (settings && queue) {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t save its settings or the queue.")
        }
        if (settings && scrobbles) {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t save its settings or unsent Last.fm scrobbles.")
        }
        if (queue && scrobbles) {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t save the queue or unsent Last.fm scrobbles.")
        }
        if (settings) {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t save its settings.")
        }
        if (queue) {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t save the queue.")
        }
        if (scrobbles) {
            return I18n.i18nc("@info", "Ferrous couldn\u2019t save unsent Last.fm scrobbles.")
        }
        return I18n.i18nc("@info", "Some recent changes weren\u2019t saved.")
    }

    // The cause in plain words; empty for none it can name. A denied or
    // read-only folder is named: the settings, the queue and Last.fm's
    // unsent scrobbles are all saved in Ferrous's settings folder ("this
    // folder" named none until the system's text under it). The others are
    // worded as Tag Change Details words a tag save's.
    readonly property string causeText: {
        switch (String((root.failure || {}).cause || "")) {
        case "permission":
            return I18n.i18nc("@info why Ferrous's settings, queue or scrobbles could not be saved",
                "You don\u2019t have permission to write in Ferrous\u2019s settings folder.")
        case "readOnlyFileSystem":
            return I18n.i18nc("@info why Ferrous's settings, queue or scrobbles could not be saved",
                "Ferrous\u2019s settings folder is on a read-only file system.")
        case "noSpace":
            return I18n.i18nc("@info why a file could not be saved", "There isn\u2019t enough free space on the disk.")
        case "busy":
            return I18n.i18nc("@info why Last.fm scrobbles could not be saved before quitting",
                "Last.fm is busy, possibly waiting for the network, and didn\u2019t save them in time.")
        case "engineStopped":
            return I18n.i18nc("@info why the settings and the queue could not be saved",
                "The playback engine has stopped.")
        case "notAnswered":
            return I18n.i18nc("@info why the settings and the queue may not have been saved",
                "Saving didn\u2019t finish in time.")
        default:
            return ""
        }
    }

    // What Quit Anyway loses, named from the same flags as the lead (one
    // fixed sentence named scrobbles under a lead that said only the
    // settings had failed). Where Ferrous can't tell whether they were
    // saved, it says they may be lost.
    readonly property string consequenceText: {
        const failure = root.failure || {}
        if (failure.cause === "notAnswered" || failure.cause === "engineStopped") {
            return I18n.i18nc("@info", "Quit Anyway may lose unsaved settings and queue changes.")
        }
        const settings = failure.settings === true
        const queue = failure.queue === true
        const scrobbles = failure.scrobbles === true
        if (settings && queue && scrobbles) {
            return I18n.i18nc("@info", "Quit Anyway loses unsaved settings, queue changes and unsent Last.fm scrobbles.")
        }
        if (settings && queue) {
            return I18n.i18nc("@info", "Quit Anyway loses unsaved settings and queue changes.")
        }
        if (settings && scrobbles) {
            return I18n.i18nc("@info", "Quit Anyway loses unsaved settings and unsent Last.fm scrobbles.")
        }
        if (queue && scrobbles) {
            return I18n.i18nc("@info", "Quit Anyway loses unsaved queue changes and unsent Last.fm scrobbles.")
        }
        if (settings) {
            return I18n.i18nc("@info", "Quit Anyway loses unsaved settings.")
        }
        if (queue) {
            return I18n.i18nc("@info", "Quit Anyway loses unsaved queue changes.")
        }
        if (scrobbles) {
            return I18n.i18nc("@info", "Quit Anyway loses unsent Last.fm scrobbles.")
        }
        return I18n.i18nc("@info", "Quit Anyway loses the changes that weren\u2019t saved.")
    }
    readonly property string detailText: String((root.failure || {}).detail || "")
    // The system's errors, one a line as the backend joins them.
    readonly property var detailLines: root.detailText.split("\n")
        .map(line => line.trim()).filter(line => line.length > 0)

    signal retryRequested()
    signal quitRequested()

    objectName: "quitSaveFailedDialog"
    title: I18n.i18nc("@title:window", "Couldn\u2019t Save Before Quitting")
    // Escape and the title bar's close button keep Ferrous open.
    // At least as wide as the heading, which is one line in any font.
    contentWidth: quitSaveFailedHeading.implicitWidth

    footer: Components.DialogFooter {
        id: dialogButtons
        primary: quitSaveFailedRetryButton
        Button {
            id: quitSaveFailedRetryButton
            objectName: "quitSaveFailedRetryButton"
            text: I18n.i18nc("@action:button", "Retry")
            icon.name: "view-refresh"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: {
                // Hidden at once: a second click finds no button.
                root.dismiss()
                root.retryRequested()
            }
        }
        Button {
            objectName: "quitSaveFailedQuitButton"
            text: I18n.i18nc("@action:button", "Quit Anyway")
            icon.name: "application-exit"
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                root.dismiss()
                root.quitRequested()
            }
        }
    }

    // What wasn't saved, then why in plain words, then the system's text,
    // such as "failed to replace …: Permission denied (os error 13)", in
    // secondary ink, as Tag Change Details orders a failed file's lines.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2

        Label {
            id: quitSaveFailedHeading
            objectName: "quitSaveFailedHeading"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            // The lead, semibold, as every dialog's first sentence is (the
            // three lines read as one grey block). It names what wasn't
            // saved ("Some recent changes weren't saved." left that to the
            // system's text under it).
            font.weight: Font.DemiBold
            textFormat: Text.PlainText
            text: root.leadText
        }

        Label {
            objectName: "quitSaveFailedCause"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: root.causeText
        }

        // The system's errors, each a paragraph of its own in the small
        // font and secondary ink, 6 px apart and set off from the cause
        // above (they ran together as one grey block at the cause's size,
        // and one error's wrapped line began on the next's edge). A path
        // in one breaks between its folders, and the error's code stays
        // whole ("13)" was alone on a line).
        ColumnLayout {
            objectName: "quitSaveFailedMessage"
            // The errors as the backend gave them.
            readonly property string message: root.detailText
            Layout.fillWidth: true
            Layout.topMargin: root.uiPalette.scaledSize(4)
            visible: root.detailLines.length > 0
            spacing: root.uiPalette.scaledSize(6)

            Repeater {
                model: root.detailLines

                Components.MessageLabel {
                    required property string modelData
                    objectName: "quitSaveFailedMessageLine"
                    Layout.fillWidth: true
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.textSecondary
                    message: modelData
                }
            }
        }
    }

    Label {
        objectName: "quitSaveFailedDetails"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        // What quitting now loses, by its name, rather than "them", which
        // could be read as the error above it (a sentence on unsaved
        // changes first said "not saved" a third time). Retry says what it
        // does itself ("Retry saves again." repeated it). It suggests no
        // cure: the cause above says why, which is not always a full disk.
        textFormat: Text.PlainText
        text: root.consequenceText
    }
}
