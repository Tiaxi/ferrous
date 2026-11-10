// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// Help › About KDE: the KDE community that makes the frameworks Ferrous is
// built on, in the words of KDE's own About KDE dialog.
Dialog {
    id: root

    readonly property string kdeUrl: "https://kde.org/"
    readonly property string freeSoftwareUrl: "https://www.gnu.org/philosophy/free-sw.html"
    readonly property string getInvolvedUrl: "https://community.kde.org/Get_Involved"
    readonly property string donateUrl: "https://kde.org/donate/"

    objectName: "aboutKdeDialog"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    title: I18n.i18nc("@title:window", "About KDE")
    standardButtons: Dialog.Close

    // The dialogs' one heading style, in place of the style's larger one.
    header: Components.DialogHeader {
        dialog: root
        uiPalette: root.uiPalette
    }
    width: Math.min(Kirigami.Units.gridUnit * 26,
                    (parent ? parent.width : Kirigami.Units.gridUnit * 28) - 2 * Kirigami.Units.gridUnit)

    // A link as Kirigami draws one, in the window's link colour (the
    // About window's, which reads on the dialog), announced as a link, with
    // the focus ring of the window's other controls while the keyboard is
    // on it.
    component DialogLink: Kirigami.UrlButton {
        id: link

        color: root.uiPalette ? root.uiPalette.link : Kirigami.Theme.linkColor
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
            border.color: root.uiPalette ? root.uiPalette.focusRing : Kirigami.Theme.focusColor
        }
    }

    // The window's colours; the focus ring takes its token.
    property var uiPalette: null

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                objectName: "aboutKdeIcon"
                source: "kde"
                Layout.preferredWidth: Kirigami.Units.iconSizes.large
                Layout.preferredHeight: Kirigami.Units.iconSizes.large
            }

            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                Kirigami.Heading {
                    level: 2
                    text: I18n.i18nc("@title", "KDE")
                }
                Label {
                    text: I18n.i18nc("@info KDE's motto", "Be Free!")
                }
            }
        }

        Label {
            objectName: "aboutKdeText"
            Layout.fillWidth: true
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

        // The links, after the text they belong to and in its order: each a
        // control that Tab reaches and Enter or Space opens.
        Flow {
            objectName: "aboutKdeLinks"
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing * 2

            DialogLink {
                objectName: "aboutKdeLink0"
                text: I18n.i18nc("@action:button link", "What Free Software Is")
                url: root.freeSoftwareUrl
            }
            DialogLink {
                objectName: "aboutKdeLink1"
                text: I18n.i18nc("@action:button link", "Join KDE")
                url: root.getInvolvedUrl
            }
            DialogLink {
                objectName: "aboutKdeLink2"
                text: I18n.i18nc("@action:button link", "Donate to KDE")
                url: root.donateUrl
            }
            DialogLink {
                objectName: "aboutKdeLink3"
                text: I18n.i18nc("@action:button link", "kde.org")
                url: root.kdeUrl
            }
        }
    }
}
