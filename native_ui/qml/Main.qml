import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQml 2.15
import FerrousNative 1.0
import org.kde.kirigami 2.20 as Kirigami

Kirigami.ApplicationWindow {
    id: root
    width: 1600
    height: 980
    minimumWidth: 1280
    minimumHeight: 780
    visible: true
    title: "Ferrous"
    property int selectedLibraryAlbumIndex: -1
    property var filteredLibrarySourceIndexes: []
    property var filteredLibraryLabels: []
    property int lastCenteredQueueIndex: -2
    readonly property var uiBridge: bridge ? bridge : bridgeFallback

    QtObject {
        id: bridgeFallback
        property string playbackState: "Stopped"
        property string positionText: "00:00"
        property string durationText: "00:00"
        property real positionSeconds: 0
        property real durationSeconds: 0
        property real volume: 1.0
        property int queueLength: 0
        property var queueItems: []
        property int selectedQueueIndex: -1
        property var waveformPeaksPacked: ""
        property bool spectrogramReset: false
        property real dbRange: 90
        property bool logScale: false
        property int sampleRateHz: 48000
        property var libraryAlbums: []
        property bool libraryScanInProgress: false
        property int libraryRootCount: 0
        property int libraryTrackCount: 0
        property bool connected: false
        signal snapshotChanged()
        signal bridgeError(string message)
        function play() {}
        function pause() {}
        function stop() {}
        function next() {}
        function previous() {}
        function seek(seconds) {}
        function setVolume(value) {}
        function playAt(index) {}
        function selectQueueIndex(index) {}
        function removeAt(index) {}
        function moveQueue(from, to) {}
        function clearQueue() {}
        function replaceAlbumAt(index) {}
        function appendAlbumAt(index) {}
        function scanRoot(path) {}
        function scanDefaultMusicRoot() {}
        function requestSnapshot() {}
        function shutdown() {}
        function takeSpectrogramRowsDeltaPacked() { return ({ rows: 0, bins: 0, data: "" }) }
    }

    function togglePlayPause() {
        if (uiBridge.playbackState === "Playing") {
            uiBridge.pause()
        } else {
            uiBridge.play()
        }
    }

    function moveSelected(delta) {
        const from = uiBridge.selectedQueueIndex
        if (from < 0 || uiBridge.queueLength <= 0) {
            return
        }
        const to = Math.max(0, Math.min(uiBridge.queueLength - 1, from + delta))
        if (to !== from) {
            uiBridge.moveQueue(from, to)
        }
    }

    function librarySourceIndexAt(viewIndex) {
        if (viewIndex < 0 || viewIndex >= filteredLibrarySourceIndexes.length) {
            return -1
        }
        return filteredLibrarySourceIndexes[viewIndex]
    }

    function rebuildLibraryFilter() {
        const labels = uiBridge.libraryAlbums || []
        const term = librarySearchField.text.trim().toLowerCase()
        const filteredIndexes = []
        const filteredLabels = []
        for (let i = 0; i < labels.length; i++) {
            const label = labels[i]
            if (term.length === 0 || label.toLowerCase().indexOf(term) !== -1) {
                filteredIndexes.push(i)
                filteredLabels.push(label)
            }
        }
        filteredLibrarySourceIndexes = filteredIndexes
        filteredLibraryLabels = filteredLabels
        if (selectedLibraryAlbumIndex >= 0
                && filteredLibrarySourceIndexes.indexOf(selectedLibraryAlbumIndex) === -1) {
            selectedLibraryAlbumIndex = -1
        }
    }

    Action {
        id: quitAction
        text: "Quit"
        shortcut: StandardKey.Quit
        onTriggered: Qt.quit()
    }
    Action {
        id: previousAction
        text: "Previous"
        icon.name: "media-skip-backward"
        shortcut: "Ctrl+Left"
        onTriggered: uiBridge.previous()
    }
    Action {
        id: playAction
        text: "Play"
        icon.name: "media-playback-start"
        shortcut: "Media Play"
        onTriggered: uiBridge.play()
    }
    Action {
        id: pauseAction
        text: "Pause"
        icon.name: "media-playback-pause"
        shortcut: "Media Pause"
        onTriggered: uiBridge.pause()
    }
    Action {
        id: stopAction
        text: "Stop"
        icon.name: "media-playback-stop"
        shortcut: "Media Stop"
        onTriggered: uiBridge.stop()
    }
    Action {
        id: nextAction
        text: "Next"
        icon.name: "media-skip-forward"
        shortcut: "Ctrl+Right"
        onTriggered: uiBridge.next()
    }
    Action {
        id: clearPlaylistAction
        text: "Clear Playlist"
        onTriggered: uiBridge.clearQueue()
    }
    Action {
        id: moveTrackUpAction
        text: "Move Track Up"
        shortcut: "Ctrl+Shift+Up"
        onTriggered: root.moveSelected(-1)
    }
    Action {
        id: moveTrackDownAction
        text: "Move Track Down"
        shortcut: "Ctrl+Shift+Down"
        onTriggered: root.moveSelected(1)
    }

    Shortcut {
        sequence: "Space"
        onActivated: root.togglePlayPause()
    }
    Shortcut {
        sequence: "Media Previous"
        onActivated: previousAction.trigger()
    }
    Shortcut {
        sequence: "Media Next"
        onActivated: nextAction.trigger()
    }
    Shortcut {
        sequence: "Delete"
        onActivated: {
            if (uiBridge.selectedQueueIndex >= 0) {
                uiBridge.removeAt(uiBridge.selectedQueueIndex)
            }
        }
    }

    menuBar: MenuBar {
        Menu {
            title: "File"
            MenuItem { action: quitAction }
        }
        Menu {
            title: "Edit"
        }
        Menu {
            title: "View"
        }
        Menu {
            title: "Playback"
            MenuItem { action: previousAction }
            MenuItem { action: playAction }
            MenuItem { action: pauseAction }
            MenuItem { action: stopAction }
            MenuItem { action: nextAction }
            MenuSeparator {}
            MenuItem { action: moveTrackUpAction }
            MenuItem { action: moveTrackDownAction }
            MenuSeparator {}
            MenuItem { action: clearPlaylistAction }
        }
        Menu {
            title: "Help"
        }
    }

    footer: ToolBar {
        contentItem: RowLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: uiBridge.connected
                    ? (uiBridge.playbackState.toLowerCase() + " | "
                       + uiBridge.positionText + "/" + uiBridge.durationText
                       + " | tracks " + uiBridge.queueLength)
                    : "bridge disconnected"
                elide: Text.ElideRight
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            id: transportBar
            Layout.fillWidth: true
            implicitHeight: 56

            contentItem: RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 12
                spacing: 8

                RowLayout {
                    spacing: 2
                    ToolButton { action: previousAction; display: AbstractButton.IconOnly }
                    ToolButton { action: playAction; display: AbstractButton.IconOnly }
                    ToolButton { action: pauseAction; display: AbstractButton.IconOnly }
                    ToolButton { action: stopAction; display: AbstractButton.IconOnly }
                    ToolButton { action: nextAction; display: AbstractButton.IconOnly }
                }

                Slider {
                    id: seekSlider
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(uiBridge.durationSeconds, 1.0)
                    stepSize: 0
                    onPressedChanged: {
                        if (!pressed) {
                            uiBridge.seek(value)
                        }
                    }

                    background: Item {
                        implicitHeight: 24
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            anchors.fill: parent
                            color: "white"
                            border.color: "#a0a9b3"
                            radius: 1
                        }

                        WaveformItem {
                            id: waveformItem
                            anchors.fill: parent
                            anchors.margins: 1
                            peaksData: uiBridge.waveformPeaksPacked
                            positionSeconds: uiBridge.positionSeconds
                            durationSeconds: uiBridge.durationSeconds
                        }
                    }

                    handle: Rectangle {
                        x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
                        y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                        implicitWidth: 3
                        implicitHeight: seekSlider.height - 4
                        radius: 1
                        color: "#2f7cd6"
                        border.color: "#1f5aa7"
                    }
                }

                Binding {
                    target: seekSlider
                    property: "value"
                    value: uiBridge.positionSeconds
                    when: !seekSlider.pressed
                }

                Label {
                    text: uiBridge.positionText + "/" + uiBridge.durationText
                    horizontalAlignment: Text.AlignRight
                    Layout.minimumWidth: 96
                }

                ToolButton {
                    icon.name: "audio-volume-high"
                    display: AbstractButton.IconOnly
                    enabled: false
                    focusPolicy: Qt.NoFocus
                }

                Slider {
                    id: volumeSlider
                    Layout.preferredWidth: 140
                    from: 0
                    to: 1
                    stepSize: 0
                    onMoved: uiBridge.setVolume(value)
                    onPressedChanged: {
                        if (!pressed) {
                            uiBridge.setVolume(value)
                        }
                    }
                }

                Binding {
                    target: volumeSlider
                    property: "value"
                    value: uiBridge.volume
                    when: !volumeSlider.pressed
                }
            }
        }

        SplitView {
            id: mainSplit
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            Rectangle {
                color: Kirigami.Theme.backgroundColor
                SplitView.preferredWidth: Math.max(300, root.width * 0.26)
                SplitView.minimumWidth: 250

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: width
                        color: "#0c0c0c"

                        Text {
                            anchors.centerIn: parent
                            text: "Album Art"
                            color: "#8c8c8c"
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Kirigami.Theme.backgroundColor
                        border.color: Qt.rgba(0, 0, 0, 0.12)

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                ComboBox {
                                    model: ["Folders"]
                                    Layout.fillWidth: true
                                }
                                ToolButton {
                                    icon.name: "document-edit"
                                    display: AbstractButton.IconOnly
                                }
                                Button {
                                    text: "Scan Music"
                                    onClicked: uiBridge.scanDefaultMusicRoot()
                                }
                            }

                            TextField {
                                id: librarySearchField
                                Layout.fillWidth: true
                                placeholderText: "Search"
                                onTextChanged: root.rebuildLibraryFilter()
                            }

                            Label {
                                Layout.fillWidth: true
                                text: "Indexed tracks: " + uiBridge.libraryTrackCount
                                      + " | roots: " + uiBridge.libraryRootCount
                                      + (uiBridge.libraryScanInProgress ? " | scanning..." : "")
                                color: Kirigami.Theme.disabledTextColor
                                elide: Text.ElideRight
                            }

                            ListView {
                                id: libraryAlbumView
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: root.filteredLibraryLabels

                                delegate: Rectangle {
                                    readonly property int sourceIndex: root.librarySourceIndexAt(index)
                                    width: ListView.view.width
                                    height: 24
                                    color: sourceIndex === root.selectedLibraryAlbumIndex
                                        ? Kirigami.Theme.highlightColor
                                        : (index % 2 === 0
                                            ? Kirigami.Theme.backgroundColor
                                            : Kirigami.Theme.alternateBackgroundColor)

                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.leftMargin: 8
                                        text: modelData
                                        elide: Text.ElideRight
                                        anchors.right: parent.right
                                        anchors.rightMargin: 6
                                        color: sourceIndex === root.selectedLibraryAlbumIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        onClicked: function(mouse) {
                                            root.selectedLibraryAlbumIndex = sourceIndex
                                            if (mouse.button === Qt.RightButton) {
                                                albumMenu.popup()
                                            }
                                        }
                                        onDoubleClicked: {
                                            if (sourceIndex >= 0) {
                                                uiBridge.replaceAlbumAt(sourceIndex)
                                            }
                                        }
                                    }

                                    Menu {
                                        id: albumMenu
                                        MenuItem {
                                            text: "Play Album"
                                            onTriggered: {
                                                if (sourceIndex >= 0) {
                                                    uiBridge.replaceAlbumAt(sourceIndex)
                                                }
                                            }
                                        }
                                        MenuItem {
                                            text: "Append Album"
                                            onTriggered: {
                                                if (sourceIndex >= 0) {
                                                    uiBridge.appendAlbumAt(sourceIndex)
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            Label {
                                visible: root.filteredLibraryLabels.length === 0
                                text: uiBridge.libraryAlbums.length === 0
                                    ? (uiBridge.libraryScanInProgress ? "Scanning library..." : "No albums indexed")
                                    : "No albums match search"
                                color: Kirigami.Theme.disabledTextColor
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                }
            }

            SplitView {
                orientation: Qt.Vertical
                SplitView.fillWidth: true

                Rectangle {
                    color: Kirigami.Theme.backgroundColor
                    SplitView.fillWidth: true
                    SplitView.preferredHeight: root.height * 0.58
                    SplitView.minimumHeight: 220
                    border.color: Qt.rgba(0, 0, 0, 0.12)

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 26
                            color: Kirigami.Theme.alternateBackgroundColor
                            border.color: Qt.rgba(0, 0, 0, 0.08)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                Label { text: "#"; Layout.preferredWidth: 24 }
                                Label { text: "Title"; Layout.fillWidth: true }
                                Label { text: "Length"; Layout.preferredWidth: 72 }
                            }
                        }

                        ListView {
                            id: playlistView
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: uiBridge.queueItems

                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 24
                                color: index === uiBridge.selectedQueueIndex
                                    ? Kirigami.Theme.highlightColor
                                    : (index % 2 === 0 ? Kirigami.Theme.backgroundColor
                                                        : Kirigami.Theme.alternateBackgroundColor)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    Label {
                                        text: (index + 1).toString().padStart(2, "0")
                                        Layout.preferredWidth: 24
                                        color: index === uiBridge.selectedQueueIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                    Label {
                                        text: modelData
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        color: index === uiBridge.selectedQueueIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                    Label {
                                        text: ""
                                        Layout.preferredWidth: 72
                                        horizontalAlignment: Text.AlignRight
                                        color: index === uiBridge.selectedQueueIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton
                                    onPressed: uiBridge.selectQueueIndex(index)
                                    onDoubleClicked: uiBridge.playAt(index)
                                }
                            }
                        }

                        Label {
                            visible: uiBridge.queueLength === 0
                            text: "Playlist is empty"
                            color: Kirigami.Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 10
                        }

                        Connections {
                            target: uiBridge
                            function onSnapshotChanged() {
                                if (uiBridge.selectedQueueIndex >= 0
                                        && uiBridge.selectedQueueIndex !== root.lastCenteredQueueIndex) {
                                    playlistView.positionViewAtIndex(uiBridge.selectedQueueIndex, ListView.Contain)
                                    root.lastCenteredQueueIndex = uiBridge.selectedQueueIndex
                                } else if (uiBridge.selectedQueueIndex < 0) {
                                    root.lastCenteredQueueIndex = -2
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 220
                    color: "#0b0b0f"
                    border.color: Qt.rgba(0, 0, 0, 0.25)

                    SpectrogramItem {
                        id: spectrogramItem
                        anchors.fill: parent
                        maxColumns: Math.max(640, Math.floor(width))
                        dbRange: uiBridge.dbRange
                        logScale: uiBridge.logScale
                        sampleRateHz: uiBridge.sampleRateHz
                    }
                }
            }
        }
    }

    onClosing: function(close) { uiBridge.shutdown() }

    Connections {
        target: uiBridge
        function onSnapshotChanged() {
            root.rebuildLibraryFilter()
            if (root.selectedLibraryAlbumIndex >= uiBridge.libraryAlbums.length) {
                root.selectedLibraryAlbumIndex = uiBridge.libraryAlbums.length - 1
            }
            if (uiBridge.spectrogramReset) {
                spectrogramItem.reset()
            }
            const delta = uiBridge.takeSpectrogramRowsDeltaPacked()
            if (delta.rows > 0 && delta.bins > 0) {
                spectrogramItem.appendPackedRows(delta.data, delta.rows, delta.bins)
            }
        }
        function onBridgeError(message) {
            if (message.indexOf("[analysis]") !== -1
                    || message.indexOf("[gst]") !== -1
                    || message.indexOf("[bridge]") !== -1
                    || message.indexOf("[bridge-json]") !== -1) {
                return
            }
            console.warn("bridge error:", message)
        }
    }

    Component.onCompleted: {
        root.rebuildLibraryFilter()
    }
}
