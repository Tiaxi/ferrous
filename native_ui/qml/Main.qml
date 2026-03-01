import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQml 2.15
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

    function togglePlayPause() {
        if (bridge.playbackState === "Playing") {
            bridge.pause()
        } else {
            bridge.play()
        }
    }

    function moveSelected(delta) {
        const from = bridge.selectedQueueIndex
        if (from < 0 || bridge.queueLength <= 0) {
            return
        }
        const to = Math.max(0, Math.min(bridge.queueLength - 1, from + delta))
        if (to !== from) {
            bridge.moveQueue(from, to)
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
        onTriggered: bridge.previous()
    }
    Action {
        id: playAction
        text: "Play"
        icon.name: "media-playback-start"
        shortcut: "Media Play"
        onTriggered: bridge.play()
    }
    Action {
        id: pauseAction
        text: "Pause"
        icon.name: "media-playback-pause"
        shortcut: "Media Pause"
        onTriggered: bridge.pause()
    }
    Action {
        id: stopAction
        text: "Stop"
        icon.name: "media-playback-stop"
        shortcut: "Media Stop"
        onTriggered: bridge.stop()
    }
    Action {
        id: nextAction
        text: "Next"
        icon.name: "media-skip-forward"
        shortcut: "Ctrl+Right"
        onTriggered: bridge.next()
    }
    Action {
        id: clearPlaylistAction
        text: "Clear Playlist"
        onTriggered: bridge.clearQueue()
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
            if (bridge.selectedQueueIndex >= 0) {
                bridge.removeAt(bridge.selectedQueueIndex)
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
                text: bridge.connected
                    ? (bridge.playbackState.toLowerCase() + " | "
                       + bridge.positionText + "/" + bridge.durationText
                       + " | tracks " + bridge.queueLength)
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
                    to: Math.max(bridge.durationSeconds, 1.0)
                    stepSize: 0
                    onMoved: bridge.seek(value)
                    onPressedChanged: {
                        if (!pressed) {
                            bridge.seek(value)
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

                        Canvas {
                            id: waveformCanvas
                            anchors.fill: parent
                            anchors.margins: 1
                            antialiasing: false

                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                const w = width
                                const h = height
                                if (w <= 0 || h <= 0) {
                                    return
                                }

                                const peaks = bridge.waveformPeaks
                                ctx.fillStyle = "#ffffff"
                                ctx.fillRect(0, 0, w, h)

                                if (peaks.length > 0) {
                                    ctx.fillStyle = "#0f2e5d"
                                    const centerY = h / 2
                                    for (let x = 0; x < w; x++) {
                                        const idx = Math.floor((x / Math.max(1, w - 1)) * (peaks.length - 1))
                                        const peak = Math.max(0.0, Math.min(1.0, Number(peaks[idx])))
                                        const bar = Math.max(1, Math.floor(peak * (h / 2)))
                                        ctx.fillRect(x, centerY - bar, 1, bar * 2)
                                    }
                                }

                                const progress = bridge.durationSeconds > 0
                                    ? Math.max(0, Math.min(1, bridge.positionSeconds / bridge.durationSeconds))
                                    : 0
                                const progressX = Math.floor(progress * w)

                                ctx.fillStyle = "rgba(120, 190, 255, 0.26)"
                                ctx.fillRect(0, 0, progressX, h)

                                ctx.fillStyle = "#2f7cd6"
                                ctx.fillRect(progressX, 0, 1, h)
                            }

                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()

                            Connections {
                                target: bridge
                                function onSnapshotChanged() {
                                    waveformCanvas.requestPaint()
                                }
                            }
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
                    value: bridge.positionSeconds
                    when: !seekSlider.pressed
                }

                Label {
                    text: bridge.positionText + "/" + bridge.durationText
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
                    stepSize: 0.01
                    onMoved: bridge.setVolume(value)
                    onPressedChanged: {
                        if (!pressed) {
                            bridge.setVolume(value)
                        }
                    }
                }

                Binding {
                    target: volumeSlider
                    property: "value"
                    value: bridge.volume
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
                                Button { text: "Configure" }
                            }

                            TextField {
                                Layout.fillWidth: true
                                placeholderText: "Search"
                            }

                            ListView {
                                id: libraryAlbumView
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: bridge.libraryAlbums

                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: 24
                                    color: index === root.selectedLibraryAlbumIndex
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
                                        color: index === root.selectedLibraryAlbumIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        onClicked: function(mouse) {
                                            root.selectedLibraryAlbumIndex = index
                                            if (mouse.button === Qt.RightButton) {
                                                albumMenu.popup()
                                            }
                                        }
                                        onDoubleClicked: bridge.replaceAlbumAt(index)
                                    }

                                    Menu {
                                        id: albumMenu
                                        MenuItem {
                                            text: "Play Album"
                                            onTriggered: bridge.replaceAlbumAt(index)
                                        }
                                        MenuItem {
                                            text: "Append Album"
                                            onTriggered: bridge.appendAlbumAt(index)
                                        }
                                    }
                                }
                            }

                            Label {
                                visible: bridge.libraryAlbums.length === 0
                                text: bridge.libraryScanInProgress ? "Scanning library..." : "No albums indexed"
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
                            model: bridge.queueItems

                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 24
                                color: index === bridge.selectedQueueIndex
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
                                        color: index === bridge.selectedQueueIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                    Label {
                                        text: modelData
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        color: index === bridge.selectedQueueIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                    Label {
                                        text: ""
                                        Layout.preferredWidth: 72
                                        horizontalAlignment: Text.AlignRight
                                        color: index === bridge.selectedQueueIndex
                                            ? Kirigami.Theme.highlightedTextColor
                                            : Kirigami.Theme.textColor
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton
                                    onClicked: bridge.selectQueueIndex(index)
                                    onDoubleClicked: bridge.playAt(index)
                                }
                            }
                        }

                        Label {
                            visible: bridge.queueLength === 0
                            text: "Playlist is empty"
                            color: Kirigami.Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 10
                        }

                        Connections {
                            target: bridge
                            function onSnapshotChanged() {
                                if (bridge.selectedQueueIndex >= 0) {
                                    playlistView.positionViewAtIndex(bridge.selectedQueueIndex, ListView.Contain)
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

                    Canvas {
                        id: spectroPlaceholder
                        anchors.fill: parent
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const w = width
                            const h = height

                            const grad = ctx.createLinearGradient(0, h, 0, 0)
                            grad.addColorStop(0.0, "#fff470")
                            grad.addColorStop(0.35, "#ff7a00")
                            grad.addColorStop(0.65, "#c00074")
                            grad.addColorStop(1.0, "#11004f")
                            ctx.fillStyle = grad
                            ctx.fillRect(0, 0, w, h)

                            ctx.fillStyle = "rgba(0,0,0,0.26)"
                            for (let x = 0; x < w; x += 20) {
                                const stripe = (Math.sin(x / 43.0) * 0.5 + 0.5) * h * 0.65
                                ctx.fillRect(x, 0, 8, stripe)
                            }

                            ctx.fillStyle = "#ffffff"
                            ctx.globalAlpha = 0.22
                            for (let y = h - 4; y > h * 0.72; y -= 3) {
                                ctx.fillRect(0, y, w, 1)
                            }
                            ctx.globalAlpha = 1.0
                        }
                    }
                }
            }
        }
    }

    onClosing: function(close) { bridge.shutdown() }

    Connections {
        target: bridge
        function onSnapshotChanged() {
            if (root.selectedLibraryAlbumIndex >= bridge.libraryAlbums.length) {
                root.selectedLibraryAlbumIndex = bridge.libraryAlbums.length - 1
            }
        }
        function onBridgeError(message) {
            console.warn("bridge error:", message)
        }
    }
}
