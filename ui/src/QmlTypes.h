// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Registers the C++ types that the FerrousUi QML module's files use. The app
// calls it before it loads the module, and so does the test that loads the
// module as the app does.
void registerFerrousQmlTypes();
