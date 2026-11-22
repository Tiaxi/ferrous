// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// About Ferrous, laid out as KDE's About dialog: the icon, name and
// version, then the About, Components, Authors and License tabs, in a window
// of its own, as KAboutApplicationDialog.
Components.DialogWindow {
    id: root

    readonly property string version: typeof appVersion !== "undefined" ? String(appVersion) : ""
    // The libraries Ferrous runs on, as main.cpp gives them: each a name and
    // a version.
    readonly property var components: typeof appComponents !== "undefined" ? appComponents : []
    readonly property string homepageUrl: "https://github.com/Tiaxi/ferrous"
    readonly property string bugReportUrl: "https://github.com/Tiaxi/ferrous/issues"
    readonly property string licenseUrl: "https://www.gnu.org/licenses/gpl-3.0.html"
    property int tabIndex: 0

    // The version information a bug report needs, one line each.
    function versionInformation() {
        const lines = [I18n.i18nc("@info %1 is the version", "Ferrous %1", root.version)]
        for (let i = 0; i < root.components.length; ++i) {
            const component = root.components[i]
            lines.push(I18n.i18nc("@info component name, version", "%1 %2", component.name, component.version))
        }
        return lines.join("\n")
    }

    function copyVersionInformation() {
        clipboardText.text = root.versionInformation()
        clipboardText.selectAll()
        clipboardText.copy()
        clipboardText.text = ""
    }

    objectName: "aboutDialog"
    title: I18n.i18nc("@title:window", "About Ferrous")
    // The content's inset from the window's edges.
    readonly property real contentPadding: Kirigami.Units.gridUnit
    // The pages' inset within their frame.
    readonly property real pageInset: Kirigami.Units.largeSpacing
    // The room between the pages' frame and the buttons.
    readonly property real pageBottomGap: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    // The pages' width, from the window's: their text wraps to it, so their
    // height is known as soon as the window's width is, before it shows.
    readonly property real pageWidth: Math.max(0, root.width - 2 * root.contentPadding - 2 * root.pageInset)
    // As wide as the tabs, the Components page, the links and the buttons
    // need; the texts wrap within it.
    readonly property real contentMinimumWidth: Math.ceil(Math.max(
        aboutTabs.implicitWidth + 2 * root.contentPadding,
        Math.max(root.componentsWidth, aboutLinks.implicitWidth) + 2 * root.pageInset + 2 * root.contentPadding,
        aboutFooter.implicitWidth))
    // The Components page: the names' column, 9 grid units or as wide as
    // the longest name, and the versions after it.
    readonly property real componentNameWidth: {
        let widest = 0
        for (let i = 0; i < root.components.length; ++i) {
            widest = Math.max(widest, componentMetrics.advanceWidth(String(root.components[i].name)))
        }
        return Math.max(root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 9), Math.ceil(widest))
    }
    readonly property real componentsWidth: {
        let widest = 0
        for (let i = 0; i < root.components.length; ++i) {
            widest = Math.max(widest, componentMetrics.advanceWidth(String(root.components[i].version)))
        }
        return root.componentNameWidth + Kirigami.Units.largeSpacing + Math.ceil(widest)
    }
    FontMetrics {
        id: componentMetrics
        font: root.uiPalette.baseFont
    }
    // The heading, the tabs, the pages' frame and the room around it.
    readonly property real chromeHeight: 2 * root.contentPadding + aboutHeading.implicitHeight
        + aboutTabs.Layout.topMargin + aboutTabs.implicitHeight + aboutPagesFrame.Layout.topMargin
        + aboutPagesFrame.topPadding + aboutPagesFrame.bottomPadding + root.pageBottomGap
        + aboutFooter.implicitHeight
    minimumWidth: root.contentMinimumWidth
    // The heading, the tabs and three lines of a page, which scrolls.
    minimumHeight: Math.ceil(root.chromeHeight + 3 * componentMetrics.height)
    // 29 grid units with the default font, as much wider as a larger font
    // makes the text, or wider when the tabs or the buttons need more; as tall as the tallest page at that width, within
    // the screen, where the pages scroll.
    defaultWidth: Math.max(root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 29), root.contentMinimumWidth)
    defaultHeight: Math.ceil(root.chromeHeight + aboutPages.implicitHeight)

    onAboutToShow: {
        root.tabIndex = 0
        aboutTabs.currentItem.forceActiveFocus(Qt.PopupFocusReason)
    }

    // The arrow keys move between the tabs, as in KDE's About dialog.
    function stepTab(step) {
        root.tabIndex = (root.tabIndex + step + aboutTabs.count) % aboutTabs.count
        aboutTabs.currentItem.forceActiveFocus(Qt.TabFocusReason)
    }

    component AboutTab: TabButton {
        width: implicitWidth
        Keys.onLeftPressed: root.stepTab(-1)
        Keys.onRightPressed: root.stepTab(1)
    }

    // A text field that only the clipboard sees.
    TextEdit {
        id: clipboardText
        textFormat: TextEdit.PlainText
        visible: false
    }

    footer: RowLayout {
        id: aboutFooter
        spacing: 0

        Button {
            objectName: "aboutCopyVersionButton"
            Layout.leftMargin: dialogButtons.inset
            Layout.bottomMargin: dialogButtons.bottomPadding
            icon.width: dialogButtons.glyphSize
            icon.height: dialogButtons.glyphSize
            icon.name: "edit-copy"
            text: I18n.i18nc("@action:button", "Copy Version Information")
            onClicked: root.copyVersionInformation()
        }

        Components.DialogButtons {

            id: dialogButtons
            primary: aboutCloseButton
            Layout.fillWidth: true

            Button {
                id: aboutCloseButton
                objectName: "aboutCloseButton"
                text: I18n.i18nc("@action:button", "Close")
                icon.name: "window-close"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: root.reject()
            }
        }
    }

    ColumnLayout {
        id: aboutContent
        anchors.fill: parent
        anchors.margins: root.contentPadding
        // The pages' frame ends this far above the buttons.
        anchors.bottomMargin: root.contentPadding + root.pageBottomGap

        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, root.visible) }

        spacing: 0

        RowLayout {
            id: aboutHeading
            Layout.leftMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.gridUnit - 2

            Kirigami.Icon {
                objectName: "aboutIcon"
                // 48 px with the default font, and larger with a larger
                // one, beside the name and version it heads.
                implicitWidth: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.large)
                implicitHeight: root.uiPalette.scaledSize(Kirigami.Units.iconSizes.large)
                // The built-in drawing, which shows whether the icon is
                // installed or not.
                source: "qrc:/icons/hicolor/48x48/apps/ferrous.svg"
                fallback: "ferrous"
                Accessible.ignored: true
            }

            ColumnLayout {
                spacing: 0

                Label {
                    objectName: "aboutName"
                    textFormat: Text.PlainText
                    text: "Ferrous"
                    font.pointSize: root.uiPalette.basePointSize * 1.125
                    font.weight: Font.DemiBold
                    color: root.uiPalette.text
                    Accessible.role: Accessible.Heading
                }

                Label {
                    objectName: "aboutVersion"
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@info", "Version %1", root.version)
                    font.features: root.uiPalette.tabularFigures
                    color: root.uiPalette.textSecondary
                }
            }
        }

        TabBar {
            id: aboutTabs
            objectName: "aboutTabs"
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing + 2
            currentIndex: root.tabIndex
            onCurrentIndexChanged: root.tabIndex = currentIndex

            AboutTab { text: I18n.i18nc("@title:tab", "About") }
            AboutTab { text: I18n.i18nc("@title:tab", "Components") }
            AboutTab { text: I18n.i18nc("@title:tab", "Authors") }
            AboutTab { text: I18n.i18nc("@title:tab", "License") }
        }

        // The pages on a frame joined to the tabs, as KDE's About dialog,
        // a tab widget, draws them (the tabs' band stopped
        // mid-window, with no page under it): the frame's top edge runs
        // along the tabs' foot and opens under the selected tab, which
        // stands on it; under the first tab, its corner is square.
        Frame {
            id: aboutPagesFrame
            objectName: "aboutPagesFrame"
            // The frame's edge, as the style draws it.
            readonly property color edgeColor: Kirigami.ColorUtils.linearInterpolation(
                Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, Kirigami.Theme.frameContrast)
            // Where the selected tab stands on the frame.
            readonly property real tabLeft: aboutTabs.leftPadding + aboutTabs.currentItem.x
            readonly property real tabWidth: aboutTabs.currentItem.width
            Layout.fillWidth: true
            Layout.fillHeight: true
            // The top edge on the tabs' last row of pixels.
            Layout.topMargin: -1
            padding: root.pageInset

            // The opening under the selected tab, between its sides.
            Rectangle {
                objectName: "aboutPagesFrameOpening"
                parent: aboutPagesFrame
                x: aboutPagesFrame.tabLeft + 1
                width: Math.max(0, aboutPagesFrame.tabWidth - 2)
                height: 1
                color: root.uiPalette.chrome
            }

            // The square corners under a tab at either end: the tab's side
            // runs on down the frame's.
            component SquareCorner: Item {
                property bool atRight: false
                parent: aboutPagesFrame
                x: atRight ? aboutPagesFrame.width - width : 0
                width: Kirigami.Units.cornerRadius + 1
                height: Kirigami.Units.cornerRadius + 1

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: parent.atRight ? 0 : 1
                    anchors.rightMargin: parent.atRight ? 1 : 0
                    color: root.uiPalette.chrome
                }

                Rectangle {
                    x: parent.atRight ? parent.width - 1 : 0
                    width: 1
                    height: parent.height
                    color: aboutPagesFrame.edgeColor
                }
            }

            SquareCorner {
                objectName: "aboutPagesFrameLeftCorner"
                visible: aboutPagesFrame.tabLeft < 1
            }

            SquareCorner {
                objectName: "aboutPagesFrameRightCorner"
                atRight: true
                visible: aboutPagesFrame.tabLeft + aboutPagesFrame.tabWidth > aboutPagesFrame.width - 1
            }

            // The pages, in the room the window leaves them: they scroll
            // when a smaller window or a large font leaves less than they
            // need, and never run under the buttons.
            Flickable {
                id: aboutPagesView
                objectName: "aboutPagesView"
                anchors.fill: parent
                implicitHeight: aboutPages.implicitHeight
                contentWidth: width
                contentHeight: aboutPages.implicitHeight
                clip: true
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    objectName: "aboutPagesScrollBar"
                    policy: aboutPagesView.interactive ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }

                // Each page lays its lines out top to bottom by anchors, which
                // places them at once: its height is known before the window
                // shows, so the window opens tall enough for it. As tall as the
                // tallest page, so the window keeps its height between tabs.
                Item {
                    id: aboutPages
                    objectName: "aboutPages"
                    readonly property int currentIndex: root.tabIndex
                    width: root.pageWidth
                    implicitHeight: Math.max(aboutPage.implicitHeight, componentsPage.implicitHeight,
                        authorsPage.implicitHeight, licensePage.implicitHeight)
                    height: implicitHeight

                    Item {
                        id: aboutPage
                        objectName: "aboutPageAbout"
                        width: parent.width
                        visible: aboutPages.currentIndex === 0
                        implicitHeight: aboutLinks.y + aboutLinks.implicitHeight

                        Label {
                            id: aboutDescription
                            objectName: "aboutDescription"
                            width: parent.width
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: I18n.i18nc("@info", "A music player for KDE Plasma with live spectrogram and waveform views.")
                        }

                        Label {
                            id: aboutCopyright
                            objectName: "aboutCopyright"
                            anchors.top: aboutDescription.bottom
                            anchors.topMargin: Kirigami.Units.largeSpacing + 2
                            width: parent.width
                            wrapMode: Text.Wrap
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.textSecondary
                            textFormat: Text.PlainText
                            text: I18n.i18nc("@info", "© 2026 The Ferrous authors · GNU GPL, version 3 or later")
                        }

                        RowLayout {
                            id: aboutLinks
                            objectName: "aboutLinks"
                            anchors.top: aboutCopyright.bottom
                            anchors.topMargin: Kirigami.Units.largeSpacing + 2
                            spacing: Kirigami.Units.gridUnit - 2

                            Kirigami.UrlButton {
                                objectName: "aboutHomepageLink"
                                url: root.homepageUrl
                                text: I18n.i18nc("@action:button", "Homepage")
                                color: root.uiPalette.link
                            }

                            Kirigami.UrlButton {
                                objectName: "aboutBugReportLink"
                                url: root.bugReportUrl
                                text: I18n.i18nc("@action:button", "Report a Bug")
                                color: root.uiPalette.link
                            }
                        }
                    }

                    // A line for each library: its name, and its version.
                    Item {
                        id: componentsPage
                        objectName: "aboutPageComponents"
                        readonly property real rowHeight: Math.ceil(componentMetrics.height)
                        readonly property real rowSpacing: Kirigami.Units.smallSpacing
                        width: parent.width
                        visible: aboutPages.currentIndex === 1
                        implicitHeight: root.components.length > 0
                            ? root.components.length * (rowHeight + rowSpacing) - rowSpacing
                            : 0

                        Repeater {
                            model: root.components

                            delegate: Item {
                                required property var modelData
                                required property int index
                                y: index * (componentsPage.rowHeight + componentsPage.rowSpacing)
                                width: componentsPage.width
                                height: componentsPage.rowHeight

                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: root.componentNameWidth
                                    textFormat: Text.PlainText
                                    text: modelData.name
                                    elide: Text.ElideRight
                                }

                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: root.componentNameWidth + Kirigami.Units.largeSpacing
                                    width: Math.max(0, parent.width - x)
                                    textFormat: Text.PlainText
                                    text: modelData.version
                                    elide: Text.ElideRight
                                    font.features: root.uiPalette.tabularFigures
                                    color: root.uiPalette.textSecondary
                                }
                            }
                        }
                    }

                    Item {
                        id: authorsPage
                        objectName: "aboutPageAuthors"
                        width: parent.width
                        visible: aboutPages.currentIndex === 2
                        implicitHeight: contributorsLink.y + contributorsLink.implicitHeight

                        Label {
                            id: authorsText
                            width: parent.width
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: I18n.i18nc("@info", "The Ferrous authors")
                        }

                        Kirigami.UrlButton {
                            id: contributorsLink
                            objectName: "aboutContributorsLink"
                            anchors.top: authorsText.bottom
                            anchors.topMargin: Kirigami.Units.smallSpacing
                            width: Math.min(implicitWidth, parent.width)
                            wrapMode: Text.Wrap
                            url: root.homepageUrl + "/graphs/contributors"
                            text: I18n.i18nc("@action:button", "Contributors")
                            color: root.uiPalette.link
                        }
                    }

                    Item {
                        id: licensePage
                        objectName: "aboutPageLicense"
                        width: parent.width
                        visible: aboutPages.currentIndex === 3
                        implicitHeight: licenseLink.y + licenseLink.implicitHeight

                        Label {
                            id: licenseText
                            width: parent.width
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: I18n.i18nc("@info", "Ferrous is free software: you can redistribute it and modify it under "
                                + "the terms of the GNU General Public License, version 3 or any later version.")
                        }

                        Kirigami.UrlButton {
                            id: licenseLink
                            objectName: "aboutLicenseLink"
                            anchors.top: licenseText.bottom
                            anchors.topMargin: Kirigami.Units.smallSpacing
                            // Wraps rather than run past the dialog's edge.
                            width: Math.min(implicitWidth, parent.width)
                            wrapMode: Text.Wrap
                            url: root.licenseUrl
                            text: I18n.i18nc("@action:button", "GNU General Public License, version 3")
                            color: root.uiPalette.link
                        }
                    }
                }
            }
        }
    }
}
