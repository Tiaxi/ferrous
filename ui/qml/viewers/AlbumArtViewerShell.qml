import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "../components" as Components

Item {
    id: root

    required property var windowRoot
    required property bool viewerOpen
    required property bool useWholeScreenViewerMode
    required property int popupTransitionMs
    required property string titleText
    required property var closeViewer
    required property var toggleInfoVisible
    required property var switchComparisonImage

    property alias popupHost: albumArtPopupHost
    property alias windowHost: albumArtWindowHost
    readonly property real popupWidth: albumArtViewer.width
    readonly property real popupHeight: albumArtViewer.height
    readonly property real wholeScreenWidth: albumArtFullscreenWindow.width
    readonly property real wholeScreenHeight: albumArtFullscreenWindow.height
    readonly property bool wholeScreenVisible: albumArtFullscreenWindow.visibility === Window.FullScreen

    onViewerOpenChanged: Qt.callLater(root.syncPresentation)
    onUseWholeScreenViewerModeChanged: {
        if (root.viewerOpen) {
            Qt.callLater(root.syncPresentation)
        }
    }

    function syncPresentation() {
        if (root.viewerOpen && !root.useWholeScreenViewerMode) {
            if (!albumArtViewer.visible) {
                albumArtViewer.open()
            }
        } else if (albumArtViewer.visible) {
            albumArtViewer.close()
        }
        if (root.viewerOpen && root.useWholeScreenViewerMode) {
            albumArtFullscreenWindow.requestActivate()
            root.focusFullscreen()
        }
    }

    function focusFullscreen() {
        if (root.wholeScreenVisible) {
            albumArtFullscreenFocusSink.forceActiveFocus()
        }
    }

    Component.onCompleted: Qt.callLater(root.syncPresentation)

    Popup {
        id: albumArtViewer
        parent: Overlay.overlay
        x: 0
        y: 0
        width: root.windowRoot.width
        height: root.windowRoot.height
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape
        visible: root.viewerOpen && !root.useWholeScreenViewerMode
        enter: Components.PopupTransition { duration: root.popupTransitionMs }
        exit: Components.PopupTransition { duration: root.popupTransitionMs }

        Shortcut {
            sequence: "I"
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.toggleInfoVisible()
        }

        Shortcut {
            sequence: "Left"
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.switchComparisonImage()
        }

        Shortcut {
            sequence: "Right"
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.switchComparisonImage()
        }

        onClosed: {
            if (root.viewerOpen && !root.useWholeScreenViewerMode) {
                root.closeViewer()
            }
        }

        background: Rectangle {
            color: "#000000"
            opacity: 0.87
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onClicked: root.closeViewer()
        }

        Components.ViewerCloseButton {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            fillColor: Qt.rgba(1, 1, 1, 0.16)
            borderColor: Qt.rgba(1, 1, 1, 0.52)
            onClicked: root.closeViewer()
        }

        Item {
            id: albumArtPopupHost
            anchors.fill: parent
        }
    }

    Window {
        id: albumArtFullscreenWindow
        screen: root.windowRoot.screen
        transientParent: root.windowRoot
        modality: Qt.ApplicationModal
        flags: Qt.Window | Qt.FramelessWindowHint
        visibility: root.viewerOpen && root.useWholeScreenViewerMode
            ? Window.FullScreen
            : Window.Hidden
        color: "#000000"
        title: root.titleText

        onVisibilityChanged: function() {
            if (albumArtFullscreenWindow.visibility === Window.FullScreen) {
                requestActivate()
                root.focusFullscreen()
            }
        }

        onClosing: function(close) {
            if (root.viewerOpen && root.useWholeScreenViewerMode) {
                root.closeViewer()
            }
        }

        FocusScope {
            id: albumArtFullscreenFocusSink
            anchors.fill: parent
            focus: albumArtFullscreenWindow.visibility === Window.FullScreen

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    root.closeViewer()
                } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                    event.accepted = true
                    root.switchComparisonImage()
                }
            }
        }

        Shortcut {
            sequence: "I"
            context: Qt.WindowShortcut
            enabled: albumArtFullscreenWindow.visibility === Window.FullScreen
            onActivated: root.toggleInfoVisible()
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: root.focusFullscreen()
            onClicked: root.closeViewer()
        }

        Components.ViewerCloseButton {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            fillColor: Qt.rgba(1, 1, 1, 0.16)
            borderColor: Qt.rgba(1, 1, 1, 0.52)
            onClicked: root.closeViewer()
        }

        Item {
            anchors.fill: parent

            Item {
                id: albumArtWindowHost
                anchors.fill: parent
            }
        }
    }
}
