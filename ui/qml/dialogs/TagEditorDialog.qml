// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/FormatUtils.js" as FormatUtils
import "../logic/KeyUtils.js" as KeyUtils
import "." as Dialogs

// Edit Tags, in a window of its own: KWin draws its title bar, and it
// moves and resizes beyond the main window's edges, which it blocks while
// it shows. Its questions open over it. Escape and the title bar's close
// button ask about unsaved edits first, as Cancel does.
Components.DialogWindow {
    id: root

    required property var tagEditorApi

    property int selectionAnchor: -1
    readonly property int totalRows: tagEditorTable.count
    readonly property int selectedCount: root.tagEditorApi.selectedCount || 0
    property bool operationInFlight: false
    property string operationText: ""
    // A rename preview runs; a load started meanwhile drops its answer.
    property bool renamePreviewPending: false
    property bool closeAfterSave: false
    // Ferrous is waiting to quit until the edits here are saved or dropped.
    property bool quitPending: false
    // The user chose to quit without waiting for the running save or rename,
    // which the exit leaves running.
    property bool quitWithoutOperation: false
    // How long quitting waits for a save or rename before it offers to quit
    // without it, as quitting does for the settings and queue.
    readonly property int quitWaitBoundMs: 5000
    readonly property real tableRowHeight: root.uiPalette.rowHeight
    // The status of a row with unsaved edits: its mark's tooltip and
    // description, and its Status cell while that column shows.
    readonly property string modifiedText: I18n.i18nc("@item tag editor row status", "Modified")
    // The Status cell of a row whose save or rename failed: why is in its
    // tooltip and in Details… (the system's own words, in the cell, were
    // elided to "failed to …").
    readonly property string notSavedText: I18n.i18nc("@item tag editor row status: saving the file's tags failed", "Not saved")
    readonly property string notRenamedText: I18n.i18nc("@item tag editor row status: renaming the file failed", "Not renamed")
    // Shown in a field whose rows hold different values, which saving
    // keeps: one word, the same in every field. It is short enough for
    // Track's and Disc's beside their Revert at any font ("Multiple values"
    // didn't fit there, so the whole form changed its word to "Mixed" as
    // one of them gained its Revert; widening them took the table's room).
    readonly property string keepText: I18n.i18nc("@info:placeholder short, in fields as narrow as a track number's: the selected files keep their different values", "Mixed")
    readonly property bool statusHasFailure: root.tagEditorApi.statusIsError === true
    readonly property bool busy: root.operationInFlight || root.tagEditorApi.loading || root.tagEditorApi.saving
    // Bumped whenever the selection or the edits change, for the bindings
    // that ask the editor which fields changed.
    property int summaryRevision: 0
    readonly property var editorFields: [
        { key: "title", label: I18n.i18nc("@label tag field", "Title"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Title"), formLabel: I18n.i18nc("@label:textbox tag field", "Title:"), allowTitleCase: true, allowCapitalize: true },
        { key: "artist", label: I18n.i18nc("@label tag field", "Artist"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Artist"), formLabel: I18n.i18nc("@label:textbox tag field", "Artist:"), allowTitleCase: true, allowCapitalize: true },
        { key: "album", label: I18n.i18nc("@label tag field", "Album"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Album"), formLabel: I18n.i18nc("@label:textbox tag field", "Album:"), allowTitleCase: true, allowCapitalize: true },
        { key: "albumArtist", label: I18n.i18nc("@label tag field", "Album Artist"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Album artist"), formLabel: I18n.i18nc("@label:textbox tag field", "Album artist:"), allowTitleCase: true, allowCapitalize: true },
        { key: "genre", label: I18n.i18nc("@label tag field", "Genre"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Genre"), formLabel: I18n.i18nc("@label:textbox tag field", "Genre:"), allowTitleCase: false, allowCapitalize: true },
        { key: "year", label: I18n.i18nc("@label tag field", "Year"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Year"), formLabel: I18n.i18nc("@label:textbox tag field", "Year:"), allowTitleCase: false, allowCapitalize: false },
        { key: "trackNo", label: I18n.i18nc("@label tag field", "Track"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Track"), formLabel: I18n.i18nc("@label:textbox tag field", "Track:"), allowTitleCase: false, allowCapitalize: false },
        { key: "totalTracks", label: I18n.i18nc("@label tag field", "Tracks"), row: "trackNo", formLabel: "", allowTitleCase: false, allowCapitalize: false },
        { key: "discNo", label: I18n.i18nc("@label tag field", "Disc"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Disc"), formLabel: I18n.i18nc("@label:textbox tag field", "Disc:"), allowTitleCase: false, allowCapitalize: false },
        { key: "totalDiscs", label: I18n.i18nc("@label tag field", "Discs"), row: "discNo", formLabel: "", allowTitleCase: false, allowCapitalize: false },
        { key: "comment", label: I18n.i18nc("@label tag field", "Comment"), rowLabel: I18n.i18nc("@item:intext tag field, as its row in the form is labelled", "Comment"), formLabel: I18n.i18nc("@label:textbox tag field", "Comment:"), allowTitleCase: false, allowCapitalize: false }
    ]
    // The table's columns, each at its width with the default 10 pt font,
    // scaled with a larger one, or wider when its header or sample needs
    // it. File Name is straight after #, as what tells the rows apart, in
    // view where the table scrolls (from a 1000 px window, it was wholly
    // out of view). `sample` is the widest value a short column holds
    // whole: the numbers and Format are as narrow as that allows. Where the
    // view is narrower than the columns together, the columns that may
    // `give` way do, before the table scrolls sideways: first File Name,
    // Folder and Status, down to their least, then the tags, down to
    // theirs; the numbers and Format keep their widths. A column's least is
    // its `least` width, or more for its values, up to what it `holds`:
    // File Name whole up to 220 px, and Title, Artist and Album no narrower
    // than their values or their header up to 160, scaled with the font (at
    // 160 whatever they held, they left File Name at 140, which cut three
    // of four names beside room to spare). A tag keeps 100 for what is
    // typed over a short value, and Title 160 with no value yet to
    // measure. Artist, Album and Genre, which `fit`, are no wider than
    // their least whatever the room (see tableColumnBaseWidths). Folder,
    // which may be `cut`, gives only what its values don't need at first,
    // then on down to its `least`. A table that scrolls has each of
    // them at its least and none wider than the view, but Title, whose new
    // values are checked there before Save: it has all the room beside #,
    // File Name and Disc, which follows it, to the view's edge (it stayed
    // at 190 px beside an unchanged Artist), or `holds` to scroll to where
    // that would be less than its `least`. Disc is beside it so that a
    // batch's new disc numbers are checked in the same view as its titles
    // (at 24 pt Title took the view to its edge, and a changed Disc was
    // out of it); with too little room for both, Title has it all. A
    // committed change is whole
    // whatever its column's width: it wraps, and its row grows while that
    // column is in view. Where
    // the view is wider, Title, File Name and Folder, which `want` room,
    // take it in turn, up to what they need, and then File Name and Folder,
    // which `stretch`, take the rest. Status takes no room until a
    // save or a rename failed for a row; a row with unsaved edits has a
    // mark in the # column instead, whose room is always there, so that an
    // edit moves no column (a Status column that came with the first edit
    // took its width from File Name and Folder). The window opens wide
    // enough for every column, so none gives way on a large screen (with
    // too little room for them all, either the tags or the paths were cut
    // short).
    readonly property var tableColumns: [
        // "Track", which says what it numbers: "#" read as the rows' own
        // numbering, in a batch numbered in order, beside a changed Disc.
        { key: "trackNo", label: I18n.i18nc("@title:column track number", "Track"), width: 40, number: true, sample: "000" },
        { key: "fileName", label: I18n.i18nc("@title:column", "File Name"), width: 220, give: 1, least: 140, holds: 220, wants: 480, stretch: true, path: true },
        { key: "title", label: I18n.i18nc("@title:column", "Title"), width: 190, give: 2, least: 160, holds: 160, wants: 400, wantsAtLeast: 240 },
        { key: "discNo", label: I18n.i18nc("@title:column", "Disc"), width: 40, sample: "00" },
        { key: "artist", label: I18n.i18nc("@title:column", "Artist"), width: 170, give: 2, least: 100, holds: 160, fit: true },
        { key: "album", label: I18n.i18nc("@title:column", "Album"), width: 170, give: 2, least: 100, holds: 160, fit: true },
        { key: "genre", label: I18n.i18nc("@title:column", "Genre"), width: 100, give: 2, least: 70, holds: 160, fit: true },
        { key: "year", label: I18n.i18nc("@title:column", "Year"), width: 52, sample: "0000" },
        { key: "formatKind", label: I18n.i18nc("@title:column file format", "Format"), width: 56, sample: "FLAC" },
        { key: "directory", label: I18n.i18nc("@title:column", "Folder"), width: 156, give: 1, least: 100, holds: 156, wants: 320, cut: true, stretch: true, path: true },
        { key: "status", label: I18n.i18nc("@title:column", "Status"), width: 120, give: 1, least: 0, samples: [root.modifiedText, root.notSavedText, root.notRenamedText], onlyWithStatus: true }
    ]
    // Whether a row has a status to read: a save or rename that failed for
    // it.
    readonly property bool rowStatusShown: root.tagEditorApi.hasRowErrors === true
    // The mark of a row with unsaved edits, in the # column's left padding:
    // a 6 px dot at the default font, clear of a three-digit number, whose
    // room the # column has beside its Revert's.
    readonly property int modifiedMarkSize: 2 * Math.round(root.uiPalette.scaledSize(6) / 2)
    // In its place on a row whose save or rename failed: Breeze's
    // dialog-error, its square 12 px at the default font, in the negative
    // colour (the row had the dot of an edit not yet saved, and its failure
    // only in Status, at the table's far end).
    readonly property string failureGlyph: Qt.resolvedUrl("../assets/status-error.svg")
    readonly property int failureGlyphSize: 2 * Math.round(root.uiPalette.scaledSize(16) / 2)
    // The marks' slot, at the start of the # column's left padding: as
    // wide as the error glyph's square, or 8 px; each mark is centred in it.
    readonly property real markSlotWidth: Math.max(Kirigami.Units.largeSpacing,
        Math.ceil(root.failureGlyphSize * 3 / 4))
    // The # column's padding before its numbers: the marks' slot, and a
    // pixel clear of it.
    readonly property real numberLeadingRoom: root.markSlotWidth + 1
    readonly property real tableInset: Kirigami.Units.largeSpacing
    // The room a column's header keeps after its label for the column's
    // Revert, up to the column's edge, whether it shows or not, so that
    // marking a change moves no column: "Title ↺", and "Track ↺" alike,
    // with the digits ending where "Track" does; its glyph is as large as a line of
    // text holds, 14 px in a 20 px button with the default font.
    readonly property real columnRevertGlyph: root.uiPalette.labelIconSize - 2
    readonly property real columnRevertRoom: root.uiPalette.labelIconSize + 2 * Kirigami.Units.smallSpacing
    // What a column needs whatever the room: its header whole, semibold
    // and, in a column of a field, with its Revert in place of the padding
    // at that end, or its sample value, with the cell's padding, which in
    // a column of numbers ends under the label, rather than a header
    // elided to a letter. The Revert's room is there before
    // an edit needs it (#, Year and Disc took it as they were typed in,
    // and moved every column up to 14 px).
    function columnNeed(column) {
        // The bindings that ask follow the fonts: advanceWidth() is a
        // function, which they cannot follow.
        columnHeaderMetrics.font
        columnValueMetrics.font
        const padding = Kirigami.Units.largeSpacing
        const leading = column.number ? root.numberLeadingRoom : padding
        const header = columnHeaderMetrics.advanceWidth(column.label) + leading
            + (root.fieldAt(column.key) !== null ? root.columnRevertRoom : padding)
        const samples = column.samples || (column.sample ? [column.sample] : [])
        const value = samples.reduce(function(widest, sample) {
            return Math.max(widest, columnValueMetrics.advanceWidth(sample))
        }, 0)
        // The label lays out up to a pixel wider than its advance.
        return Math.ceil(Math.max(header + 1, column.number
            ? value + 1 + leading + root.columnRevertRoom
            : value + 2 * padding))
    }
    // Each column's width with room enough, or none for Status while no
    // row has a failure.
    readonly property var tableColumnWidths: root.tableColumns.map(function(column) {
        if (column.onlyWithStatus && !root.rowStatusShown) {
            return 0
        }
        return Math.max(root.uiPalette.scaledSize(column.width), root.columnNeed(column))
    })
    // The rows the columns are measured by: the first measuredRowLimit, as
    // the files loaded or were loaded again, so that typing moves no column
    // and a large batch costs as little as a small one. Null until then.
    readonly property int measuredRowLimit: 200
    property var measuredRows: null
    // What the measured values of a column that `holds` them need, with
    // the cell's padding, whatever its width; 0 with none, or none to
    // measure.
    readonly property var tableColumnMeasuredWidths: {
        // As in columnNeed, for the binding to follow the font.
        columnValueMetrics.font
        const rows = root.measuredRows || []
        return root.tableColumns.map(function(column) {
            if (!column.holds) {
                return 0
            }
            let widest = 0
            for (let i = 0; i < rows.length; ++i) {
                widest = Math.max(widest, columnValueMetrics.advanceWidth(root.cellText(column.key, rows[i])))
            }
            // A cell lays its text out up to a pixel or so wider than its
            // advance (349.5 px for 349 at 24 pt), and elided it.
            return widest > 0 ? Math.ceil(widest) + 2 + 2 * Kirigami.Units.largeSpacing : 0
        })
    }
    // What the values of a column that `holds` them need, up to what it
    // holds, which it is with none to measure; a tag that `fit`s its
    // values, measured and empty, needs none.
    readonly property var tableColumnContentWidths: root.tableColumns.map(function(column, index) {
        if (!column.holds) {
            return 0
        }
        const measured = root.tableColumnMeasuredWidths[index]
        const most = root.uiPalette.scaledSize(column.holds)
        return measured > 0 ? Math.min(most, measured) : (column.fit && root.measuredRows !== null ? 0 : most)
    })
    // What each column has before the room is shared out: its width, or,
    // for a tag that `fit`s its values (Artist, Album, Genre), what they
    // need as the files loaded, within its least and what it holds, and
    // Folder, which may be `cut`, its least. The room that frees goes to
    // the columns that `want` it (at their widths, Artist and Album were
    // half empty beside new titles three lines tall and file names cut
    // short).
    readonly property var tableColumnBaseWidths: root.tableColumns.map(function(column, index) {
        return column.fit || column.cut ? root.tableColumnLeastWidths[index] : root.tableColumnWidths[index]
    })
    // How wide a column that `wants` room would be, given it: what its
    // values need as the files loaded, or what it `wantsAtLeast`, up to
    // what it `wants`; others, their base.
    readonly property var tableColumnWantWidths: root.tableColumns.map(function(column, index) {
        if (!column.wants) {
            return root.tableColumnBaseWidths[index]
        }
        return Math.ceil(Math.min(root.uiPalette.scaledSize(column.wants), Math.max(
            root.tableColumnMeasuredWidths[index], root.uiPalette.scaledSize(column.wantsAtLeast || 0))))
    })
    // The narrowest each column gives way to: its least width, what its
    // values need, or what its header or sample needs.
    readonly property var tableColumnLeastWidths: root.tableColumns.map(function(column, index) {
        const preferred = root.tableColumnWidths[index]
        const least = column.give
            ? Math.max(root.uiPalette.scaledSize(column.least), column.cut ? 0 : root.tableColumnContentWidths[index])
            : preferred
        return Math.min(preferred, Math.max(least, root.columnNeed(column)))
    })
    // The widths the columns are shown at in the table's width.
    readonly property var shownColumnWidths: {
        const preferred = root.tableColumnBaseWidths
        const least = root.tableColumnLeastWidths.slice()
        const room = tagEditorTable.width - 2 * root.tableInset
        const total = preferred.reduce(function(sum, width) { return sum + width }, 0)
        const widths = preferred.slice()
        let over = total - room
        if (over <= 0) {
            // The room to spare goes, in turn, to what each column that
            // `wants` it needs for the values it had as the files loaded,
            // up to that: Title, whose new values are checked here before
            // Save, then File Name, which tells the rows apart, then
            // Folder. Title wants room for two lines of a long title too,
            // as what is typed over short ones is often longer (at 190 px
            // a batch's new titles took three). Then the columns that
            // stretch share what is left.
            let spare = -over
            for (const key of ["title", "fileName", "directory"]) {
                const i = root.tableColumns.findIndex(function(column) { return column.key === key })
                const taken = Math.max(0, Math.min(spare, root.tableColumnWantWidths[i] - widths[i]))
                widths[i] += taken
                spare -= taken
            }
            const stretching = root.tableColumns.filter(function(column) { return column.stretch }).length
            const extra = Math.floor(spare / stretching)
            root.tableColumns.forEach(function(column, i) {
                if (column.stretch) {
                    widths[i] += extra
                }
            })
            over = 0
        }
        // File Name holds its values within the view beside #, where its
        // end elides, rather than past the view's edge, which cut it with
        // no ellipsis.
        const name = root.tableColumns.findIndex(function(column) { return column.key === "fileName" })
        const number = root.tableColumns.findIndex(function(column) { return column.key === "trackNo" })
        least[name] = Math.min(least[name],
            Math.max(root.columnNeed(root.tableColumns[name]), room - preferred[number]))
        const slackOf = function(group, floor) {
            let slack = 0
            root.tableColumns.forEach(function(column, i) {
                if (column.give === group) {
                    slack += widths[i] - floor[i]
                }
            })
            return slack
        }
        // The first group down to its values, then to its least, before the
        // next gives any: where that is still too wide, every column is at
        // its least and the table scrolls (keeping their widths, they pushed
        // what identifies a row out of view).
        const values = least.map(function(width, i) {
            return root.tableColumns[i].cut
                ? Math.min(preferred[i], Math.max(width, root.tableColumnContentWidths[i]))
                : width
        })
        for (const [group, floor] of [[1, values], [1, least], [2, least]]) {
            const slack = slackOf(group, floor)
            if (slack <= 0 || over <= 0) {
                continue
            }
            const taken = Math.min(over, slack)
            root.tableColumns.forEach(function(column, i) {
                if (column.give === group) {
                    widths[i] = Math.max(floor[i], Math.floor(widths[i] - taken * (widths[i] - floor[i]) / slack))
                }
            })
            over -= taken
        }
        // Still too wide, the table scrolls: Title to the view's edge, or,
        // with less than its least left there, 160 px to scroll to; and
        // no column wider than the view, which a change wraps within, so
        // that it is whole once scrolled to, clear of the vertical bar.
        if (over > 0) {
            const view = Math.floor(room - root.scrollBarClearance)
            // # and File Name, which stay in view as the rest scroll, take
            // no more than about half of it (at 24 pt from a 1000 px window
            // they took four fifths, and Folder never showed whole), or,
            // where their headers alone need more, stay unpinned.
            const pinnedMost = Math.floor(view * root.pinnedShare)
            const pins = widths[number] + root.columnNeed(root.tableColumns[name]) <= pinnedMost
            if (pins) {
                widths[name] = Math.min(widths[name], pinnedMost - widths[number])
            }
            const pinned = pins ? widths[number] + widths[name] : 0
            const title = root.tableColumns.findIndex(function(column) { return column.key === "title" })
            const disc = root.tableColumns.findIndex(function(column) { return column.key === "discNo" })
            const beside = Math.floor(room - widths[number] - widths[name])
            const titleLeast = root.uiPalette.scaledSize(root.tableColumns[title].least)
            widths[title] = !pins && beside < titleLeast
                ? root.uiPalette.scaledSize(root.tableColumns[title].holds)
                : beside - widths[disc] >= titleLeast ? beside - widths[disc] : beside
            // Every other column whole, once scrolled to, in the view or
            // beside the pinned columns.
            root.tableColumns.forEach(function(column, i) {
                if (!pins || i !== title && i >= root.pinnedColumnCount) {
                    widths[i] = Math.min(widths[i], Math.max(root.columnNeed(column), view - pinned))
                }
            })
        }
        // The whole pixels the rounding left go to the last column that
        // stretches, so Status ends at the table's inset.
        const left = Math.floor(room - widths.reduce(function(sum, width) { return sum + width }, 0))
        if (left > 0) {
            for (let i = root.tableColumns.length - 1; i >= 0; --i) {
                if (root.tableColumns[i].stretch) {
                    widths[i] += left
                    break
                }
            }
        }
        return widths
    }
    // How far the vertical bar reaches past the table's inset and a cell's
    // padding: 5 px with Breeze's. Title's text keeps that clear at the
    // view's edge, rather than the column ending short of it, which let
    // slivers of the next show beside the bar.
    readonly property real scrollBarClearance: Math.max(0,
        tagEditorDownBar.implicitWidth - root.tableInset - Kirigami.Units.largeSpacing)
    // The column whose text keeps clear of the vertical bar where the table
    // scrolls sideways: the one that ends at the view's edge as it opens,
    // Disc beside Title, or else Title.
    readonly property string scrollBarEdgeKey: {
        const edge = tagEditorTable.width - 2 * root.tableInset
        let end = 0
        for (let i = 0; i < root.shownColumnWidths.length; ++i) {
            end += root.shownColumnWidths[i]
            if (root.shownColumnWidths[i] > 0 && Math.abs(end - edge) < 1) {
                return root.tableColumns[i].key
            }
        }
        return "title"
    }
    readonly property bool tableScrolls: root.tableContentWidth > tagEditorTable.width
    // The rows' width: the view's, or the columns' where they are wider
    // still, when the table scrolls sideways.
    readonly property real tableContentWidth: Math.max(tagEditorTable.width,
        root.shownColumnWidths.reduce(function(sum, width) { return sum + width }, 2 * root.tableInset))

    function columnWidth(index) {
        return root.shownColumnWidths[index]
    }

    // Where the table scrolls sideways, what identifies a row stays at its
    // left: #, with the change mark, and File Name, the first two columns,
    // with a 1 px seam after them, while the others scroll under them (a
    // change in Disc was checked with its file scrolled out of view).
    readonly property int pinnedColumnCount: 2
    // The most of the view they take; more, and they scroll with the rest.
    readonly property real pinnedShare: 0.55
    readonly property bool pinsColumns: root.tableScrolls && root.shownColumnWidths[0] + root.shownColumnWidths[1]
        <= Math.floor((tagEditorTable.width - 2 * root.tableInset - root.scrollBarClearance) * root.pinnedShare)
    readonly property real pinnedShift: root.pinsColumns ? tagEditorTable.contentX : 0
    // From the row's edge to the seam's far side.
    readonly property real pinnedWidth: root.tableInset + root.shownColumnWidths[0] + root.shownColumnWidths[1]

    // The room above a cell's first line and under its last, in a row as
    // tall as one line as in one that wraps, so that all start on one line.
    readonly property real cellLineInset: (root.tableRowHeight - columnValueMetrics.height) / 2
    // The lines a file name takes before it is cut: three, as a title
    // wrapped at 24 pt takes, so a long name keeps its extension (on two
    // it read "and a Distant Thunderst…" beside a three-line title).
    readonly property int fileNameLineLimit: 3

    FontMetrics {
        id: columnHeaderMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
        font.weight: Font.DemiBold
    }

    FontMetrics {
        id: columnValueMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
    }

    // The fields the edits changed, by their labels.
    function changedFieldLabels() {
        root.summaryRevision
        const labels = []
        for (let i = 0; i < root.editorFields.length; ++i) {
            if (root.tagEditorApi.bulkFieldChanged(root.editorFields[i].key)) {
                labels.push(root.editorFields[i])
            }
        }
        return labels
    }

    // The form's rows that hold the changed fields, in the form's order, by
    // the form's labels: a total is its number's row, "Track".
    function changedRowLabels(changed) {
        const labels = []
        for (let i = 0; i < changed.length; ++i) {
            const label = root.fieldAt(changed[i].row || changed[i].key).rowLabel
            if (labels.indexOf(label) < 0) {
                labels.push(label)
            }
        }
        return labels
    }

    // The most rows the summary names; past that, the first of them and
    // how many more ("Title, Track, Disc and 2 more").
    readonly property int summaryNamedRows: 4

    // "8 files selected. Genre changed from Classical.", or, for more than
    // one field, "12 files selected. Changed: Title, Track, Disc.": which
    // fields to check before Save ("5 fields changed." named none of them,
    // and some were out of view in the table).
    function summaryText() {
        const count = root.selectedCount > 0 ? root.selectedCount : root.totalRows
        let text = root.selectedCount > 0
            ? I18n.i18ncp("@info:status", "%1 file selected.", "%1 files selected.", count)
            : I18n.i18ncp("@info:status", "%1 file loaded.", "%1 files loaded.", count)
        const changed = root.changedFieldLabels()
        if (changed.length === 1) {
            const key = changed[0].key
            const original = root.tagEditorApi.bulkOriginalValueMixed(key)
                ? "" : root.tagEditorApi.bulkOriginalValue(key)
            text += " " + (original.length > 0
                ? I18n.i18nc("@info:status tag field, value it had", "%1 changed from %2.", changed[0].label, original)
                : I18n.i18nc("@info:status tag field", "%1 changed.", changed[0].label))
        } else if (changed.length > 1) {
            const rows = root.changedRowLabels(changed)
            const separator = I18n.i18nc("@info:status separator between the tag fields of a list", ", ")
            if (rows.length <= root.summaryNamedRows) {
                text += " " + I18n.i18nc("@info:status %1 is a list of tag fields", "Changed: %1.",
                    rows.join(separator))
            } else {
                const named = root.summaryNamedRows - 1
                text += " " + I18n.i18ncp("@info:status %2 is a list of tag fields, %1 how many more fields changed",
                    "Changed: %2 and %1 more.", "Changed: %2 and %1 more.",
                    rows.length - named, rows.slice(0, named).join(separator))
            }
        }
        return text
    }

    function statusLine() {
        if (root.tagEditorApi.loading) {
            return I18n.i18nc("@info:status", "Loading tags…")
        }
        if (root.operationInFlight || root.tagEditorApi.saving) {
            return root.operationText
        }
        if (root.tagEditorApi.statusText.length > 0 && (root.statusHasFailure || tagEditorStatusFlash.visible)) {
            return root.tagEditorApi.statusText
        }
        return root.summaryText()
    }

    // The tooltip of a field's revert button: "Revert to Classical".
    function revertToolTip(key) {
        root.summaryRevision
        const original = root.tagEditorApi.bulkOriginalValueMixed(key) ? "" : root.tagEditorApi.bulkOriginalValue(key)
        return original.length > 0
            ? I18n.i18nc("@info:tooltip value the field had", "Revert to %1", original)
            : I18n.i18nc("@info:tooltip", "Revert")
    }

    title: I18n.i18nc("@title:window", "Edit Tags")
    asksBeforeClosing: true
    onCloseRequested: root.requestClose()

    readonly property real contentSpacing: Kirigami.Units.gridUnit - 2
    // The table's width up to the end of a column: its frame, its insets
    // and the columns before it.
    function tableWidthThrough(key) {
        let width = 2 + 2 * root.tableInset
        for (let i = 0; i < root.tableColumns.length; ++i) {
            width += root.tableColumnWidths[i]
            if (root.tableColumns[i].key === key) {
                break
            }
        }
        return width
    }
    readonly property real fieldsWidth: Math.max(Kirigami.Units.gridUnit * 19, root.fieldsMinimumWidth)
    // The fields' narrowest: 15 grid units, or what their labels and a
    // field need (at 16, the table scrolled on a 1400 px screen once the
    // track numbers' column read "Track").
    readonly property real fieldsLeastWidth: Math.max(Kirigami.Units.gridUnit * 15, root.fieldsMinimumWidth)
    // The fields' width as shown: narrower, down to their least, where
    // that gives Title and File Name what they want (from a 1600 px main
    // window, the batch's longest file name was cut by 47 px beside titles
    // of two lines), or else spares the table its sideways scroll bar with
    // every column at its least width or wider, as on a 1400 px screen;
    // otherwise their width.
    readonly property real fieldsShownWidth: {
        const rest = root.width - 2 * root.contentPadding - root.contentSpacing - 2 - 2 * root.tableInset
        let wanted = 0
        root.tableColumns.forEach(function(column, i) {
            wanted += column.key === "directory" ? root.tableColumnBaseWidths[i] : root.tableColumnWantWidths[i]
        })
        const least = root.tableColumnLeastWidths.reduce(function(sum, width) { return sum + width }, 0)
        for (const table of [wanted, least]) {
            const spare = Math.floor(rest - table)
            if (spare < root.fieldsWidth && spare >= root.fieldsLeastWidth) {
                return spare
            }
        }
        return root.fieldsWidth
    }
    // What the window holds, across: the fields and every column of the
    // table at its width, clear of its scroll bar.
    readonly property real fittedWidth: Math.ceil(2 * root.contentPadding + root.fieldsWidth
        + root.contentSpacing + root.tableWidthThrough("directory") + Kirigami.Units.gridUnit)
    // As wide as what it holds and no wider, within the screen's room: from
    // a narrow main window too (no wider than a 1000 px one, its table
    // showed three of its columns, the changed Disc out of view, on a
    // screen with room to spare). Only a window that falls short of it by
    // less than 2 grid units is a width to keep, frame and all, as the two
    // have the same frame (not 1624 px from a 1600 px main window). With
    // less room, File Name and Folder give way first, then the tags.
    readonly property real parentWindowWidth: root.windowRoot && root.windowRoot.width > 0
        ? root.windowRoot.width : 0
    defaultWidth: root.parentWindowWidth > 0 && root.fittedWidth > root.parentWindowWidth
            && root.fittedWidth - root.parentWindowWidth < 2 * Kirigami.Units.gridUnit
        ? root.parentWindowWidth : root.fittedWidth
    // As tall as the fields, or a table of 12 rows, whichever is taller,
    // and no taller than 46 grid units, scaled with the font: a few files
    // leave no wide empty table, and the user may make it taller.
    defaultHeight: Math.ceil(Math.min(root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 46), Math.max(fieldsColumn.implicitHeight,
            2 + root.tableRowHeight * 13 + Kirigami.Units.gridUnit))
        + 2 * root.contentPadding + tagEditorFooter.implicitHeight)
    // The fields at their narrowest beside some of the table, and room for
    // a few of the fields above the buttons.
    minimumWidth: Math.ceil(2 * root.contentPadding + root.fieldsLeastWidth
        + root.contentSpacing + Kirigami.Units.gridUnit * 16)
    minimumHeight: Math.ceil(2 * root.contentPadding + Kirigami.Units.gridUnit * 12 + tagEditorFooter.implicitHeight)

    // Sent when the edits that held up quitting are saved or dropped.
    signal quitMayProceed()

    objectName: "tagEditorDialog"

    function closeEditor() {
        // The results and errors of a save show here, so the editor stays
        // open until the save has finished.
        if (root.tagEditorApi.saving) {
            return
        }
        root.tagEditorApi.close()
        root.reject()
    }

    // The field whose typed text differs from what it holds and is not
    // yet applied to the tracks, or null. A field applies its text as it
    // loses focus or on Enter; until then the text is an unsaved change
    // too, which Save and Ctrl+S apply and save (rather than Save staying
    // disabled while the user types, and Ctrl+S doing nothing).
    property Item pendingEditField: null
    readonly property bool hasUnsavedChanges: root.tagEditorApi.dirty || root.pendingEditField !== null
    // The typed text marks its change at once, as it will once applied
    // (rather than nothing showing it until the field loses focus): its
    // field's label and Revert, its column's header, and the rows it
    // goes to.
    readonly property string pendingEditKey: root.pendingEditField ? root.pendingEditField.field.key : ""
    // And those rows' cells show it, as their marks say they changed (the
    // cell kept the old value until Tab): shown only, the tracks have it
    // once it is applied.
    readonly property string pendingEditText: root.pendingEditField ? root.pendingEditField.text : ""
    readonly property var pendingEditRows: {
        const rows = ({})
        const snapshot = root.pendingEditField ? root.pendingEditField.editRowsSnapshot : []
        for (let i = 0; i < snapshot.length; ++i) {
            rows[snapshot[i]] = true
        }
        return rows
    }
    // Whether the edits change a field in the selected rows, or in any
    // row, typed text included.
    function bulkFieldMarked(key) {
        return root.pendingEditKey === key
            || (root.summaryRevision >= 0 && root.tagEditorApi.bulkFieldChanged(key))
    }
    function columnMarked(key) {
        return root.pendingEditKey === key
            || (root.summaryRevision >= 0 && root.tagEditorApi.fieldChanged(key))
    }
    readonly property bool canSave: !root.tagEditorApi.loading && !root.tagEditorApi.saving
        && root.hasUnsavedChanges

    // A field commits its typed text when it loses focus.
    function commitPendingEditorEdits() {
        if (root.visible) {
            tagEditorTable.forceActiveFocus()
        }
    }

    function triggerSave(closeAfterSuccess) {
        if (!root.canSave) {
            return false
        }
        // The typed text first: it may be the only change, or none after
        // all.
        root.commitPendingEditorEdits()
        if (!root.tagEditorApi.dirty) {
            return false
        }
        root.operationInFlight = true
        root.operationText = I18n.i18nc("@info:status", "Saving tags…")
        root.closeAfterSave = closeAfterSuccess
        if (!root.tagEditorApi.save()) {
            root.operationInFlight = false
            root.operationText = ""
            root.closeAfterSave = false
            return false
        }
        return true
    }

    // Rename Files shows the new names first; renameNow renames once the
    // user agrees.
    function triggerRename() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving || root.selectedCount === 0) return
        root.commitPendingEditorEdits()
        root.operationInFlight = true
        root.operationText = I18n.i18nc("@info:status", "Checking new file names…")
        root.renamePreviewPending = root.tagEditorApi.previewRenameSelectedFiles()
        if (!root.renamePreviewPending) {
            root.operationInFlight = false
            root.operationText = ""
        }
    }

    // Renames what the preview showed, even if the selection changed since.
    function renameNow() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving) return
        root.operationInFlight = true
        root.operationText = I18n.i18nc("@info:status", "Renaming files…")
        if (!root.tagEditorApi.renamePreviewedFiles()) {
            root.operationInFlight = false
            root.operationText = ""
        }
    }

    // The close button, Cancel, and Escape all close through here.
    function requestClose() {
        if (root.tagEditorApi.saving) {
            return
        }
        root.commitPendingEditorEdits()
        if (root.tagEditorApi.dirty) {
            tagEditorCloseConfirmDialog.purpose = "close"
            tagEditorCloseConfirmDialog.open()
            return
        }
        root.closeEditor()
    }

    // Reloading drops unsaved edits, so it asks first, as closing does.
    function requestReload() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving) {
            return
        }
        root.commitPendingEditorEdits()
        if (root.tagEditorApi.dirty) {
            tagEditorCloseConfirmDialog.purpose = "reload"
            tagEditorCloseConfirmDialog.open()
            return
        }
        root.reloadNow()
    }

    function reloadNow() {
        root.clearSelection()
        root.tagEditorApi.reload()
        root.initializeSelection()
    }

    function saveAndClose() {
        if (!root.triggerSave(true)) {
            tagEditorCloseConfirmDialog.dismiss()
            root.continueQuit(!root.tagEditorApi.dirty)
            return
        }
        if (root.quitPending) {
            // Quitting waits for this save within the same bound as for any
            // other, so the confirmation gives way to the wait.
            tagEditorCloseConfirmDialog.dismiss()
            root.waitForOperationToQuit()
        }
    }

    // Whether Ferrous may quit now. When edits are unsaved, this asks whether
    // to save them, and when a save is running, it waits for it; either way
    // it sends quitMayProceed once quitting may go on.
    function prepareToQuit() {
        if (root.tagEditorApi.saving) {
            if (root.quitWithoutOperation) {
                return true
            }
            root.waitForOperationToQuit()
            return false
        }
        root.commitPendingEditorEdits()
        if (root.tagEditorApi.dirty) {
            root.quitPending = true
            if (!root.visible) {
                root.open()
            }
            tagEditorCloseConfirmDialog.purpose = "quit"
            tagEditorCloseConfirmDialog.open()
            return false
        }
        root.quitPending = false
        return true
    }

    // The one way quitting waits for a running save or rename: it shows the
    // files being written, and after the bound offers to quit without them.
    function waitForOperationToQuit() {
        root.quitPending = true
        // A repeated quit request neither stacks dialogs nor postpones the
        // offer to quit anyway.
        if (!tagEditorQuitWaitDialog.visible) {
            tagEditorQuitWaitDialog.offerChoice = false
            tagEditorQuitWaitDialog.open()
            tagEditorQuitWaitTimeout.restart()
        }
    }

    // The save or rename that quitting waited for has finished.
    function stopWaitingForOperation() {
        tagEditorQuitWaitTimeout.stop()
        tagEditorQuitWaitDialog.dismiss()
        root.quitWithoutOperation = false
    }

    function continueQuit(succeeded) {
        if (!root.quitPending) {
            return
        }
        root.quitPending = false
        // A failure stays on screen, and the user decides again.
        if (succeeded && root.prepareToQuit()) {
            root.quitMayProceed()
        }
    }

    function selectOnlyRow(row) {
        root.tagEditorApi.setSelectedRows([row])
        root.selectionAnchor = row
        tagEditorTable.currentIndex = row
    }

    function handleRowClick(row, modifiers) {
        // The row takes the keyboard from a column's Revert, which would
        // keep it, and Enter, as the focus scope's focused item.
        const focused = root.activeFocusItem
        if (focused && focused.inTableHeader === true) {
            focused.focus = false
        }
        tagEditorTable.forceActiveFocus()
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        if (shift && root.selectionAnchor >= 0) {
            root.tagEditorApi.selectRange(root.selectionAnchor, row)
            tagEditorTable.currentIndex = row
            return
        }
        if (ctrl) {
            root.tagEditorApi.toggleRowSelected(row)
            root.selectionAnchor = row
            tagEditorTable.currentIndex = row
            return
        }
        root.selectOnlyRow(row)
    }

    // Moves the current row by keyboard; with Shift, the selection extends
    // from the anchor to it.
    function moveCurrentRow(row, extend) {
        if (root.totalRows === 0) {
            return
        }
        const target = Math.max(0, Math.min(root.totalRows - 1, row))
        if (extend && root.selectionAnchor >= 0) {
            root.tagEditorApi.selectRange(root.selectionAnchor, target)
            tagEditorTable.currentIndex = target
        } else {
            root.selectOnlyRow(target)
        }
        tagEditorTable.positionViewAtIndex(target, ListView.Contain)
    }

    // A page of the table: the rows in view under its header, as many as
    // their heights leave, less the one kept in view.
    function rowsPerPage() {
        const table = tagEditorTable
        const first = table.indexAt(1, table.contentY + root.tableRowHeight + 1)
        const last = table.indexAt(1, table.contentY + table.height - 1)
        return first >= 0 && last >= 0
            ? Math.max(1, last - first - 1)
            : Math.max(1, Math.floor(table.height / root.tableRowHeight) - 2)
    }

    function handleTableKey(event) {
        const current = Math.max(0, tagEditorTable.currentIndex)
        const page = root.rowsPerPage()
        const extend = (event.modifiers & Qt.ShiftModifier) !== 0
        switch (event.key) {
        case Qt.Key_Up: root.moveCurrentRow(tagEditorTable.currentIndex < 0 ? 0 : current - 1, extend); break
        case Qt.Key_Down: root.moveCurrentRow(tagEditorTable.currentIndex < 0 ? 0 : current + 1, extend); break
        case Qt.Key_PageUp: root.moveCurrentRow(current - page, extend); break
        case Qt.Key_PageDown: root.moveCurrentRow(current + page, extend); break
        case Qt.Key_Home: root.moveCurrentRow(0, extend); break
        case Qt.Key_End: root.moveCurrentRow(root.totalRows - 1, extend); break
        case Qt.Key_Space:
            if (tagEditorTable.currentIndex < 0) {
                return
            }
            root.tagEditorApi.toggleRowSelected(tagEditorTable.currentIndex)
            root.selectionAnchor = tagEditorTable.currentIndex
            break
        default:
            return
        }
        event.accepted = true
    }

    // The current row follows the selection, however it changes: when the
    // current row is not selected, the first selected row becomes current
    // and the anchor, so the ring and the tint are on one row and the
    // keyboard goes on from the selection (otherwise, after the selection
    // was set elsewhere, the ring stayed on row 1 while row 2 was selected). A
    // row that Ctrl or Space has just deselected stays current: it is set
    // after the selection changes.
    function followSelection() {
        const rows = root.tagEditorApi.selectedRows()
        if (rows.length === 0 || rows.indexOf(tagEditorTable.currentIndex) >= 0) {
            return
        }
        // The controller's selection is a set, in no order.
        const first = Math.min.apply(null, rows.map(Number))
        tagEditorTable.currentIndex = first
        if (rows.indexOf(root.selectionAnchor) < 0) {
            root.selectionAnchor = first
        }
    }

    function initializeSelection() {
        if (root.totalRows > 0 && root.selectedCount === 0) {
            root.selectOnlyRow(0)
        }
    }

    function clearSelection() {
        root.tagEditorApi.setSelectedRows([])
        root.selectionAnchor = -1
    }

    function selectAllRows() {
        root.tagEditorApi.selectAllRows()
        root.selectionAnchor = root.totalRows > 0 ? 0 : -1
    }

    function cellText(columnKey, row) {
        switch (columnKey) {
        case "directory": return FormatUtils.basenameFromPath(row.directory || "")
        case "title":
        case "artist":
        case "album":
        case "genre": {
            // Show every value of a field that holds several.
            const extra = row.extraValues ? row.extraValues[columnKey] : undefined
            const value = row[columnKey] || ""
            return extra && extra.length > 0 ? [value].concat(extra).join("; ") : value
        }
        case "status":
            if (row.errorText.length > 0) {
                return row.renameFailed ? root.notRenamedText : root.notSavedText
            }
            return row.dirty ? root.modifiedText : ""
        case "trackNo":
        case "discNo":
            // As the queue shows a number: "1", whatever the tag pads it to.
            return String(row[columnKey] || "").replace(/^0+(?=\d)/, "")
        default:
            return row[columnKey] || ""
        }
    }

    onAboutToShow: {
        root.initializeSelection()
        tagEditorTable.forceActiveFocus()
    }
    onClosed: {
        root.clearSelection()
        root.renamePreviewPending = false
        root.operationInFlight = false
        root.operationText = ""
        tagEditorStatusFlash.stop()
        tagEditorStatusFlash.visible = false
    }

    Connections {
        target: root.tagEditorApi
        ignoreUnknownSignals: true

        function onSelectionChanged() {
            root.followSelection()
        }

        function onOpenChanged() {
            if (root.tagEditorApi.open && !root.visible) {
                root.open()
            } else if (!root.tagEditorApi.open && root.visible) {
                root.reject()
            }
        }

        function onLoadingChanged() {
            if (!root.tagEditorApi.loading) {
                root.measuredRows = root.tagEditorApi.leadingRows(root.measuredRowLimit)
            }
            if (root.tagEditorApi.loading && root.renamePreviewPending) {
                root.renamePreviewPending = false
                root.operationInFlight = false
                root.operationText = ""
            }
            if (!root.tagEditorApi.loading) root.initializeSelection()
        }
        function onSaveFinished(success) {
            root.stopWaitingForOperation()
            root.operationInFlight = false
            root.operationText = ""
            // A failure shows in the editor, under the confirmation.
            tagEditorCloseConfirmDialog.dismiss()
            if (success && root.closeAfterSave) {
                root.closeEditor()
            }
            root.closeAfterSave = false
            tagEditorStatusFlash.restart()
            root.continueQuit(success)
        }
        function onRenamePreviewReady(rows, error) {
            // A closed editor has nothing to rename.
            if (!root.renamePreviewPending || !root.visible) {
                return
            }
            root.renamePreviewPending = false
            root.operationInFlight = false
            root.operationText = ""
            renamePreviewDialog.rows = rows
            renamePreviewDialog.errorText = error
            renamePreviewDialog.open()
        }
        function onRenameFinished() {
            root.stopWaitingForOperation()
            root.operationInFlight = false
            root.operationText = ""
            tagEditorStatusFlash.restart()
            root.continueQuit(!root.tagEditorApi.statusIsError)
        }
        function onStatusChanged() {
            if (!root.tagEditorApi.statusText.length) {
                tagEditorStatusFlash.stop()
                tagEditorStatusFlash.visible = false
                return
            }
            tagEditorStatusFlash.restart()
        }
    }

    // A result shows in the status line for a moment, then the selection's
    // summary again; a failure stays.
    Timer {
        id: tagEditorStatusFlash
        interval: 2200
        repeat: false
        property bool visible: false
        onRunningChanged: {
            if (running) {
                visible = true
            }
        }
        onTriggered: visible = false
    }

    Connections {
        target: root.tagEditorApi
        ignoreUnknownSignals: true
        function onBulkSummaryChanged() { root.summaryRevision++ }
        function onSelectionChanged() { root.summaryRevision++ }
    }

    Dialogs.AutoNumberDialog {
        id: autoNumberDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root
    }

    Dialogs.RenamePreviewDialog {
        id: renamePreviewDialog
        uiPalette: root.uiPalette
        windowRoot: root
        onRenameRequested: root.renameNow()
        onClosed: rows = []
    }

    Dialogs.TagEditorCloseConfirmDialog {
        id: tagEditorCloseConfirmDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root
        onSaveRequested: root.saveAndClose()
        onDiscardRequested: {
            if (tagEditorCloseConfirmDialog.forReload) {
                root.reloadNow()
                return
            }
            root.closeEditor()
            root.continueQuit(true)
        }
        onKeepEditingRequested: root.quitPending = false
    }

    Dialogs.TagEditorQuitWaitDialog {
        id: tagEditorQuitWaitDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root
        onKeepWaitingRequested: {
            tagEditorQuitWaitDialog.offerChoice = false
            tagEditorQuitWaitTimeout.restart()
        }
        onQuitAnywayRequested: {
            tagEditorQuitWaitTimeout.stop()
            tagEditorQuitWaitDialog.dismiss()
            root.quitPending = false
            root.quitWithoutOperation = true
            root.quitMayProceed()
        }
        onStopQuittingRequested: {
            tagEditorQuitWaitTimeout.stop()
            root.quitPending = false
        }
    }

    Timer {
        id: tagEditorQuitWaitTimeout
        objectName: "tagEditorQuitWaitTimeout"
        interval: root.quitWaitBoundMs
        onTriggered: tagEditorQuitWaitDialog.offerChoice = true
    }

    Dialogs.TagEditorStatusDetailsDialog {
        id: tagEditorStatusDetailsDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root
    }

    // A Revert's tooltip, above its glyph with their right edges in line:
    // under it, it covered the next field, or the first row. A window of
    // its own, as over the top field it rises past the dialog's edge. On
    // Wayland the compositor places such a window from its parent alone,
    // at that item's bottom right corner, and not from x and y, so its
    // parent is a point where its top left corner goes (see AppMenu.qml).
    component RevertToolTip: Item {
        id: toolTipPoint

        required property Item button
        property alias message: revertToolTip.message

        x: toolTipPoint.button.width - Math.ceil(revertToolTip.implicitWidth) - width
        y: -Kirigami.Units.smallSpacing - Math.ceil(revertToolTip.implicitHeight) - height
        width: 1
        height: 1

        Components.PlainToolTip {
            id: revertToolTip
            objectName: "tagEditorRevertToolTip"
            popupType: Popup.Window
            x: toolTipPoint.width
            y: toolTipPoint.height
            delay: Kirigami.Units.toolTipDelay
            visible: toolTipPoint.button.visible
                && (toolTipPoint.button.hovered || toolTipPoint.button.visualFocus)
            // The keyboard stays in the dialog. Qt makes a popup window's
            // popup that window's focused item as it shows, and one that
            // still is as it goes hands the keyboard back to what had it
            // then: Tab or a click away from a Revert came back to it as
            // its tooltip had faded. The tooltip's label is that item
            // instead, so there is nothing to hand back.
            onAboutToShow: revertToolTip.contentItem.focus = true
        }
    }

    // A text field of a tag: its label turns semibold and a revert button
    // shows while the edits change it.
    component TagField: TextField {
        id: fieldEditor

        required property var field
        // Its files hold different values, which saving keeps.
        property bool mixed: false
        property bool touched: false
        // The text it shows for the tracks, before the user types.
        property string committedText: ""
        // Typed and not yet applied: see the dialog's pendingEditField.
        readonly property bool pendingEdit: fieldEditor.touched && fieldEditor.text !== fieldEditor.committedText
        onPendingEditChanged: {
            if (fieldEditor.pendingEdit) {
                root.pendingEditField = fieldEditor
            } else if (root.pendingEditField === fieldEditor) {
                root.pendingEditField = null
            }
        }
        Component.onDestruction: {
            if (root.pendingEditField === fieldEditor) {
                root.pendingEditField = null
            }
        }
        property var editRowsSnapshot: []
        readonly property bool changed: root.bulkFieldMarked(fieldEditor.field.key)
        readonly property Item revert: revertButton
        KeyNavigation.tab: root.tabStopFrom(fieldEditor, 1)
        KeyNavigation.backtab: root.tabStopFrom(fieldEditor, -1)

        function captureEditRowsSnapshot() {
            editRowsSnapshot = root.tagEditorApi.selectedRows()
        }

        function syncFromController() {
            const value = root.tagEditorApi.bulkValue(fieldEditor.field.key)
            const mixed = root.tagEditorApi.bulkValueMixed(fieldEditor.field.key)
            root.setExtraValuesNote(fieldEditor.field.key,
                root.tagEditorApi.extraValuesNote(fieldEditor.field.key))
            if (activeFocus && touched) {
                return
            }
            text = mixed ? "" : value
            committedText = text
            fieldEditor.mixed = mixed
            editRowsSnapshot = []
            touched = false
        }

        objectName: "tagEditorField_" + fieldEditor.field.key
        Layout.fillWidth: true
        // Breeze outlines a field under the pointer as it does the focused
        // one: while a field here has the keyboard, only that one is
        // outlined (after a click, typing and Tab, two looked focused).
        hoverEnabled: !root.formFieldFocused && !Kirigami.Settings.tabletMode
        selectByMouse: true
        persistentSelection: true
        placeholderText: fieldEditor.mixed ? root.keepText : ""
        placeholderTextColor: root.uiPalette.placeholderText
        rightPadding: fieldEditor.changed
            ? revertButton.width + Kirigami.Units.smallSpacing
            : leftPadding
        Accessible.name: fieldEditor.field.label
        Accessible.description: fieldEditor.changed ? I18n.i18nc("@info", "Changed") : ""

        onActiveFocusChanged: {
            if (activeFocus && !touched) {
                captureEditRowsSnapshot()
            }
        }
        onTextEdited: {
            if (!touched) {
                captureEditRowsSnapshot()
            }
            touched = true
        }
        onEditingFinished: {
            if (!touched) {
                return
            }
            root.tagEditorApi.applyBulkFieldToRows(
                editRowsSnapshot,
                fieldEditor.field.key,
                text)
            editRowsSnapshot = []
            touched = false
            syncFromController()
        }

        Component.onCompleted: syncFromController()

        Connections {
            target: root.tagEditorApi
            ignoreUnknownSignals: true
            function onSelectionChanged() { fieldEditor.syncFromController() }
            function onBulkSummaryChanged() { fieldEditor.syncFromController() }
        }

        ToolButton {
            id: revertButton
            objectName: "tagEditorRevert_" + fieldEditor.field.key
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            width: root.fieldRevertWidth
            height: width
            KeyNavigation.tab: root.tabStopFrom(revertButton, 1)
            KeyNavigation.backtab: root.tabStopFrom(revertButton, -1)
            // Enter presses it, as Space does, rather than Save.
            Keys.onPressed: function(event) {
                if (KeyUtils.isEnter(event)) {
                    event.accepted = true
                    revertButton.clicked()
                }
            }
            visible: fieldEditor.changed
            enabled: !root.busy
            icon.name: "edit-undo"
            icon.width: root.uiPalette.labelIconSize
            icon.height: root.uiPalette.labelIconSize
            display: AbstractButton.IconOnly
            text: I18n.i18nc("@action:button %1 is a tag field", "Revert %1", fieldEditor.field.label)
            onClicked: {
                fieldEditor.touched = false
                // It hides as it reverts: the keyboard goes to its field.
                if (revertButton.activeFocus) {
                    fieldEditor.forceActiveFocus(Qt.OtherFocusReason)
                }
                root.tagEditorApi.revertBulkField(fieldEditor.field.key)
                fieldEditor.syncFromController()
            }

            RevertToolTip {
                button: revertButton
                message: root.revertToolTip(fieldEditor.field.key)
            }
        }
    }

    // A case change of one field, in the Tools menu.
    component CaseItem: MenuItem {
        required property string fieldKey
        required property bool titleCase
        objectName: (titleCase ? "tagEditorTitleCase_" : "tagEditorCapitalCase_") + fieldKey
        text: root.fieldAt(fieldKey).label
        onClicked: root.changeCase(fieldKey, titleCase)
    }

    // The label of a field: semibold while the edits change it.
    component FieldLabel: Label {
        required property var field
        readonly property bool changed: root.bulkFieldMarked(field.key)
        objectName: "tagEditorLabel_" + field.key
        Layout.preferredWidth: root.fieldLabelWidth
        horizontalAlignment: Text.AlignRight
        textFormat: Text.PlainText
        text: field.formLabel
        font.weight: changed ? Font.DemiBold : Font.Normal
        color: root.uiPalette.text
        Accessible.ignored: true
    }

    // The notes under the fields of rows with several values, by field.
    property var extraValuesNotes: ({})

    function setExtraValuesNote(key, note) {
        if ((root.extraValuesNotes[key] || "") === note) {
            return
        }
        const notes = Object.assign({}, root.extraValuesNotes)
        notes[key] = note
        root.extraValuesNotes = notes
    }

    function fieldAt(key) {
        for (let i = 0; i < root.editorFields.length; ++i) {
            if (root.editorFields[i].key === key) {
                return root.editorFields[i]
            }
        }
        return null
    }

    // A case change of `key` in the edited rows.
    function changeCase(key, titleCase) {
        root.commitPendingEditorEdits()
        if (titleCase) {
            root.tagEditorApi.applyEnglishTitleCase(key)
        } else if (key === "genre") {
            root.tagEditorApi.applyGenreCapitalize()
        } else {
            root.tagEditorApi.applyFinnishCapitalize(key)
        }
    }

    // Tools, Reload and the status share the row with the buttons, on
    // their inset from the window's edges: a failure, Details… straight
    // after it, which explains it (it stood beside Save, 975 px from it in
    // a 1600 px window), then the room, and Save and Cancel at the end,
    // where they stay.
    footer: RowLayout {
        id: tagEditorFooter
        spacing: Kirigami.Units.largeSpacing

        Button {
            id: toolsButton
            objectName: "tagEditorToolsButton"
            Layout.leftMargin: dialogButtons.inset
            Layout.bottomMargin: dialogButtons.bottomPadding
            text: I18n.i18nc("@action:button", "Tools")
            // A glyph, at Reload's size, as the footer's other actions
            // have: without one, beside its arrow, it read as a combo box.
            icon.name: "tools-wizard"
            icon.width: dialogButtons.glyphSize
            icon.height: dialogButtons.glyphSize
            enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
            // A menu button, which Breeze draws with its arrow.
            Accessible.role: Accessible.ButtonMenu
            onClicked: toolsMenu.open()

            Components.AppMenu {
                id: toolsMenu
                objectName: "tagEditorToolsMenu"
                y: toolsButton.height

                MenuItem {
                    objectName: "tagEditorAutoNumberButton"
                    text: I18n.i18nc("@action:inmenu", "Auto Number…")
                    icon.name: "format-list-ordered"
                    enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
                    onClicked: {
                        root.commitPendingEditorEdits()
                        autoNumberDialog.open()
                    }
                }
                MenuItem {
                    objectName: "tagEditorRenameButton"
                    text: I18n.i18nc("@action:inmenu", "Rename Files…")
                    icon.name: "edit-rename"
                    enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.selectedCount > 0
                    onClicked: root.triggerRename()
                }
                MenuSeparator {}
                Components.AppMenu {
                    title: I18n.i18nc("@title:menu", "Title Case")

                    CaseItem { fieldKey: "title"; titleCase: true }
                    CaseItem { fieldKey: "artist"; titleCase: true }
                    CaseItem { fieldKey: "album"; titleCase: true }
                    CaseItem { fieldKey: "albumArtist"; titleCase: true }
                }
                Components.AppMenu {
                    title: I18n.i18nc("@title:menu", "Capital Case")

                    CaseItem { fieldKey: "title"; titleCase: false }
                    CaseItem { fieldKey: "artist"; titleCase: false }
                    CaseItem { fieldKey: "album"; titleCase: false }
                    CaseItem { fieldKey: "albumArtist"; titleCase: false }
                    CaseItem { fieldKey: "genre"; titleCase: false }
                }
                MenuSeparator {}
                MenuItem {
                    text: I18n.i18nc("@action:inmenu", "Select All")
                    icon.name: "edit-select-all"
                    enabled: root.totalRows > 0
                    onClicked: root.selectAllRows()
                }
                MenuItem {
                    objectName: "tagEditorClearSelectionItem"
                    text: I18n.i18nc("@action:inmenu", "Clear Selection")
                    icon.name: "edit-select-none"
                    enabled: root.selectedCount > 0
                    onClicked: root.clearSelection()
                }
            }
        }

        Button {
            objectName: "tagEditorReloadButton"
            Layout.bottomMargin: dialogButtons.bottomPadding
            icon.width: dialogButtons.glyphSize
            icon.height: dialogButtons.glyphSize
            text: I18n.i18nc("@action:button", "Reload")
            icon.name: "view-refresh"
            enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
            onClicked: root.requestReload()
        }

        BusyIndicator {
            running: root.busy
            visible: running
            Layout.bottomMargin: dialogButtons.bottomPadding
            Layout.preferredWidth: root.uiPalette.labelIconSize
            Layout.preferredHeight: root.uiPalette.labelIconSize
        }

        // The status, in a slot as tall as the buttons, whatever its
        // lines: the summary's second line, as it wraps to name the fields
        // an edit changed, stands over the row's edges evenly and moves
        // nothing (the buttons keep the dialogs' inset from the bottom).
        Item {
            Layout.fillWidth: true
            // As wide as its text, for what follows it, and elided in less.
            Layout.maximumWidth: Math.ceil(statusLabel.implicitWidth)
            Layout.preferredHeight: toolsButton.implicitHeight
            Layout.bottomMargin: dialogButtons.bottomPadding
            Layout.leftMargin: Kirigami.Units.smallSpacing

            Label {
                id: statusLabel
                objectName: "tagEditorStatusLabel"
                readonly property bool failure: root.statusHasFailure && !root.busy
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.PlainText
                text: root.statusLine()
                font.pointSize: statusLabel.failure ? root.uiPalette.basePointSize : root.uiPalette.smallPointSize
                color: statusLabel.failure ? root.uiPalette.negative : root.uiPalette.textSecondary
                // The summary takes a second line where one is too short
                // for the fields it names, and elides past that; a failure
                // keeps to one, beside its Details….
                wrapMode: statusLabel.failure ? Text.NoWrap : Text.Wrap
                maximumLineCount: statusLabel.failure ? 1 : 2
                elide: Text.ElideRight
                Accessible.role: Accessible.StatusBar

                HoverHandler {
                    id: statusHover
                }

                // All of it while it is cut, as a cell's.
                Components.PlainToolTip {
                    objectName: "tagEditorStatusToolTip"
                    message: statusLabel.text
                    delay: Kirigami.Units.toolTipDelay
                    visible: statusHover.hovered && statusLabel.truncated
                }
            }
        }

        Button {
            objectName: "tagEditorDetailsButton"
            Layout.bottomMargin: dialogButtons.bottomPadding
            visible: !root.busy && root.tagEditorApi.statusDetails.length > 0
            icon.width: dialogButtons.glyphSize
            icon.height: dialogButtons.glyphSize
            icon.name: "documentinfo"
            text: I18n.i18nc("@action:button", "Details…")
            onClicked: tagEditorStatusDetailsDialog.open()
        }

        Item {
            Layout.fillWidth: true
        }

        Components.DialogButtons {

            id: dialogButtons
            primary: tagEditorSaveButton
            Button {
                id: tagEditorSaveButton
                objectName: "tagEditorSaveButton"
                text: root.tagEditorApi.saving
                    ? I18n.i18nc("@action:button", "Saving…")
                    : I18n.i18nc("@action:button", "Save")
                icon.name: "document-save"
                enabled: root.canSave
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: root.triggerSave(false)
            }
            Button {
                objectName: "tagEditorCancelButton"
                text: I18n.i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                enabled: !root.tagEditorApi.saving
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: root.requestClose()
            }
        }
    }

    RowLayout {
        id: tagEditorContent
        anchors.fill: parent
        anchors.margins: root.contentPadding

        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, root.visible) }

        spacing: root.contentSpacing

        Shortcut {
            sequences: ["Ctrl+S"]
            context: Qt.WindowShortcut
            enabled: root.visible && root.canSave
            onActivated: root.triggerSave(false)
        }

        Shortcut {
            sequences: ["Ctrl+A"]
            context: Qt.WindowShortcut
            enabled: root.visible && root.totalRows > 0
            onActivated: root.selectAllRows()
        }

        ScrollView {
            id: tagEditorFieldsScroll
            objectName: "tagEditorFields"
            Layout.preferredWidth: root.fieldsShownWidth
            Layout.minimumWidth: root.fieldsLeastWidth
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: fieldsColumn
                width: tagEditorFieldsScroll.availableWidth
                spacing: Kirigami.Units.largeSpacing + 2

                FieldRow { id: titleRow; field: root.fieldAt("title") }
                FieldRow { id: artistRow; field: root.fieldAt("artist") }
                FieldRow { id: albumRow; field: root.fieldAt("album") }
                FieldRow { id: albumArtistRow; field: root.fieldAt("albumArtist") }
                FieldRow { id: genreRow; field: root.fieldAt("genre") }
                FieldRow { id: yearRow; field: root.fieldAt("year") }
                NumberPair { id: trackPair; first: root.fieldAt("trackNo"); second: root.fieldAt("totalTracks") }
                NumberPair { id: discPair; first: root.fieldAt("discNo"); second: root.fieldAt("totalDiscs") }
                FieldRow { id: commentRow; field: root.fieldAt("comment") }
            }
        }

        // The files, on the view colour, with the queue's header.
        Rectangle {
            id: tableFrame
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.uiPalette.view
            radius: 3
            border.width: 1
            border.color: Kirigami.ColorUtils.linearInterpolation(
                root.uiPalette.view, root.uiPalette.text, Kirigami.Theme.frameContrast)

            // Only the visible rows have items, so thousands of rows open at
            // once. Selection is a role, so each row reads it once.
            ListView {
                id: tagEditorTable
                objectName: "tagEditorTable"
                anchors.fill: parent
                anchors.margins: 1
                // The rows end over the horizontal bar, which would cover
                // the last line of a change that wraps. Whether it shows
                // follows from the widths alone.
                anchors.bottomMargin: 1 + (root.tableScrolls ? tagEditorAcrossBar.height : 0)
                clip: true
                focus: true
                activeFocusOnTab: true
                KeyNavigation.backtab: root.tabStopFrom(tagEditorTable, -1)
                model: root.tagEditorApi.tableModel
                currentIndex: -1
                keyNavigationEnabled: false
                highlightFollowsCurrentItem: false
                contentWidth: root.tableContentWidth
                flickableDirection: Flickable.AutoFlickIfNeeded
                boundsBehavior: Flickable.StopAtBounds
                headerPositioning: ListView.OverlayHeader
                pixelAligned: true
                // From under the header to the rows' foot, as the
                // Configure Keyboard Shortcuts list's bar runs: on the
                // view, it ran through the header, its thumb level with
                // the column names.
                ScrollBar.vertical: ScrollBar {
                    id: tagEditorDownBar
                    parent: tableFrame
                    anchors.right: tagEditorTable.right
                    anchors.top: tagEditorTable.top
                    anchors.topMargin: root.tableRowHeight
                    anchors.bottom: tagEditorTable.bottom
                }
                ScrollBar.horizontal: ScrollBar {
                    id: tagEditorAcrossBar
                    parent: tableFrame
                    anchors.left: tagEditorTable.left
                    anchors.right: tagEditorTable.right
                    anchors.top: tagEditorTable.bottom
                }
                Accessible.role: Accessible.Table
                Accessible.name: I18n.i18nc("@label", "Files")

                Keys.onPressed: function(event) { root.handleTableKey(event) }

                header: Rectangle {
                    objectName: "tagEditorTableHeader"
                    z: 2
                    width: root.tableContentWidth
                    height: root.tableRowHeight
                    color: root.uiPalette.view

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: root.tableInset
                        anchors.rightMargin: root.tableInset
                        anchors.bottomMargin: 1

                        Repeater {
                            model: root.tableColumns
                            delegate: Item {
                                id: headerCell
                                required property var modelData
                                required property int index
                                readonly property bool editable: root.fieldAt(headerCell.modelData.key) !== null
                                readonly property bool changed: headerCell.editable
                                    && root.columnMarked(headerCell.modelData.key)
                                objectName: "tagEditorColumn_" + headerCell.modelData.key
                                width: root.columnWidth(headerCell.index)
                                height: parent.height
                                // Status, while no row has one.
                                visible: width > 0
                                readonly property bool pinned: headerCell.index < root.pinnedColumnCount
                                z: headerCell.pinned ? 2 : 0
                                transform: Translate { x: headerCell.pinned ? root.pinnedShift : 0 }

                                // Under the pinned headers, from the table's
                                // edge to the seam, the header's colour, over
                                // those that scroll.
                                Rectangle {
                                    visible: headerCell.index === 0 && root.pinsColumns
                                    z: -1
                                    x: -root.tableInset
                                    width: root.pinnedWidth
                                    height: parent.height
                                    color: root.uiPalette.view

                                    Rectangle {
                                        objectName: "tagEditorHeaderSeam"
                                        anchors.right: parent.right
                                        width: 1
                                        height: parent.height
                                        color: root.uiPalette.seam
                                    }
                                }

                                Row {
                                    anchors.fill: parent
                                    // The Revert's end has no padding: see
                                    // columnNeed.
                                    anchors.leftMargin: headerCell.modelData.number
                                        ? root.numberLeadingRoom : Kirigami.Units.largeSpacing
                                    anchors.rightMargin: headerCell.editable ? 0 : Kirigami.Units.largeSpacing
                                    spacing: Kirigami.Units.smallSpacing

                                    Label {
                                        objectName: "tagEditorColumnLabel"
                                        anchors.verticalCenter: parent.verticalCenter
                                        // A number's label ends at the
                                        // Revert's room, over its digits.
                                        width: headerCell.modelData.number
                                            ? parent.width - root.columnRevertRoom
                                            : Math.min(implicitWidth, parent.width
                                                - (columnRevert.visible ? columnRevert.width + parent.spacing : 0))
                                        horizontalAlignment: headerCell.modelData.number ? Text.AlignRight : Text.AlignLeft
                                        textFormat: Text.PlainText
                                        text: headerCell.modelData.label
                                        font.pointSize: root.uiPalette.smallPointSize
                                        font.weight: headerCell.changed ? Font.DemiBold : Font.Normal
                                        color: headerCell.changed ? root.uiPalette.text : root.uiPalette.textSecondary
                                        elide: Text.ElideRight
                                    }

                                    ToolButton {
                                        id: columnRevert
                                        objectName: "tagEditorColumnRevert_" + headerCell.modelData.key
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: headerCell.changed
                                        enabled: !root.busy
                                        readonly property bool inTableHeader: true
                                        // It is the table's focused item only
                                        // while it has the keyboard: the table,
                                        // a focus scope, gave it back to it as
                                        // Tab or a click came to the files.
                                        // Another window's turn keeps it,
                                        // when no item here has the keyboard
                                        // (`active` holds for the window of
                                        // a dialog over this one).
                                        onActiveFocusChanged: {
                                            if (columnRevert.activeFocus) {
                                                root.showInTable(headerCell)
                                            } else if (root.activeFocusItem !== null) {
                                                columnRevert.focus = false
                                            }
                                        }
                                        onVisibleChanged: {
                                            if (!columnRevert.visible) {
                                                columnRevert.focus = false
                                            }
                                        }
                                        // Shift+Tab from the first goes back to
                                        // the files, which Tab came from: the
                                        // window's order skips a focus scope
                                        // going back out of it.
                                        Keys.onBacktabPressed: function(event) {
                                            if (columnRevert.nextItemInFocusChain(false).inTableHeader === true) {
                                                event.accepted = false
                                            } else {
                                                columnRevert.focus = false
                                            }
                                        }
                                        Keys.onPressed: function(event) {
                                            if (KeyUtils.isEnter(event)) {
                                                event.accepted = true
                                                columnRevert.clicked()
                                            }
                                        }
                                        width: root.uiPalette.labelIconSize + Kirigami.Units.smallSpacing
                                        height: width
                                        padding: 0
                                        icon.name: "edit-undo"
                                        icon.width: root.columnRevertGlyph
                                        icon.height: root.columnRevertGlyph
                                        display: AbstractButton.IconOnly
                                        // By its field: the track number's column is Track's.
                                        text: headerCell.editable
                                            ? I18n.i18nc("@action:button %1 is a tag field", "Revert %1 in Every File",
                                                root.fieldAt(headerCell.modelData.key).label)
                                            : ""
                                        onClicked: root.tagEditorApi.revertField(headerCell.modelData.key)

                                        RevertToolTip {
                                            button: columnRevert
                                            message: columnRevert.text
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: root.uiPalette.hairline
                    }
                }

                delegate: Item {
                    id: tableRow
                    objectName: "tagEditorRow"
                    required property int index
                    required property string fileName
                    required property string directory
                    required property string formatKind
                    required property string title
                    required property string artist
                    required property string album
                    required property string genre
                    required property string year
                    required property string trackNo
                    required property string discNo
                    required property bool dirty
                    required property bool selected
                    required property string errorText
                    required property bool renameFailed
                    required property var extraValues
                    required property var changedFields
                    readonly property bool current: ListView.isCurrentItem && tagEditorTable.activeFocus
                    // Unsaved edits, typed text included.
                    readonly property bool marked: tableRow.dirty || root.pendingEditRows[tableRow.index] === true

                    width: root.tableContentWidth
                    // Its tallest cell's of those whose text is in view
                    // sideways, as the row is made: the Row's own implicit
                    // height comes with its polish, after the view has
                    // placed the row. A change scrolled out of view grows
                    // it as it comes in (it left a tall, empty row: at 24 pt
                    // from a 1000 px window, every row of a new title).
                    // The pinned cells are always in view, and the others
                    // only from the seam on: under the pinned ones, a
                    // change is out of view too.
                    height: {
                        let tallest = root.tableRowHeight
                        const right = tagEditorTable.contentX - rowCells.x + tagEditorTable.width
                        const left = tagEditorTable.contentX - rowCells.x + (root.pinsColumns ? root.pinnedWidth : 0)
                        for (let i = 0; i < rowCells.children.length; ++i) {
                            const cell = rowCells.children[i]
                            if (cell.height > tallest && (cell.pinned === true && root.pinsColumns || cell.x + cell.leftPadding + 1 < right
                                    && cell.x + cell.width - cell.rightPadding > left + 1)) {
                                tallest = cell.height
                            }
                        }
                        return tallest
                    }
                    // A save or rename failed for the file.
                    readonly property bool failed: tableRow.errorText.length > 0
                    Accessible.role: Accessible.Row
                    Accessible.name: tableRow.fileName
                    Accessible.description: tableRow.failed ? root.cellText("status", tableRow)
                        : tableRow.marked ? root.modifiedText : ""
                    Accessible.selected: tableRow.selected

                    Components.RowBackground {
                        objectName: "tagEditorRowBackground"
                        uiPalette: root.uiPalette
                        scrollbarLane: 0
                        hovered: rowHover.hovered
                        selected: tableRow.selected
                        active: tagEditorTable.activeFocus
                        focused: tableRow.current
                    }

                    // The row is under the pointer wherever the pointer is
                    // on it: the cells above the click area take the hover
                    // from a sibling under them, so the row's tint showed
                    // only in its insets.
                    HoverHandler {
                        id: rowHover
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onClicked: function(mouse) {
                            root.handleRowClick(tableRow.index, mouse.modifiers)
                        }
                    }

                    Row {
                        id: rowCells
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.tableInset
                        anchors.rightMargin: root.tableInset

                        Repeater {
                            model: root.tableColumns
                            delegate: Label {
                                id: tableCell
                                required property var modelData
                                required property int index
                                objectName: "tagEditorCell_" + modelData.key
                                width: root.columnWidth(index)
                                readonly property bool pinned: index < root.pinnedColumnCount
                                z: tableCell.pinned ? 2 : 0
                                transform: Translate { x: tableCell.pinned ? root.pinnedShift : 0 }
                                // A committed change is whole: it wraps,
                                // and the row grows by it. Its height is the
                                // committed value's, also while another is
                                // typed over it, which elides within that.
                                // A file name wraps to three lines before it
                                // is cut, and grows its row alike (cut on
                                // one line, it lost the name's end beside a
                                // title that had made the row two lines).
                                readonly property bool changed: tableRow.changedFields.indexOf(modelData.key) >= 0
                                readonly property bool wrapsName: modelData.key === "fileName"
                                readonly property bool wraps: tableCell.changed || tableCell.wrapsName
                                // Only a name wider than its cell can wrap.
                                readonly property bool nameWraps: tableCell.wrapsName
                                    && tableCell.implicitWidth > tableCell.width
                                readonly property string committedText: root.cellText(modelData.key, tableRow)
                                height: committedValue.item !== null && committedValue.item.lineCount > 1
                                    ? Math.ceil(committedValue.item.contentHeight + 2 * root.cellLineInset)
                                    : root.tableRowHeight
                                visible: width > 0
                                topPadding: tableCell.wraps ? root.cellLineInset : 0
                                leftPadding: modelData.number ? root.numberLeadingRoom : Kirigami.Units.largeSpacing
                                rightPadding: modelData.number ? root.columnRevertRoom
                                    : Kirigami.Units.largeSpacing
                                        + (modelData.key === root.scrollBarEdgeKey && root.tableScrolls
                                            ? root.scrollBarClearance : 0)
                                textFormat: Text.PlainText
                                readonly property bool typed: root.pendingEditKey === modelData.key
                                    && root.pendingEditRows[tableRow.index] === true
                                text: tableCell.typed
                                    ? root.cellText(modelData.key, { [modelData.key]: root.pendingEditText })
                                    : tableCell.committedText
                                font.features: modelData.number ? root.uiPalette.tabularFigures : ({})
                                // A change is DemiBold, as its field's
                                // label is, and a kept value regular, in
                                // one ink: a tint would read as selected.
                                // What it wraps by is measured in its
                                // font, so it still reads whole. A value
                                // being typed is DemiBold too, as the
                                // row's mark and the heading show it at
                                // once; it elides until it is committed,
                                // so its row keeps its height.
                                font.weight: tableCell.changed || tableCell.typed ? Font.DemiBold : Font.Normal
                                color: tableCell.failureCell
                                    ? root.uiPalette.negative
                                    : (modelData.number
                                        ? root.uiPalette.textTertiaryOn(tableRow.selected, rowHover.hovered)
                                        : root.uiPalette.text)
                                verticalAlignment: tableCell.wraps ? Text.AlignTop : Text.AlignVCenter
                                wrapMode: tableCell.wraps ? Text.Wrap : Text.NoWrap
                                maximumLineCount: tableCell.wrapsName ? root.fileNameLineLimit : undefined
                                horizontalAlignment: modelData.number ? Text.AlignRight : Text.AlignLeft
                                // A file name keeps its start: the track
                                // number and title, rather than an
                                // extension that Format shows. A folder
                                // keeps its start and its end, where the
                                // folders of a batch often differ (cut at
                                // the end, each read "ferrous-ren…"). The
                                // whole of any cell is in its tooltip while
                                // it is cut short.
                                elide: tableCell.changed && !tableCell.typed ? Text.ElideNone
                                    : (modelData.key === "directory" ? Text.ElideMiddle : Text.ElideRight)

                                // Under the pinned cells, from the row's edge
                                // to the seam, the row as it is drawn there,
                                // over the cells that scroll: its fill,
                                // selection, hover and the current row's
                                // frame, whose far side is past the seam, so
                                // they run on across it.
                                Item {
                                    visible: tableCell.index === 0 && root.pinsColumns
                                    z: -1
                                    x: -root.tableInset
                                    width: root.pinnedWidth
                                    height: tableRow.height
                                    clip: true

                                    Item {
                                        width: tableRow.width
                                        height: parent.height

                                        Components.RowBackground {
                                            uiPalette: root.uiPalette
                                            opaque: true
                                            hovered: rowHover.hovered
                                            selected: tableRow.selected
                                            active: tagEditorTable.activeFocus
                                            focused: tableRow.current
                                        }
                                    }

                                    Rectangle {
                                        objectName: "tagEditorRowSeam"
                                        anchors.right: parent.right
                                        width: 1
                                        height: parent.height
                                        color: root.uiPalette.seam
                                    }
                                }

                                // The committed change or the file name as
                                // it wraps, which is how tall the cell is:
                                // only those cells have one.
                                Loader {
                                    id: committedValue
                                    active: tableCell.changed || tableCell.nameWraps
                                    visible: false
                                    sourceComponent: Label {
                                        width: tableCell.width - tableCell.leftPadding - tableCell.rightPadding
                                        textFormat: Text.PlainText
                                        text: tableCell.committedText
                                        font: tableCell.font
                                        wrapMode: Text.Wrap
                                        maximumLineCount: tableCell.maximumLineCount
                                    }
                                }
                                // The # cell of a row with unsaved edits
                                // says what its mark means, and the Status
                                // cell of a row that failed, why.
                                readonly property bool failureCell: modelData.key === "status"
                                    && tableRow.errorText.length > 0
                                readonly property string toolTipText: tableCell.failureCell ? tableRow.errorText
                                    : tableCell.truncated
                                        ? (modelData.key === "directory" ? tableRow.directory : tableCell.text)
                                        : modelData.key !== "trackNo" ? ""
                                        : tableRow.failed ? tableRow.errorText
                                        : tableRow.marked ? root.modifiedText : ""
                                Accessible.description: tableCell.failureCell ? tableRow.errorText : ""

                                HoverHandler {
                                    id: cellHover
                                    enabled: tableCell.toolTipText.length > 0
                                }

                                ToolTip.visible: cellHover.hovered && tableCell.toolTipText.length > 0
                                ToolTip.delay: Kirigami.Units.toolTipDelay
                                ToolTip.text: tableCell.toolTipText
                            }
                        }
                    }

                    // Unsaved edits: a dot in the # column's left padding,
                    // which is there in every row, so the edit that brings
                    // it moves nothing.
                    Rectangle {
                        objectName: "tagEditorRowModifiedMark"
                        visible: tableRow.marked && !tableRow.failed
                        x: root.pinnedShift + root.tableInset + Math.round((root.markSlotWidth - width) / 2)
                        // On the first line, as the row's cells start.
                        y: Math.round((root.tableRowHeight - height) / 2)
                        width: root.modifiedMarkSize
                        height: width
                        radius: width / 2
                        color: root.uiPalette.text
                    }

                    // A failed save or rename: the error glyph in the dot's
                    // place, beside the file name, in the negative colour,
                    // readable on the selection too. What failed is its
                    // cell's tooltip, as an edit's dot's is, and its
                    // description; Status and Details… say it too.
                    Kirigami.Icon {
                        objectName: "tagEditorRowFailureMark"
                        visible: tableRow.failed
                        x: root.pinnedShift + root.tableInset + Math.round((root.markSlotWidth - width) / 2)
                        // On the first line, as the row's cells start.
                        y: Math.round((root.tableRowHeight - height) / 2)
                        width: root.failureGlyphSize
                        height: width
                        // At its size, which grows with the font.
                        roundToIconSize: false
                        source: root.failureGlyph
                        isMask: true
                        color: tableRow.selected ? root.uiPalette.negativeSelected : root.uiPalette.negative
                        Accessible.role: Accessible.Graphic
                        Accessible.name: root.cellText("status", tableRow)
                        Accessible.description: tableRow.errorText
                    }
                }
            }
        }
    }

    // The fields, in the form's order.
    readonly property list<Item> fieldEditors: [titleRow.editor, artistRow.editor, albumRow.editor,
        albumArtistRow.editor, genreRow.editor, yearRow.editor, trackPair.firstEditor, trackPair.secondEditor,
        discPair.firstEditor, discPair.secondEditor, commentRow.editor]
    readonly property bool formFieldFocused: root.fieldEditors.some(function(editor) {
        return editor !== null && editor.activeFocus
    })
    // Where Tab goes, up to the files: field to field, past the Revert of
    // an edited one (it took a press for each, and Space there undid the
    // edit just typed), then the fields' Reverts in their order, then the
    // files, whose columns' Reverts follow in the window's own order. The
    // links pass over a Revert that doesn't show.
    readonly property var tabStops: {
        const fields = []
        const reverts = []
        for (let i = 0; i < root.fieldEditors.length; ++i) {
            fields.push(root.fieldEditors[i])
            reverts.push(root.fieldEditors[i].revert)
        }
        return fields.concat(reverts, [tagEditorTable])
    }

    // The stop `step` from `item` in tabStops, or null past either end.
    function tabStopFrom(item, step) {
        const at = root.tabStops.indexOf(item)
        return at >= 0 && at + step >= 0 && at + step < root.tabStops.length ? root.tabStops[at + step] : null
    }

    // Scrolls the table sideways until `item`, in its header, is in view,
    // clear of the pinned columns.
    function showInTable(item) {
        const left = item.mapToItem(tagEditorTable, 0, 0).x
        const pinned = item.pinned !== true && root.pinsColumns ? root.pinnedWidth : 0
        if (left < pinned) {
            tagEditorTable.contentX += left - pinned
        } else if (left + item.width > tagEditorTable.width) {
            tagEditorTable.contentX += left + item.width - tagEditorTable.width
        }
    }

    // The labels' column: as wide as the longest label, semibold.
    readonly property real fieldLabelWidth: {
        let widest = 0
        for (let i = 0; i < root.editorFields.length; ++i) {
            widest = Math.max(widest, boldLabelMetrics.advanceWidth(root.editorFields[i].formLabel))
        }
        return Math.ceil(widest)
    }

    // A field's Revert: its glyph as large as a line of the field's text
    // holds, as the column's Revert: 16 px with the default font (a fixed
    // 16 px stood beside 32 px text at 24 pt).
    readonly property real fieldRevertWidth: root.uiPalette.labelIconSize + 2 * Kirigami.Units.smallSpacing

    // The least the fields' column takes: the labels and a field that shows
    // about 16 characters, so a large font doesn't squeeze the fields, and
    // Track and Disc whole. The table has the rest.
    readonly property real fieldsMinimumWidth: Math.max(Math.ceil(root.fieldLabelWidth
        + Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
        + 16 * fieldMetrics.averageCharacterWidth + 2 * Kirigami.Units.largeSpacing),
        trackPair.leastWidth, discPair.leastWidth)

    FontMetrics {
        id: fieldMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
    }

    FontMetrics {
        id: boldLabelMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
        font.weight: Font.DemiBold
    }

    // A field with its label on the left and, under it, the note on rows
    // that hold several values.
    component FieldRow: ColumnLayout {
        id: fieldRow
        required property var field
        property alias editor: rowEditor
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing

            FieldLabel { field: fieldRow.field }
            TagField { id: rowEditor; field: fieldRow.field }
        }

        Label {
            objectName: "tagEditorExtraValuesNote_" + fieldRow.field.key
            Layout.fillWidth: true
            Layout.leftMargin: root.fieldLabelWidth + Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
            visible: text.length > 0
            textFormat: Text.PlainText
            text: root.extraValuesNotes[fieldRow.field.key] || ""
            wrapMode: Text.WordWrap
            font.pointSize: root.uiPalette.smallPointSize
            color: root.uiPalette.textSecondary
        }
    }

    // Track and Disc: the number, "of", and the total. "of" is 4 px from
    // the field on either side of it, so the three read as one value
    // (at the form's 12 px it stood apart from both). The two fields share
    // what is left of the row equally, so the pair ends where the other
    // fields do (otherwise it ended 35 px short of them).
    component NumberPair: RowLayout {
        id: pair
        required property var first
        required property var second
        // Its fields at their narrowest: "Mixed" whole beside the Revert
        // (at 24 pt it elided to "Mix…" once the Revert followed the font).
        readonly property real fieldLeastWidth: Math.ceil(firstField.leftPadding
            + fieldMetrics.advanceWidth(root.keepText)
            + root.fieldRevertWidth + Kirigami.Units.smallSpacing)
        // The pair at its narrowest: the label, the fields and "of".
        readonly property real leastWidth: Math.ceil(root.fieldLabelWidth + Kirigami.Units.largeSpacing
            + 3 * pair.spacing + ofLabel.implicitWidth + 2 * pair.fieldLeastWidth)
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        property alias firstEditor: firstField
        property alias secondEditor: secondField

        FieldLabel {
            field: pair.first
            Layout.rightMargin: Kirigami.Units.largeSpacing
        }

        TagField {
            id: firstField
            field: pair.first
            Layout.fillWidth: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 3
        }

        Label {
            id: ofLabel
            objectName: "tagEditorOf_" + pair.first.key
            textFormat: Text.PlainText
            text: I18n.i18nc("@label track or disc number of the total", "of")
            color: root.uiPalette.textSecondary
            Accessible.ignored: true
        }

        TagField {
            id: secondField
            field: pair.second
            Layout.fillWidth: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 3
        }
    }
}
