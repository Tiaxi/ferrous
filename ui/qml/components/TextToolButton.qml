// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// A flat tool button with a caption, and an icon from `icon.name`, in the
// font of the text around it. The style draws a ToolButton's caption in
// the toolbar font, which KDE sets apart from the general font, whatever
// the button's own font: with a 24 pt general font, Recent Issues' Clear
// All stayed at 10 pt beside its 24 pt title (#373 H10). Here the caption
// and the icon are the button's own content, and the style draws only its
// frame, as for any tool button: on hover, when pressed and with focus.
ToolButton {
    id: root

    flat: true
    // The style draws neither a caption nor an icon of its own.
    display: AbstractButton.TextOnly
    Kirigami.MnemonicData.label: ""
    leftPadding: Kirigami.Units.smallSpacing + 2
    rightPadding: Kirigami.Units.smallSpacing + 2
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            objectName: "textToolButtonIcon"
            visible: root.icon.name.length > 0
            source: root.icon.name
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
            Layout.alignment: Qt.AlignVCenter
            Accessible.ignored: true
        }

        Label {
            objectName: "textToolButtonCaption"
            text: root.text
            color: root.enabled ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
            Layout.alignment: Qt.AlignVCenter
            Accessible.ignored: true
        }
    }
}
