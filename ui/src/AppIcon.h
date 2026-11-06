// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QIcon>
#include <QString>

#include <array>

// The app icon (#373): a drawing of its own for each pixel size, in
// assets/icons/hicolor. The build installs them in the hicolor icon theme and
// builds them in, at :/icons/hicolor/<size>x<size>/apps/ferrous.svg and
// :/icons/hicolor/scalable/apps/ferrous.svg.
namespace FerrousAppIcon {

// The pixel sizes with a drawing of their own, smallest first.
inline constexpr std::array<int, 7> kSizes{16, 22, 32, 48, 64, 128, 256};

// The built-in drawing for a size in kSizes; any other size gets the
// scalable drawing.
QString resourcePath(int size);

// The window icon: each size's drawing rendered at its own size, so that a
// window manager or task switcher that asks for one of the sizes, in device
// pixels, gets the drawing made for it.
QIcon icon();

} // namespace FerrousAppIcon
