// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import "../logic/SearchIcons.js" as SearchIcons

Image {
    required property string resultType
    required property color foreground
    // The icon's URL, when it was made already; see SearchIcons.sources.
    property string iconSource: ""
    objectName: "globalSearchResultIcon"
    sourceSize.width: 64
    sourceSize.height: 64
    fillMode: Image.PreserveAspectFit
    asynchronous: true
    source: iconSource !== "" ? iconSource : SearchIcons.source(resultType, foreground)
}
