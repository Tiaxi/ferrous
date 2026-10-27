// SPDX-License-Identifier: GPL-3.0-or-later

#include "Localization.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QQmlEngine>
#include <QString>
#include <qqml.h>

#include <string_view>

namespace FerrousLocalization {

// The build defines the domain of the i18n() calls in C++.
static_assert(std::string_view(TRANSLATION_DOMAIN) == kTranslationDomain);

void setUpApplicationDomain() {
    KLocalizedString::setApplicationDomain(kTranslationDomain);
}

void registerQmlTypes() {
    qmlRegisterSingletonType<KLocalizedQmlContext>(
        "FerrousUi", 1, 0, "I18n",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            // The engine takes ownership of the singleton it gets from here.
            auto *context = new KLocalizedQmlContext;
            context->setTranslationDomain(QString::fromLatin1(kTranslationDomain));
            return context;
        });
}

} // namespace FerrousLocalization
