// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami

// The style's push button with a caption in the button's own font. The
// desktop style paints a Button's caption in the application's font
// whatever the button's is: over the whole screen, the art preview's Back
// to Results and Apply declared 16 px, as the details over them, and
// showed 13. Here the caption and the glyph are the button's own content,
// as TextToolButton's are, and the style draws the rest: its bevel, its
// focus ring and the default button's look. At the application's font the
// button is the size of the style's own. Alt and the caption's letter
// presses it, and underlines that letter, as the style's does.
Button {
    id: root

    // The style draws neither a caption nor a glyph of its own.
    display: AbstractButton.TextOnly
    Kirigami.MnemonicData.label: ""
    // The room Breeze leaves around a caption: 8 px, and at the sides its
    // frame's pixel more.
    leftPadding: Kirigami.Units.largeSpacing + 1
    rightPadding: Kirigami.Units.largeSpacing + 1
    // And no narrower than Breeze's push button with a caption, 84 px.
    implicitWidth: Math.max(10 * Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing,
        implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    topPadding: Kirigami.Units.largeSpacing
    bottomPadding: Kirigami.Units.largeSpacing
    spacing: Kirigami.Units.smallSpacing

    contentItem: Item {
        id: content

        // Disabled, the theme's text is already its disabled one, as the style paints it.
        readonly property color ink: Kirigami.Theme.textColor

        Kirigami.MnemonicData.enabled: root.enabled && root.visible
        Kirigami.MnemonicData.controlType: Kirigami.MnemonicData.ActionElement
        Kirigami.MnemonicData.label: root.text

        implicitWidth: (glyph.visible ? glyph.width + root.spacing : 0) + Math.ceil(caption.implicitWidth)
        implicitHeight: Math.max(glyph.visible ? glyph.height : 0, Math.ceil(caption.implicitHeight))

        Shortcut {
            sequence: content.Kirigami.MnemonicData.sequence
            enabled: root.enabled && root.visible
            onActivated: root.animateClick()
        }

        // Centred, as the style's, however wide the button is, as stacked
        // ones are.
        Row {
            anchors.centerIn: parent
            spacing: root.spacing

            Kirigami.Icon {
                id: glyph
                objectName: "captionButtonIcon"
                anchors.verticalCenter: parent.verticalCenter
                width: root.icon.width
                height: root.icon.height
                visible: root.icon.name.length > 0
                source: root.icon.name
                color: content.ink
                isMask: true
                Accessible.ignored: true
            }

            Label {
                id: caption
                objectName: "captionButtonCaption"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(Math.ceil(implicitWidth),
                                content.width - (glyph.visible ? glyph.width + root.spacing : 0))
                textFormat: Text.PlainText
                text: content.Kirigami.MnemonicData.plainTextLabel
                font: root.font
                color: content.ink
                elide: Text.ElideRight
                Accessible.ignored: true

                // The mnemonic's underline, while Alt is held.
                Rectangle {
                    objectName: "captionButtonMnemonic"
                    readonly property string marked: content.Kirigami.MnemonicData.mnemonicLabel
                    readonly property int letter: marked.indexOf("&")
                    visible: content.Kirigami.MnemonicData.active && letter >= 0
                        && letter < caption.text.length && !caption.truncated
                    x: captionMetrics.advanceWidth(caption.text.substring(0, letter))
                    y: Math.round(caption.baselineOffset + captionMetrics.underlinePosition)
                    width: captionMetrics.advanceWidth(caption.text.charAt(letter))
                    height: Math.max(1, Math.round(captionMetrics.lineWidth))
                    color: content.ink
                }

                FontMetrics {
                    id: captionMetrics
                    font: caption.font
                }
            }
        }
    }
}
