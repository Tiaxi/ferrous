// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "." as Components

// A search palette's list of results (SearchPalette), in its body: the
// palette sets the model, the selection, the keys and the rows.
ListView {
    id: root
    required property Components.SearchPalette searchPalette
    objectName: root.searchPalette.namePrefix + "ResultsView"
    width: parent.width
    height: parent.height
    visible: root.searchPalette.resultsShown
    clip: true
    reuseItems: true
    // Each query makes only the rows in view; rows scrolled in come from
    // the reuse pool.
    cacheBuffer: 0
    bottomMargin: 6
    highlightFollowsCurrentItem: false
    keyNavigationEnabled: false
    boundsBehavior: Flickable.StopAtBounds
    flickDeceleration: root.searchPalette.snappyScrollFlickDeceleration
    maximumFlickVelocity: root.searchPalette.snappyScrollMaxFlickVelocity
    // The rows a running search is to replace take no clicks or
    // scrolling. The view stays enabled: a disabled Kirigami icon draws
    // dimmed, so the glyphs dimmed on each keystroke.
    interactive: !root.searchPalette.searching
    // The busy indicator of a slow search: the rows it is to replace dim.
    // A keystroke's search takes tens of milliseconds, and dimming for it
    // flashed the list, so the indicator has a threshold, the animation's
    // pause: it shows for a search still running 250 ms after its query,
    // each query starting the pause again, and goes as the search ends.
    readonly property string runningQuery: root.searchPalette.searching ? root.searchPalette.queryText : ""
    onRunningQueryChanged: {
        if (root.runningQuery.length === 0) {
            slowSearchDim.stop()
            root.opacity = 1
        } else if (root.opacity === 1) {
            slowSearchDim.restart()
        }
    }
    SequentialAnimation {
        id: slowSearchDim
        objectName: root.searchPalette.namePrefix + "SlowSearchDim"
        PauseAnimation { duration: 250 }
        NumberAnimation {
            target: root
            property: "opacity"
            to: 0.5
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.OutCubic
        }
    }
    // The app's lane scrollbar, as the library's and the queue's (Breeze's
    // own was another hand here): alone in the lane at the right, where
    // their tints end while it shows (it cut a pale piece out of the
    // selected row).
    ScrollBar.vertical: Components.LaneScrollBar {
        id: resultScrollBar
        objectName: root.searchPalette.namePrefix + "ScrollBar"
        uiPalette: root.searchPalette.uiPalette
        interactive: !root.searchPalette.searching
    }
    // The lane the rows' tints leave for the scrollbar, when it shows.
    // Their text leaves it always, so that the lengths and where the
    // context is cut stay put as typing brings the scrollbar and takes it
    // away.
    readonly property real scrollbarLane: resultScrollBar.visible ? resultScrollBar.lane : 0
    readonly property real reservedLane: resultScrollBar.lane
}
