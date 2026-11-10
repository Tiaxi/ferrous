// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/KeyUtils.js" as KeyUtils

// About Ferrous, laid out as KDE's About dialog: the icon, name and
// version, then the About, Components, Authors and License tabs.
Dialog {
    id: root

    required property var uiPalette
    required property var windowRoot

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
    parent: Overlay.overlay
    modal: true
    focus: true
    title: I18n.i18nc("@title:window", "About Ferrous")
    // 29 grid units, or wider when a large font makes the tabs or the
    // buttons need more; the texts wrap within it.
    readonly property real minimumWidth: Math.ceil(Math.max(
        aboutTabs.implicitWidth + root.leftPadding + root.rightPadding,
        root.componentsWidth + 2 * Kirigami.Units.largeSpacing + root.leftPadding + root.rightPadding,
        aboutFooter.implicitWidth))
    // The Components page: the names' column, 9 grid units or as wide as
    // the longest name, and the versions after it.
    readonly property real componentNameWidth: {
        let widest = 0
        for (let i = 0; i < root.components.length; ++i) {
            widest = Math.max(widest, componentMetrics.advanceWidth(String(root.components[i].name)))
        }
        return Math.max(Kirigami.Units.gridUnit * 9, Math.ceil(widest))
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
    width: Math.min(Math.max(Kirigami.Units.gridUnit * 29, root.minimumWidth),
        root.windowRoot.width - 2 * Kirigami.Units.gridUnit)
    closePolicy: Popup.CloseOnEscape

    onAboutToShow: root.tabIndex = 0
    onOpened: aboutTabs.currentItem.forceActiveFocus(Qt.PopupFocusReason)

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

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    // A text field that only the clipboard sees.
    TextEdit {
        id: clipboardText
        visible: false
    }

    footer: RowLayout {
        id: aboutFooter
        spacing: 0

        Button {
            objectName: "aboutCopyVersionButton"
            Layout.leftMargin: Kirigami.Units.largeSpacing * 2
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
                onClicked: root.close()
            }
        }
    }

    contentItem: ColumnLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, KeyUtils.popupTakesKeys(root)) }

        spacing: 0

        RowLayout {
            Layout.leftMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.gridUnit - 2

            Kirigami.Icon {
                objectName: "aboutIcon"
                implicitWidth: Kirigami.Units.iconSizes.large
                implicitHeight: Kirigami.Units.iconSizes.large
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
                    text: "Ferrous"
                    font.pointSize: root.uiPalette.basePointSize * 1.125
                    font.weight: Font.DemiBold
                    color: root.uiPalette.text
                    Accessible.role: Accessible.Heading
                }

                Label {
                    objectName: "aboutVersion"
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

        StackLayout {
            objectName: "aboutPages"
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing + 2
            currentIndex: root.tabIndex

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing + 2

                Label {
                    objectName: "aboutDescription"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: I18n.i18nc("@info", "A music player for KDE Plasma with live spectrogram and waveform views.")
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.textSecondary
                    text: I18n.i18nc("@info", "© 2026 The Ferrous authors · GNU GPL, version 3 or later")
                }

                RowLayout {
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

            ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: root.components

                    delegate: RowLayout {
                        required property var modelData
                        spacing: Kirigami.Units.largeSpacing

                        Label {
                            text: modelData.name
                            Layout.preferredWidth: root.componentNameWidth
                            elide: Text.ElideRight
                        }

                        Label {
                            text: modelData.version
                            font.features: root.uiPalette.tabularFigures
                            color: root.uiPalette.textSecondary
                        }
                    }
                }
            }

            ColumnLayout {
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: I18n.i18nc("@info", "The Ferrous authors")
                }

                Kirigami.UrlButton {
                    url: root.homepageUrl + "/graphs/contributors"
                    text: I18n.i18nc("@action:button", "Contributors")
                    color: root.uiPalette.link
                }
            }

            ColumnLayout {
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: I18n.i18nc("@info", "Ferrous is free software: you can redistribute it and modify it under "
                        + "the terms of the GNU General Public License, version 3 or any later version.")
                }

                Kirigami.UrlButton {
                    objectName: "aboutLicenseLink"
                    // Wraps rather than run past the dialog's edge.
                    Layout.fillWidth: true
                    Layout.maximumWidth: Math.ceil(implicitWidth)
                    wrapMode: Text.Wrap
                    url: root.licenseUrl
                    text: I18n.i18nc("@action:button", "GNU General Public License, version 3")
                    color: root.uiPalette.link
                }
            }
        }
    }
}
