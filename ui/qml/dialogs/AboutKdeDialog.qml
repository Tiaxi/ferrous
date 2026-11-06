// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0

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
    width: Math.min(Kirigami.Units.gridUnit * 26,
                    (parent ? parent.width : Kirigami.Units.gridUnit * 28) - 2 * Kirigami.Units.gridUnit)

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
                + "translators and creators who are committed to <a href=\"%1\">Free Software</a> "
                + "development. KDE produces the Plasma desktop environment, hundreds of applications, "
                + "and the many software libraries that support them.<br/><br/>"
                + "KDE is a cooperative enterprise: no single entity controls its direction or products. "
                + "Instead, we work together to achieve the common goal of building the world's finest "
                + "Free Software. Everyone is welcome to <a href=\"%2\">join and contribute</a> to KDE, "
                + "including you.<br/><br/>"
                + "KDE software is and will always be available free of charge, however creating it is "
                + "not free. You can support it with a <a href=\"%3\">donation</a>.<br/><br/>"
                + "Visit <a href=\"%4\">%4</a> for more information about the KDE community and the "
                + "software we produce.",
                root.freeSoftwareUrl, root.getInvolvedUrl, root.donateUrl, root.kdeUrl)
            onLinkActivated: function(link) { Qt.openUrlExternally(link) }

            HoverHandler {
                cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }
    }
}
