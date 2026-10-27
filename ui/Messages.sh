#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes the translation template, $podir/ferrous.pot, by KDE's convention:
# run from this directory with $XGETTEXT and $podir set, as KDE's translation
# tooling does. scripts/extract-messages.sh sets them for a local run.

set -euo pipefail

# The C++ sources, then the QML and its JavaScript, which xgettext reads as
# JavaScript. Sorted, so the template is the same on every machine.
# shellcheck disable=SC2046
$XGETTEXT --language=C++ $(find src -name '*.cpp' -o -name '*.h' | LC_ALL=C sort) \
    -o "$podir/ferrous.pot"
# shellcheck disable=SC2046
$XGETTEXT --language=JavaScript --join-existing \
    $(find qml -name '*.qml' -o -name '*.js' | LC_ALL=C sort) -o "$podir/ferrous.pot"
