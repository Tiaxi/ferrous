// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// Help › About KDE: the KDE community that makes the frameworks Ferrous is
// built on, in the words of KDE's own About KDE dialog. A window of its
// own, as About Ferrous is, with the same heading, page and button row
// (#373 H4): the icon, the name and its motto, then the text and its
// links where About Ferrous has its pages.
Components.DialogWindow {
    id: root

    readonly property string kdeUrl: "https://kde.org/"
    readonly property string freeSoftwareUrl: "https://www.gnu.org/philosophy/free-sw.html"
    readonly property string getInvolvedUrl: "https://community.kde.org/Get_Involved"
    readonly property string donateUrl: "https://kde.org/donate/"

    objectName: "aboutKdeDialog"
    title: I18n.i18nc("@title:window", "About KDE")

    // About Ferrous's geometry: the content's inset from the window's
    // edges, the page's inset and its gap under the heading, where About
    // Ferrous has its tabs and the gap under them, and the room between
    // the page's last line and the buttons.
    readonly property real contentPadding: Kirigami.Units.gridUnit
    readonly property real pageInset: Kirigami.Units.largeSpacing
    readonly property real pageTopMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing + 2
    readonly property real pageBottomGap: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    // The room around the page, inside its view, for a link's focus ring,
    // which the view would otherwise cut.
    readonly property real ringRoom: 4
    // The page's width, from the window's: its text wraps to it, so its
    // height is known as soon as the window's width is, before it shows.
    readonly property real pageWidth: Math.max(0, root.width - 2 * root.contentPadding - 2 * root.pageInset)
    // As wide as the heading, the widest link and the buttons need; the
    // text wraps within it.
    readonly property real contentMinimumWidth: Math.ceil(Math.max(
        Kirigami.Units.gridUnit * 18,
        aboutKdeHeading.implicitWidth + Kirigami.Units.smallSpacing + 2 * root.contentPadding,
        aboutKdeLinks.widestLink + 2 * root.pageInset + 2 * root.contentPadding,
        aboutKdeFooter.implicitWidth))
    FontMetrics {
        id: bodyMetrics
        font: root.uiPalette.baseFont
    }
    // The heading and the room around the page.
    readonly property real chromeHeight: 2 * root.contentPadding + aboutKdeHeading.implicitHeight
        + root.pageTopMargin + root.pageBottomGap + aboutKdeFooter.implicitHeight
    minimumWidth: root.contentMinimumWidth
    // The heading and three lines of the page, which scrolls.
    minimumHeight: Math.ceil(root.chromeHeight + 3 * bodyMetrics.height)
    // About Ferrous's 29 grid units, or wider when a large font makes the
    // heading or the buttons need more; as tall as the page at that width,
    // within the screen, where the page scrolls.
    defaultWidth: Math.max(Kirigami.Units.gridUnit * 29, root.contentMinimumWidth)
    defaultHeight: Math.ceil(root.chromeHeight + aboutKdePage.implicitHeight)

    // The default button has the keyboard, as in KDE's dialogs; Tab goes
    // to the links.
    onAboutToShow: {
        aboutKdePageView.contentY = 0
        aboutKdeCloseButton.forceActiveFocus(Qt.PopupFocusReason)
    }

    // A link as Kirigami draws one, in the window's link colour, announced
    // as a link, with the focus ring of the window's other controls while
    // the keyboard is on it.
    component DialogLink: Kirigami.UrlButton {
        id: link

        color: root.uiPalette.link
        Accessible.role: Accessible.Link

        Rectangle {
            objectName: "dialogLinkFocusRing"
            visible: link.activeFocus
            x: -3
            y: -3
            width: link.mouseArea.width + 6
            height: link.height + 6
            radius: 3
            color: "transparent"
            border.width: 2
            border.color: root.uiPalette.focusRing
        }
    }

    footer: RowLayout {
        id: aboutKdeFooter
        spacing: 0

        Components.DialogButtons {
            id: dialogButtons
            primary: aboutKdeCloseButton
            Layout.fillWidth: true

            Button {
                id: aboutKdeCloseButton
                objectName: "aboutKdeCloseButton"
                text: I18n.i18nc("@action:button", "Close")
                icon.name: "window-close"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: root.reject()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.contentPadding
        // The page ends this far above the buttons.
        anchors.bottomMargin: root.contentPadding + root.pageBottomGap - root.ringRoom

        // Enter presses the default button, as in KDE's dialogs; a link
        // takes its own.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, root.visible) }

        spacing: 0

        RowLayout {
            id: aboutKdeHeading
            Layout.leftMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.gridUnit - 2

            Kirigami.Icon {
                objectName: "aboutKdeIcon"
                implicitWidth: Kirigami.Units.iconSizes.large
                implicitHeight: Kirigami.Units.iconSizes.large
                source: "kde"
                Accessible.ignored: true
            }

            ColumnLayout {
                spacing: 0

                Label {
                    objectName: "aboutKdeName"
                    text: I18n.i18nc("@title", "KDE")
                    font.pointSize: root.uiPalette.basePointSize * 1.125
                    font.weight: Font.DemiBold
                    color: root.uiPalette.text
                    Accessible.role: Accessible.Heading
                }

                Label {
                    objectName: "aboutKdeMotto"
                    text: I18n.i18nc("@info KDE's motto", "Be Free!")
                    color: root.uiPalette.textSecondary
                }
            }
        }

        // The page, in the room the window leaves it: it scrolls when a
        // smaller window or a large font leaves less than it needs, and
        // never runs under the buttons.
        Flickable {
            id: aboutKdePageView
            objectName: "aboutKdePageView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: root.pageInset - root.ringRoom
            Layout.rightMargin: root.pageInset - root.ringRoom
            Layout.topMargin: root.pageTopMargin
            implicitHeight: contentHeight
            contentWidth: width
            contentHeight: aboutKdePage.implicitHeight + root.ringRoom
            clip: true
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                objectName: "aboutKdePageScrollBar"
                policy: aboutKdePageView.interactive ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }

            // Its lines laid out top to bottom by anchors, and the links
            // placed by hand, which places them at once: the page's height
            // is known before the window shows, so the window opens tall
            // enough for it.
            Item {
                id: aboutKdePage
                objectName: "aboutKdePage"
                x: root.ringRoom
                width: root.pageWidth
                implicitHeight: aboutKdeLinks.y + aboutKdeLinks.implicitHeight
                height: implicitHeight

                Label {
                    id: aboutKdeText
                    objectName: "aboutKdeText"
                    width: parent.width
                    wrapMode: Text.Wrap
                    textFormat: Text.StyledText
                    text: I18n.i18nc("@info",
                        "<b>KDE</b> is a world-wide community of software engineers, artists, writers, "
                        + "translators and creators who are committed to Free Software development. KDE "
                        + "produces the Plasma desktop environment, hundreds of applications, and the many "
                        + "software libraries that support them.<br/><br/>"
                        + "KDE is a cooperative enterprise: no single entity controls its direction or products. "
                        + "Instead, we work together to achieve the common goal of building the world's finest "
                        + "Free Software. Everyone is welcome to join and contribute to KDE, including you."
                        + "<br/><br/>"
                        + "KDE software is and will always be available free of charge, however creating it is "
                        + "not free. Donations support it.")
                }

                // The links, after the text they belong to and in its
                // order, as many to a line as fit: each a control that Tab
                // reaches and Enter or Space opens.
                Item {
                    id: aboutKdeLinks
                    objectName: "aboutKdeLinks"
                    readonly property var links: [aboutKdeLink0, aboutKdeLink1, aboutKdeLink2, aboutKdeLink3]
                    readonly property real spacing: Kirigami.Units.gridUnit - 2
                    readonly property real lineSpacing: Kirigami.Units.smallSpacing
                    readonly property real widestLink: Math.max(aboutKdeLink0.implicitWidth,
                        aboutKdeLink1.implicitWidth, aboutKdeLink2.implicitWidth, aboutKdeLink3.implicitWidth)
                    // Each link's place, and the height of the lines.
                    readonly property var places: {
                        const result = []
                        let x = 0
                        let y = 0
                        let lineHeight = 0
                        for (let i = 0; i < links.length; ++i) {
                            const link = links[i]
                            if (x > 0 && x + link.implicitWidth > width) {
                                x = 0
                                y += lineHeight + lineSpacing
                                lineHeight = 0
                            }
                            result.push(Qt.point(x, y))
                            x += link.implicitWidth + spacing
                            lineHeight = Math.max(lineHeight, link.implicitHeight)
                        }
                        return { points: result, height: y + lineHeight }
                    }
                    anchors.top: aboutKdeText.bottom
                    anchors.topMargin: Kirigami.Units.largeSpacing + 2
                    width: parent.width
                    implicitHeight: places.height
                    height: implicitHeight

                    DialogLink {
                        id: aboutKdeLink0
                        objectName: "aboutKdeLink0"
                        x: aboutKdeLinks.places.points[0].x
                        y: aboutKdeLinks.places.points[0].y
                        text: I18n.i18nc("@action:button link", "What Free Software Is")
                        url: root.freeSoftwareUrl
                    }
                    DialogLink {
                        id: aboutKdeLink1
                        objectName: "aboutKdeLink1"
                        x: aboutKdeLinks.places.points[1].x
                        y: aboutKdeLinks.places.points[1].y
                        text: I18n.i18nc("@action:button link", "Join KDE")
                        url: root.getInvolvedUrl
                    }
                    DialogLink {
                        id: aboutKdeLink2
                        objectName: "aboutKdeLink2"
                        x: aboutKdeLinks.places.points[2].x
                        y: aboutKdeLinks.places.points[2].y
                        text: I18n.i18nc("@action:button link", "Donate to KDE")
                        url: root.donateUrl
                    }
                    DialogLink {
                        id: aboutKdeLink3
                        objectName: "aboutKdeLink3"
                        x: aboutKdeLinks.places.points[3].x
                        y: aboutKdeLinks.places.points[3].y
                        text: I18n.i18nc("@action:button link", "kde.org")
                        url: root.kdeUrl
                    }
                }
            }
        }
    }
}
