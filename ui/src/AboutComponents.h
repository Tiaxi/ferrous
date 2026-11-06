// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QVariantList>

// The libraries and the windowing system Ferrous runs on, for the About
// dialog's Components tab and its version information: each entry a map
// with a "name" and a "version".
QVariantList aboutComponents();
