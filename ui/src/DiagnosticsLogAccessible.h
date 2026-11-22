// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace DiagnosticsLogAccessible {

// Makes a DiagnosticsLogView read-only text to assistive technology, as a
// text edit is. The item needs an Accessible role in QML as well, which
// puts it among the items that assistive technology sees.
void install();

} // namespace DiagnosticsLogAccessible
