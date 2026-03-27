import QtQuick 2.15
import QtQuick.Controls 2.15

Item {
    id: root

    required property bool viewerOpen
    required property string viewerSource
    required property bool infoVisible
    required property int initialViewToken
    required property int viewerDecodeWidth
    required property int viewerDecodeHeight
    required property string infoOverlayText
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var closeViewer
    required property var toggleInfoVisible
    required property var focusFullscreen
    required property string comparisonLabel
    required property bool comparisonModeAvailable

    property real zoom: 1.0
    property real panX: 0.0
    property real panY: 0.0
    property bool pendingInitialView: false

    function requestInitialView() {
        root.zoom = 1.0
        root.panX = 0.0
        root.panY = 0.0
        root.pendingInitialView = true
        root.applyInitialView()
    }

    function clampPan() {
        const scaledW = albumArtTransform.width * root.zoom
        const scaledH = albumArtTransform.height * root.zoom
        const limitX = Math.max(0, (scaledW - albumArtViewport.width) / 2)
        const limitY = Math.max(0, (scaledH - albumArtViewport.height) / 2)
        root.panX = Math.max(-limitX, Math.min(limitX, root.panX))
        root.panY = Math.max(-limitY, Math.min(limitY, root.panY))
    }

    function isPointOnImage(item, x, y) {
        const p = albumArtImageFull.mapFromItem(item, x, y)
        const xOff = (albumArtImageFull.width - albumArtImageFull.paintedWidth) / 2
        const yOff = (albumArtImageFull.height - albumArtImageFull.paintedHeight) / 2
        return p.x >= xOff
            && p.y >= yOff
            && p.x <= xOff + albumArtImageFull.paintedWidth
            && p.y <= yOff + albumArtImageFull.paintedHeight
    }

    function applyInitialView() {
        if (!root.pendingInitialView || !root.viewerOpen) {
            return
        }
        if (albumArtViewport.width <= 0 || albumArtViewport.height <= 0) {
            return
        }
        if (albumArtImageFull.status === Image.Loading) {
            return
        }
        root.zoom = 1.0
        root.panX = 0.0
        root.panY = 0.0
        root.clampPan()
        root.pendingInitialView = false
    }

    onInitialViewTokenChanged: root.requestInitialView()
    onViewerOpenChanged: root.applyInitialView()
    onWidthChanged: root.applyInitialView()
    onHeightChanged: root.applyInitialView()

    clip: true

    Item {
        id: albumArtViewport
        anchors.fill: parent
        clip: true
        onWidthChanged: root.applyInitialView()
        onHeightChanged: root.applyInitialView()

        Item {
            id: albumArtTransform
            readonly property real nativeWidth: albumArtImageFull.sourceSize.width > 0
                ? albumArtImageFull.sourceSize.width
                : albumArtViewport.width
            readonly property real nativeHeight: albumArtImageFull.sourceSize.height > 0
                ? albumArtImageFull.sourceSize.height
                : albumArtViewport.height
            readonly property real fitScale: {
                const w = nativeWidth > 0 ? nativeWidth : 1
                const h = nativeHeight > 0 ? nativeHeight : 1
                const scaleX = albumArtViewport.width / w
                const scaleY = albumArtViewport.height / h
                return Math.min(1.0, scaleX, scaleY)
            }

            width: Math.max(1, nativeWidth * fitScale)
            height: Math.max(1, nativeHeight * fitScale)
            x: (albumArtViewport.width - width) / 2 + root.panX
            y: (albumArtViewport.height - height) / 2 + root.panY
            scale: root.zoom
            transformOrigin: Item.Center

            Image {
                id: albumArtImageFull
                anchors.fill: parent
                source: root.viewerSource
                fillMode: Image.PreserveAspectFit
                smooth: true
                asynchronous: true
                cache: true
                retainWhileLoading: true
                sourceSize.width: root.viewerDecodeWidth
                sourceSize.height: root.viewerDecodeHeight
                onStatusChanged: root.applyInitialView()
            }
        }

        MouseArea {
            id: albumArtPanArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            preventStealing: true
            property real lastX: 0
            property real lastY: 0
            cursorShape: root.zoom > 1.0 ? Qt.OpenHandCursor : Qt.ArrowCursor

            onPressed: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    albumArtViewerContextMenu.popup()
                    return
                }
                if (!root.isPointOnImage(albumArtPanArea, mouse.x, mouse.y)) {
                    root.closeViewer()
                    return
                }
                lastX = mouse.x
                lastY = mouse.y
                cursorShape = Qt.ClosedHandCursor
            }

            onReleased: {
                cursorShape = root.zoom > 1.0 ? Qt.OpenHandCursor : Qt.ArrowCursor
            }

            onPositionChanged: function(mouse) {
                if (!pressed || root.zoom <= 1.0) {
                    return
                }
                root.panX += mouse.x - lastX
                root.panY += mouse.y - lastY
                lastX = mouse.x
                lastY = mouse.y
                root.clampPan()
            }

            onDoubleClicked: function(mouse) {
                if (mouse.button !== Qt.LeftButton) {
                    return
                }
                if (root.zoom > 1.0) {
                    root.zoom = 1.0
                    root.panX = 0.0
                    root.panY = 0.0
                } else {
                    root.zoom = 2.0
                    root.clampPan()
                }
            }

            onWheel: function(wheel) {
                const oldZoom = root.zoom
                const delta = wheel.angleDelta.y > 0 ? 1.1 : 0.9
                const nextZoom = Math.max(1.0, Math.min(6.0, oldZoom * delta))
                if (Math.abs(nextZoom - oldZoom) < 0.0001) {
                    wheel.accepted = true
                    return
                }
                const pivotX = wheel.x - albumArtViewport.width / 2
                const pivotY = wheel.y - albumArtViewport.height / 2
                const ratio = nextZoom / oldZoom
                root.zoom = nextZoom
                root.panX = (root.panX + pivotX) * ratio - pivotX
                root.panY = (root.panY + pivotY) * ratio - pivotY
                root.clampPan()
                wheel.accepted = true
            }
        }

        Menu {
            id: albumArtViewerContextMenu

            MenuItem { action: root.replaceFromItunesAction }
            MenuItem {
                enabled: false
                visible: !root.replaceFromItunesAction.enabled
                text: root.currentTrackItunesArtworkDisabledReason()
            }
        }
    }

    Column {
        z: 30
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 12
        spacing: 8

        Rectangle {
            visible: root.comparisonLabel.length > 0
            color: Qt.rgba(0, 0, 0, 0.58)
            border.color: Qt.rgba(1, 1, 1, 0.24)
            radius: 10
            width: comparisonLabelRow.implicitWidth + 20
            height: comparisonLabelRow.implicitHeight + 12

            Row {
                id: comparisonLabelRow
                anchors.centerIn: parent
                spacing: 6

                Text {
                    visible: root.comparisonModeAvailable
                    text: "\u25C0 \u25B6"
                    color: Qt.rgba(1, 1, 1, 0.45)
                    font.pixelSize: 13
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: root.comparisonLabel
                    color: "#ffffff"
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                hoverEnabled: true
                preventStealing: true
                onPressed: root.focusFullscreen()
                onWheel: function(wheel) {
                    root.focusFullscreen()
                    wheel.accepted = true
                }
            }
        }

        Rectangle {
            width: 40
            height: 40
            radius: 8
            color: Qt.rgba(1, 1, 1, 0.16)
            border.color: Qt.rgba(1, 1, 1, 0.52)

            ToolButton {
                anchors.fill: parent
                contentItem: Text {
                    text: "i"
                    color: "#ffffff"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: root.toggleInfoVisible()
            }
        }

        Rectangle {
            visible: root.infoVisible && albumArtInfoLabel.text.length > 0
            width: Math.min(540, root.width - 24)
            color: Qt.rgba(0, 0, 0, 0.58)
            border.color: Qt.rgba(1, 1, 1, 0.24)
            radius: 10
            implicitHeight: albumArtInfoLabel.implicitHeight + 20

            Text {
                id: albumArtInfoLabel
                anchors.fill: parent
                anchors.margins: 10
                color: "#f2f2f2"
                text: root.infoOverlayText
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                hoverEnabled: true
                preventStealing: true
                onPressed: root.focusFullscreen()
                onWheel: function(wheel) {
                    root.focusFullscreen()
                    wheel.accepted = true
                }
            }
        }
    }
}
