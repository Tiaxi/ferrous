// SPDX-License-Identifier: GPL-3.0-or-later

import QtCore
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/KeyUtils.js" as KeyUtils
import "../logic/PathUtils.js" as PathUtils
import "../logic/WheelUtils.js" as WheelUtils

// The album art viewer's view: the art on black, fitted, zoomed and
// panned; while `infoVisible`, a column of the image's details, about 40
// characters wide, 280 px with its 1 px seam at the default font, over its
// right edge. The view is a media surface, dark in both themes, as the
// spectrogram viewer is (UiPalette's mediaPlot): the art on the plot's
// floor, the details on its strip colour, in the plot's text colours.
// Over the whole screen, the details are a step larger, read from further
// away. While they show, the art fits the space left of their seam, centred
// there (#373 H8: it stayed centred in the whole view, off-centre in what
// the details leave of it), and keeps its zoom and pan.
// During a preview from Replace from iTunes, the details go on with what the
// preview shows and its buttons, as a dialog's: Back to Results and, for a
// candidate, Apply. The art then keeps 24 px clear of the view's edge and
// of the details' seam.
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
    // Whether the viewer is over the whole screen: the details' text is
    // then 1.2 times the body size, 16 px with the default 10 pt font, the
    // file's name 18 px, and their column 320 px with its seam.
    property bool fullscreen: false
    // The colours of the view, dark in both themes.
    readonly property var plot: root.uiPalette.mediaPlot
    readonly property real infoScale: root.fullscreen ? 1.2 : 1
    readonly property real infoPointSize: root.uiPalette.basePointSize * root.infoScale
    // The glyph of a button in the details: 16 px, a step larger with
    // the larger text over the whole screen.
    readonly property int detailsIconSize: root.uiPalette.scaledSize(
        Math.round(Kirigami.Units.iconSizes.small * root.infoScale))
    // The file's name: the body size; during a preview, a step larger, 12 pt
    // with the default 10 pt font; over the whole screen, 18 px with it.
    readonly property real infoTitlePointSize: root.uiPalette.basePointSize
        * (root.fullscreen ? 1.35 : (root.previewActive ? 1.2 : 1))
    // The image's details, from BridgeClient.cachedImageFileDetails: path,
    // fileName, width, height, fileSizeText, format, embeddedCover.
    property var fileInfo: ({})
    // Where the image is from: "folder", "embedded" or "itunes".
    property string sourceKind: "folder"
    property var switchComparisonImage: function() {}
    // How far the details keep below the top, clear of controls over them.
    property real infoTopInset: 0
    // A preview from Replace from iTunes: what it shows, such as "iTunes
    // Candidate", and Back to Results, which closeViewer does then. With a
    // candidate, Apply calls applyPreview while previewCanApply.
    property bool previewActive: false
    property string previewLabel: ""
    property bool previewHasCandidate: false
    property bool previewCanApply: false
    property var applyPreview: null
    readonly property alias imageStatus: albumArtImageFull.status
    readonly property bool zoomed: root.zoom > 1.0
    // The space the art fits in, across: the whole view, or while the
    // details show the space left of their seam, during a preview less 24 px
    // on each side.
    readonly property real artMargin: root.previewActive && root.infoVisible ? 24 : 0
    readonly property real fitLeft: root.artMargin
    readonly property real fitWidth: root.infoVisible
        ? Math.max(1, infoColumn.x - 1 - 2 * root.artMargin)
        : albumArtViewport.width
    readonly property real fitCenterX: root.fitLeft + root.fitWidth / 2
    onFitWidthChanged: root.clampPan()

    // A push button of the details, reached with Tab, with its tooltip.
    // Space presses it rather than playing or pausing, which the viewer's
    // shortcuts would take; keys with modifiers stay the shortcuts'.
    // It is drawn in the plot's colours, as the strip's buttons are: the
    // style's button, and the theme's icons, would be a light theme's on
    // the dark details. Raised, it is a lift of the plate with its edge;
    // flat, only the hover tint; the default one, `isDefault`, is filled
    // with the plot's checked fill. Keyboard focus is the plot's 2 px ring
    // 1 px outside it, with a 1 px gap in the details' colour.
    component DetailsButton: Button {
        id: detailsButton

        property string toolTipText: ""
        property bool isDefault: false
        readonly property bool filled: detailsButton.isDefault && detailsButton.enabled
        readonly property color contentColor: detailsButton.filled
            ? root.plot.checkedGlyph
            : root.plot.plotText

        focusPolicy: Qt.TabFocus
        hoverEnabled: true
        font.pointSize: root.infoPointSize
        leftPadding: Math.round(8 * root.infoScale)
        rightPadding: Math.round(8 * root.infoScale)
        topPadding: Math.round(5 * root.infoScale)
        bottomPadding: Math.round(5 * root.infoScale)
        spacing: Kirigami.Units.smallSpacing
        icon.width: root.detailsIconSize
        icon.height: root.detailsIconSize
        // The focus ring reaches past the button, over the next one's gap.
        z: detailsButton.visualFocus ? 1 : 0
        Accessible.defaultButton: detailsButton.isDefault
        Keys.onShortcutOverride: function(event) {
            event.accepted = KeyUtils.isPlainSpace(event)
        }
        ToolTip.text: toolTipText
        ToolTip.visible: (hovered || visualFocus) && toolTipText.length > 0
        ToolTip.delay: Kirigami.Units.toolTipDelay

        // Centred, however wide the button is, as stacked buttons are; the
        // text is elided if the button is narrower than it.
        // Its size asks for the whole label, whatever the label's width.
        contentItem: Item {
            implicitWidth: (detailsButtonIcon.visible ? detailsButtonIcon.width + detailsButton.spacing : 0)
                + detailsButtonLabel.implicitWidth
            implicitHeight: Math.max(detailsButtonIcon.visible ? detailsButtonIcon.height : 0,
                detailsButtonLabel.implicitHeight)
            opacity: detailsButton.enabled ? 1 : 0.5

            Row {
                id: detailsButtonContent
                anchors.centerIn: parent
                spacing: detailsButton.spacing

                Kirigami.Icon {
                    id: detailsButtonIcon
                    objectName: "albumArtDetailsButtonIcon"
                    anchors.verticalCenter: parent.verticalCenter
                    width: detailsButton.icon.width
                    height: detailsButton.icon.height
                    visible: detailsButton.icon.name.length > 0
                    source: detailsButton.icon.name
                    color: detailsButton.contentColor
                    isMask: true
                }

                Label {
                    id: detailsButtonLabel
                    objectName: "albumArtDetailsButtonLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, detailsButton.availableWidth
                        - (detailsButtonIcon.visible ? detailsButtonIcon.width + detailsButton.spacing : 0))
                    text: detailsButton.text
                    font: detailsButton.font
                    color: detailsButton.contentColor
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }
            }
        }

        background: Item {
            Rectangle {
                objectName: "albumArtDetailsButtonFill"
                anchors.fill: parent
                radius: 3
                opacity: detailsButton.enabled ? 1 : 0.5
                color: detailsButton.isDefault
                    ? root.plot.checkedFill
                    : (detailsButton.flat ? "transparent" : ColorUtils.withAlpha(root.plot.plotText, 0.10))
                border.width: detailsButton.isDefault || detailsButton.flat ? 0 : 1
                border.color: root.plot.plotPlateEdge
            }

            Rectangle {
                objectName: "albumArtDetailsButtonHover"
                anchors.fill: parent
                radius: 3
                color: ColorUtils.withAlpha(root.plot.plotText,
                    detailsButton.down ? 0.18 : 0.12)
                opacity: detailsButton.enabled && (detailsButton.hovered || detailsButton.down) ? 1 : 0
                visible: opacity > 0

                Behavior on opacity {
                    enabled: Kirigami.Units.shortDuration > 0
                    NumberAnimation {
                        duration: Kirigami.Units.shortDuration
                        easing.type: Easing.OutCubic
                    }
                }
            }

            // The gap between the button and its focus ring: the details'.
            Rectangle {
                objectName: "albumArtDetailsButtonFocusGap"
                anchors.fill: parent
                anchors.margins: -1
                radius: 4
                visible: detailsButton.visualFocus
                color: "transparent"
                border.width: 1
                border.color: root.plot.plotStrip
            }

            Rectangle {
                objectName: "albumArtDetailsButtonFocusRing"
                anchors.fill: parent
                anchors.margins: -3
                radius: 6
                visible: detailsButton.visualFocus
                color: "transparent"
                border.width: 2
                border.color: root.plot.plotFocusRing
            }
        }
    }

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
        const limitX = Math.max(0, (scaledW - root.fitWidth) / 2)
        const limitY = Math.max(0, (scaledH - albumArtViewport.height) / 2)
        root.panX = Math.max(-limitX, Math.min(limitX, root.panX))
        root.panY = Math.max(-limitY, Math.min(limitY, root.panY))
    }

    // Zooms to `nextZoom` keeping the image point at (`x`, `y`) in the
    // viewport under that point, as far as the pan limits allow. The image
    // is centred in the space it fits in, offset by the pan and scaled about
    // its centre, so the point at `pivot = (x, y) - that space's centre`
    // shows the image point `(pivot - pan) / zoom` from the image's centre.
    function zoomAbout(x, y, nextZoom) {
        const pivotX = x - root.fitCenterX
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

    // Black in both themes, as the spectrogram's floor.
    Rectangle {
        objectName: "albumArtFloor"
        anchors.fill: parent
        color: root.plot.plotBg
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
        anchors.fill: parent
        clip: true
        onWidthChanged: root.applyInitialView()
        onHeightChanged: root.applyInitialView()

        Item {
            id: albumArtTransform
            // The art's shape: its decoded size's, or the view's until it
            // has loaded. The decode request is a size to fit in, not the
            // art's size, so it says nothing of its shape.
            readonly property real aspect: albumArtImageFull.implicitWidth > 0
                    && albumArtImageFull.implicitHeight > 0
                ? albumArtImageFull.implicitWidth / albumArtImageFull.implicitHeight
                : Math.max(1, root.fitWidth) / Math.max(1, albumArtViewport.height)

            // Fitted to its space at zoom 1, enlarged if it is smaller.
            width: Math.max(1, Math.min(root.fitWidth, albumArtViewport.height * aspect))
            height: Math.max(1, width / aspect)
            x: root.fitLeft + (root.fitWidth - width) / 2 + root.panX
            y: (albumArtViewport.height - height) / 2 + root.panY
            scale: root.zoom
            transformOrigin: Item.Center

            // Shares its decoded image with albumArtViewerPreloadImage in
            // Main.qml, which must keep the same image options.
            Image {
                id: albumArtImageFull
                objectName: "albumArtImage"
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
            objectName: "albumArtViewerContextMenu"

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
            color: root.plot.plotTextSecondary
        }

        Label {
            objectName: "albumArtErrorText"
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(implicitWidth, parent.width)
            text: I18n.i18nc("@info", "Couldn't load this image.")
            color: root.plot.plotTextSecondary
            elide: Text.ElideMiddle
        }
    }

    Rectangle {
        objectName: "albumArtInfoSeam"
        visible: root.infoVisible
        x: infoColumn.x - 1
        width: 1
        height: parent.height
        // The plates' edge on the floor, solid, over any art.
        color: Qt.tint(root.plot.plotBg, root.plot.plotPlateEdge)
    }

    // The details: the file's name, then label and value rows at the body
    // size; the folder on its own line, then its path, elided in the
    // middle, with Copy Path. The rows' labels share a column as wide as the
    // widest of them, up to two fifths of the row, so every value starts at
    // the same x and keeps the rest; a label wider than that wraps between
    // its words, and the column is never narrower than its widest word.
    // The column is about 40 characters wide, 280 px with its seam at the
    // default font, up to two fifths of the view, and never under 280 px;
    // over the whole screen, a seventh wider, 320 px, for its larger text,
    // which the column's room to spare at the desktop size takes the rest
    // of.
    // During a preview, it is wider for its buttons if they need it, up to
    // half the view: in a narrower view, the buttons stack, so neither is
    // cut short.
    Item {
        id: infoColumn
        objectName: "albumArtInfoColumn"
        readonly property real widthScale: root.fullscreen ? 8 / 7 : 1
        // Without its seam: 279 px, or 319 px over the whole screen.
        readonly property real minimumWidth: Math.round(280 * infoColumn.widthScale) - 1
        readonly property real fortyCharacters: Math.max(infoColumn.minimumWidth,
            Math.ceil(infoFontMetrics.averageCharacterWidth * 40 * infoColumn.widthScale) - 1)
        visible: root.infoVisible
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.previewActive
            ? Math.max(infoColumn.minimumWidth, Math.min(Math.floor(root.width / 2), Math.max(infoColumn.fortyCharacters,
                Math.ceil(Math.max(previewBackButton.implicitWidth,
                    previewApplyButton.visible ? previewApplyButton.implicitWidth : 0)) + 32)))
            : Math.max(infoColumn.minimumWidth, Math.min(Math.floor(root.width * 0.4), infoColumn.fortyCharacters))

        FontMetrics {
            id: infoFontMetrics
            font: root.uiPalette.baseFont
        }

        // Opaque, so the details read the same over any art: the plot's
        // strip colour, as the title strip's over the whole screen.
        Rectangle {
            objectName: "albumArtInfoBacking"
            anchors.fill: parent
            color: root.plot.plotStrip
        }

        // Presses, the wheel and the pointer's hover on the details and
        // their seam stay on them, off the art's pan and zoom below.
        MouseArea {
            objectName: "albumArtInfoInputArea"
            anchors.fill: parent
            anchors.leftMargin: -1
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            onPressed: root.focusFullscreen()
            onWheel: function(wheel) { wheel.accepted = true }
        }

        ColumnLayout {
            id: infoLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.topMargin: 12 + root.infoTopInset
            spacing: 0

            // During a preview, what it shows, such as "iTunes Candidate",
            // over the file's name.
            Label {
                objectName: "albumArtPreviewLabel"
                Layout.fillWidth: true
                Layout.bottomMargin: 2
                visible: root.previewActive && text.length > 0
                text: root.previewLabel
                font.pointSize: root.infoPointSize
                color: root.plot.plotTextSecondary
                elide: Text.ElideRight
            }

            readonly property real labelColumnWidth: Math.max(Math.ceil(infoRows.widestWord),
                Math.min(Math.ceil(infoRows.widestLabel), Math.floor(infoLayout.width * 0.4)))

            Label {
                objectName: "albumArtInfoTitle"
                Layout.fillWidth: true
                Layout.bottomMargin: Math.round(8 * root.infoScale)
                text: root.sourceKind === "embedded"
                    ? I18n.i18nc("@info:title album art stored in the track's file", "Embedded Image")
                    : ((root.fileInfo && root.fileInfo.fileName)
                        || PathUtils.pathFromAnyUrl(root.viewerSource).replace(/^.*\//, ""))
                font.pointSize: root.infoTitlePointSize
                font.weight: Font.DemiBold
                color: root.plot.plotText
                elide: Text.ElideMiddle
            }

            Repeater {
                id: infoRows
                // Of every row, shown or not, so the values stay where they
                // are when a row hides for another image.
                readonly property real widestLabel: {
                    let widest = 0
                    for (let i = 0; i < infoRows.count; ++i) {
                        const row = infoRows.itemAt(i)
                        if (row) {
                            widest = Math.max(widest, row.labelImplicitWidth)
                        }
                    }
                    return widest
                }
                // The widest single word of any label: a label wraps only
                // between its words.
                readonly property real widestWord: {
                    let widest = 0
                    for (let i = 0; i < infoRows.count; ++i) {
                        const row = infoRows.itemAt(i)
                        if (row) {
                            widest = Math.max(widest, row.labelWidestWord)
                        }
                    }
                    return widest
                }
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
                    readonly property real labelImplicitWidth: infoLabel.implicitWidth
                    readonly property real labelWidestWord: {
                        let widest = 0
                        for (const word of infoLabel.text.split(/\s+/)) {
                            widest = Math.max(widest, labelMetrics.advanceWidth(word))
                        }
                        return widest
                    }
                    objectName: "albumArtInfoRow"
                    Layout.fillWidth: true
                    Layout.topMargin: Math.round(5 * root.infoScale)
                    Layout.bottomMargin: Math.round(5 * root.infoScale)
                    visible: modelData.value.length > 0
                    spacing: 12

                    Label {
                        id: infoLabel
                        objectName: "albumArtInfoLabel"
                        // Exactly the column's width, however long the value.
                        Layout.minimumWidth: infoLayout.labelColumnWidth
                        Layout.preferredWidth: infoLayout.labelColumnWidth
                        Layout.maximumWidth: infoLayout.labelColumnWidth
                        Layout.alignment: Qt.AlignTop
                        text: modelData.name
                        font.pointSize: root.infoPointSize
                        color: root.plot.plotTextSecondary
                        wrapMode: Text.WordWrap

                        FontMetrics {
                            id: labelMetrics
                            font: infoLabel.font
                        }
                    }

                    Label {
                        objectName: "albumArtInfoValue"
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        text: modelData.value
                        font.pointSize: root.infoPointSize
                        font.features: root.uiPalette.tabularFigures
                        color: root.plot.plotText
                        wrapMode: Text.Wrap
                    }
                }
            }

            Label {
                objectName: "albumArtFolderLabel"
                visible: root.folderPath.length > 0
                Layout.topMargin: Math.round(5 * root.infoScale)
                text: I18n.i18nc("@label the folder the image is in", "Folder")
                font.pointSize: root.infoPointSize
                color: root.plot.plotTextSecondary
            }

            RowLayout {
                visible: root.folderPath.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(root.uiPalette.toolButtonSize, folderPathLabel.implicitHeight)
                spacing: 4

                Label {
                    id: folderPathLabel
                    objectName: "albumArtFolderPath"
                    Layout.fillWidth: true
                    text: root.displayPath(root.folderPath)
                    font.pointSize: root.infoPointSize
                    color: root.plot.plotText
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
                    plotColors: root.plot
                    text: I18n.i18nc("@action:button", "Copy Path")
                    icon.name: "edit-copy"
                    glyphColor: root.plot.plotTextSecondary
                    Accessible.description: root.folderPath
                    onClicked: root.copyText(root.folderPath)
                }
            }

            // From the iTunes artwork dialog: the other image to compare.
            DetailsButton {
                objectName: "albumArtCompareButton"
                visible: root.comparisonModeAvailable
                Layout.topMargin: Math.round(12 * root.infoScale)
                flat: true
                icon.name: "view-refresh"
                text: root.sourceKind === "itunes"
                    ? I18n.i18nc("@action:button", "Show Current Album Art")
                    : I18n.i18nc("@action:button artwork found on iTunes", "Show iTunes Candidate")
                toolTipText: I18n.i18nc("@info:tooltip", "Left or Right switches between the two images")
                onClicked: root.switchComparisonImage()
            }
        }

        // The preview's buttons, as a dialog's, 12 px under the details: at
        // the end of a row, the default one, Apply, last, tinted as the
        // default button. When they do not
        // fit side by side, with a larger font or a longer translation, they
        // stack, each the column's width, Back to Results first.
        // Anchors and bindings place them, so they are in place the moment
        // Apply shows or hides: a layout would place them only at its next
        // polish, leaving Apply where it was laid out before, past the
        // column's end, until then.
        Item {
            id: previewFooter
            objectName: "albumArtPreviewFooter"
            visible: root.previewActive
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: infoLayout.bottom
            anchors.topMargin: 12
            height: previewButtons.y + previewButtons.height

            Item {
                id: previewButtons
                objectName: "albumArtPreviewButtons"
                readonly property real spacing: Kirigami.Units.smallSpacing
                readonly property bool oneRow: previewBackButton.implicitWidth
                    + (previewApplyButton.visible
                        ? spacing + previewApplyButton.implicitWidth : 0) <= width
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                height: oneRow
                    ? Math.max(previewBackButton.height,
                        previewApplyButton.visible ? previewApplyButton.height : 0)
                    : previewBackButton.height + (previewApplyButton.visible
                        ? spacing + previewApplyButton.height : 0)

                DetailsButton {
                    id: previewBackButton
                    objectName: "albumArtPreviewBackButton"
                    x: previewButtons.oneRow
                        ? (previewApplyButton.visible
                            ? previewApplyButton.x - previewButtons.spacing
                            : previewButtons.width) - width
                        : 0
                    y: previewButtons.oneRow ? (previewButtons.height - height) / 2 : 0
                    width: previewButtons.oneRow ? implicitWidth : previewButtons.width
                    icon.name: "go-previous"
                    text: I18n.i18nc("@action:button", "Back to Results")
                    toolTipText: I18n.i18nc("@info:tooltip %1 is the key", "Back to Results (%1)", "Esc")
                    onClicked: root.closeViewer()
                }

                DetailsButton {
                    id: previewApplyButton
                    objectName: "albumArtPreviewApplyButton"
                    x: previewButtons.oneRow ? previewButtons.width - width : 0
                    y: previewButtons.oneRow
                        ? (previewButtons.height - height) / 2
                        : previewBackButton.height + previewButtons.spacing
                    width: previewButtons.oneRow ? implicitWidth : previewButtons.width
                    visible: root.previewHasCandidate
                    enabled: root.previewCanApply
                    isDefault: true
                    icon.name: "dialog-ok-apply"
                    text: I18n.i18nc("@action:button", "Apply")
                    toolTipText: I18n.i18nc("@info:tooltip", "Replace the album art with this candidate")
                    onClicked: root.applyPreview()
                }
            }
        }
    }
}
