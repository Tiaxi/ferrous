// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Ferrous translates its user interface with KI18n, from the "ferrous"
// gettext domain. See docs/DEVELOPMENT.md for the translation template.
namespace FerrousLocalization {

inline constexpr char kTranslationDomain[] = "ferrous";

// Makes "ferrous" the application's translation domain, for the i18n() calls
// in C++. Call it before the first translation, and so before any QML loads.
void setUpApplicationDomain();

// Registers the I18n singleton (FerrousUi 1.0) that QML and its JavaScript
// translate with, as in I18n.i18nc("@action:button", "Save"). Each engine gets
// its own instance, so engines need no further setup.
void registerQmlTypes();

} // namespace FerrousLocalization
