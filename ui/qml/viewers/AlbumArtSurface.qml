// SPDX-License-Identifier: GPL-3.0-or-later

import QtCore
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/PathUtils.js" as PathUtils
import "../logic/WheelUtils.js" as WheelUtils

// The album art viewer's view: the art on the view colour, fitted, zoomed
// and panned; while `infoVisible`, a 280 px column of the image's details
// behind a 1 px seam on the right.
Item {
    id: root

    required property bool viewerOpen
    required property string viewerSource
    required property bool infoVisible
    required property int initialViewToken
    required property int viewerDecodeWidth
    required property int viewerDecodeHeight
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var closeViewer
    required property var focusFullscreen
    required property bool comparisonModeAvailable
    required property var uiPalette
    // The image's details, from BridgeClient.cachedImageFileDetails: path,
    // fileName, width, height, fileSizeText, format, embeddedCover.
    property var fileInfo: ({})
    // Where the image is from: "folder", "embedded" or "itunes".
    property string sourceKind: "folder"
    property var switchComparisonImage: function() {}
    // How far the details keep below the top, clear of controls over them.
    property real infoTopInset: 0
    readonly property alias imageStatus: albumArtImageFull.status
    readonly property bool zoomed: root.zoom > 1.0

    property real zoom: 1.0
    // One wheel notch zooms by this factor; smaller steps by part of it.
    readonly property real zoomPerNotch: 1.1
    readonly property real wheelPixelsPerNotch: 120
    property real panX: 0.0
    property real panY: 0.0
    property bool pendingInitialView: false

    // Fits the art again: the viewer's Reset, and a double-click.
    function resetView() {
        root.zoom = 1.0
        root.panX = 0.0
        root.panY = 0.0
    }

    // The folder's path with the home folder as "~".
    function displayPath(path) {
        const home = PathUtils.urlToLocalPath(StandardPaths.writableLocation(StandardPaths.HomeLocation))
        if (home.length > 1 && (path === home || path.indexOf(home + "/") === 0)) {
            return "~" + path.substring(home.length)
        }
        return path
    }

    function copyText(text) {
        clipboardText.text = text
        clipboardText.selectAll()
        clipboardText.copy()
        clipboardText.text = ""
    }

    function requestInitialView() {
        root.zoom = 1.0
        root.panX = 0.0
        root.panY = 0.0
        root.pendingInitialView = true
        root.applyInitialView()
    }

    // Opens the context menu from the keyboard, at the viewer's centre.
    function openContextMenu() {
        albumArtViewerContextMenu.popup(albumArtViewport,
            albumArtViewport.width / 2, albumArtViewport.height / 2)
    }

    function clampPan() {
        const scaledW = albumArtTransform.width * root.zoom
        const scaledH = albumArtTransform.height * root.zoom
        const limitX = Math.max(0, (scaledW - albumArtViewport.width) / 2)
        const limitY = Math.max(0, (scaledH - albumArtViewport.height) / 2)
        root.panX = Math.max(-limitX, Math.min(limitX, root.panX))
        root.panY = Math.max(-limitY, Math.min(limitY, root.panY))
    }

    // Zooms to `nextZoom` keeping the image point at (`x`, `y`) in the
    // viewport under that point, as far as the pan limits allow. The image
    // is centred in the viewport, offset by the pan and scaled about its
    // centre, so the point at `pivot = (x, y) - viewport centre` shows the
    // image point `(pivot - pan) / zoom` from the image's centre.
    function zoomAbout(x, y, nextZoom) {
        const pivotX = x - albumArtViewport.width / 2
        const pivotY = y - albumArtViewport.height / 2
        const ratio = nextZoom / root.zoom
        root.zoom = nextZoom
        root.panX = pivotX - (pivotX - root.panX) * ratio
        root.panY = pivotY - (pivotY - root.panY) * ratio
        root.clampPan()
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

    readonly property string infoPath: (root.fileInfo && root.fileInfo.path) || ""
    readonly property string folderPath: root.sourceKind === "folder" && root.infoPath.length > 0
        ? root.infoPath.substring(0, Math.max(1, root.infoPath.lastIndexOf("/")))
        : ""

    clip: true

    Rectangle {
        objectName: "albumArtFloor"
        anchors.fill: parent
        color: root.uiPalette.view
    }

    // Holds the text Copy Path puts on the clipboard.
    TextEdit {
        id: clipboardText
        visible: false
        textFormat: TextEdit.PlainText
    }

    Item {
        id: albumArtViewport
        objectName: "albumArtViewport"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.infoVisible ? Math.max(0, parent.width - infoColumn.width - 1) : parent.width
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

            // Shares its decoded image with albumArtViewerPreloadImage in
            // Main.qml, which must keep the same image options.
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
                    root.resetView()
                } else {
                    root.zoomAbout(mouse.x, mouse.y, 2.0)
                }
            }

            onWheel: function(wheel) {
                const notches = WheelUtils.verticalNotches(wheel, root.wheelPixelsPerNotch)
                if (notches === 0) {
                    wheel.accepted = false
                    return
                }
                const oldZoom = root.zoom
                let nextZoom = Math.max(1.0, Math.min(
                    6.0, oldZoom * Math.pow(root.zoomPerNotch, notches)))
                // Rounding leaves zooming in and back out a hair above the
                // fitted size; land on it, so the view is fitted again.
                if (nextZoom - 1.0 < 0.0001) {
                    nextZoom = 1.0
                }
                if (nextZoom === oldZoom) {
                    wheel.accepted = true
                    return
                }
                root.zoomAbout(wheel.x, wheel.y, nextZoom)
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

    // A file it cannot read: the view's own state, in place of the art.
    Column {
        objectName: "albumArtErrorState"
        anchors.centerIn: albumArtViewport
        width: Math.min(implicitWidth, albumArtViewport.width - 24)
        spacing: 6
        visible: albumArtImageFull.status === Image.Error

        Kirigami.Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 16
            height: 16
            source: "image-missing"
            isMask: true
            color: root.uiPalette.textSecondary
        }

        Label {
            objectName: "albumArtErrorText"
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(implicitWidth, parent.width)
            text: I18n.i18nc("@info", "Couldn't load this image.")
            color: root.uiPalette.textSecondary
            elide: Text.ElideMiddle
        }
    }

    Rectangle {
        objectName: "albumArtInfoSeam"
        visible: root.infoVisible
        x: infoColumn.x - 1
        width: 1
        height: parent.height
        color: root.uiPalette.seam
    }

    // The details: the file's name, then label and value rows; the folder
    // on its own line, then its path, elided in the middle, with Copy Path.
    Item {
        id: infoColumn
        objectName: "albumArtInfoColumn"
        visible: root.infoVisible
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 279

        // Presses on the details stay on them.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            onPressed: root.focusFullscreen()
            onWheel: function(wheel) { wheel.accepted = true }
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.topMargin: 12 + root.infoTopInset
            spacing: 0

            Label {
                objectName: "albumArtInfoTitle"
                Layout.fillWidth: true
                Layout.bottomMargin: 8
                text: root.sourceKind === "embedded"
                    ? I18n.i18nc("@info:title album art stored in the track's file", "Embedded Image")
                    : ((root.fileInfo && root.fileInfo.fileName)
                        || PathUtils.pathFromAnyUrl(root.viewerSource).replace(/^.*\//, ""))
                font.weight: Font.DemiBold
                color: root.uiPalette.text
                elide: Text.ElideMiddle
            }

            Repeater {
                model: [
                    { name: I18n.i18nc("@label image width and height", "Dimensions"),
                      value: root.fileInfo && root.fileInfo.width > 0 && root.fileInfo.height > 0
                          ? I18n.i18nc("@info image size, width × height", "%1 × %2 px",
                              Number(root.fileInfo.width).toLocaleString(Qt.locale(), "f", 0),
                              Number(root.fileInfo.height).toLocaleString(Qt.locale(), "f", 0))
                          : "" },
                    { name: I18n.i18nc("@label", "File size"),
                      value: (root.fileInfo && root.fileInfo.fileSizeText) || "" },
                    { name: I18n.i18nc("@label image file format", "Format"),
                      value: (root.fileInfo && (root.fileInfo.format || root.fileInfo.extension)) || "" },
                    { name: I18n.i18nc("@label where the album art is from", "Source"),
                      value: root.sourceKind === "embedded"
                          ? I18n.i18nc("@info album art source", "Embedded in the track")
                          : (root.sourceKind === "itunes"
                              ? I18n.i18nc("@info album art source", "iTunes")
                              : I18n.i18nc("@info album art source", "Folder image")) }
                ]

                delegate: RowLayout {
                    required property var modelData
                    objectName: "albumArtInfoRow"
                    Layout.fillWidth: true
                    Layout.topMargin: 5
                    Layout.bottomMargin: 5
                    visible: modelData.value.length > 0
                    spacing: 12

                    Label {
                        Layout.preferredWidth: 76
                        Layout.alignment: Qt.AlignTop
                        text: modelData.name
                        font.pointSize: root.uiPalette.smallPointSize
                        color: root.uiPalette.textSecondary
                        elide: Text.ElideRight
                    }

                    Label {
                        objectName: "albumArtInfoValue"
                        Layout.fillWidth: true
                        text: modelData.value
                        font.pointSize: root.uiPalette.smallPointSize
                        font.features: root.uiPalette.tabularFigures
                        color: root.uiPalette.text
                        wrapMode: Text.Wrap
                    }
                }
            }

            Label {
                objectName: "albumArtFolderLabel"
                visible: root.folderPath.length > 0
                Layout.topMargin: 5
                text: I18n.i18nc("@label the folder the image is in", "Folder")
                font.pointSize: root.uiPalette.smallPointSize
                color: root.uiPalette.textSecondary
            }

            RowLayout {
                visible: root.folderPath.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                spacing: 4

                Label {
                    id: folderPathLabel
                    objectName: "albumArtFolderPath"
                    Layout.fillWidth: true
                    text: root.displayPath(root.folderPath)
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.text
                    elide: Text.ElideMiddle
                    // The whole path, however it is elided.
                    ToolTip.text: root.folderPath
                    ToolTip.visible: folderPathHover.hovered
                    ToolTip.delay: Kirigami.Units.toolTipDelay

                    HoverHandler {
                        id: folderPathHover
                    }
                }

                Components.ViewerToolButton {
                    objectName: "albumArtCopyPathButton"
                    uiPalette: root.uiPalette
                    text: I18n.i18nc("@action:button", "Copy Path")
                    icon.name: "edit-copy"
                    glyphColor: root.uiPalette.textSecondary
                    Accessible.description: root.folderPath
                    onClicked: root.copyText(root.folderPath)
                }
            }

            // From the iTunes artwork dialog: the other image to compare.
            Button {
                objectName: "albumArtCompareButton"
                visible: root.comparisonModeAvailable
                Layout.topMargin: 12
                flat: true
                focusPolicy: Qt.TabFocus
                icon.name: "view-refresh"
                text: root.sourceKind === "itunes"
                    ? I18n.i18nc("@action:button", "Show Current Album Art")
                    : I18n.i18nc("@action:button artwork found on iTunes", "Show iTunes Candidate")
                ToolTip.text: I18n.i18nc("@info:tooltip", "Left or Right switches between the two images")
                ToolTip.visible: hovered || visualFocus
                ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: root.switchComparisonImage()
            }
        }
    }
}
