import QtQuick 2.15

Transition {
    id: root
    property int duration: 0

    NumberAnimation {
        properties: "opacity,scale,x,y"
        duration: root.duration
    }
}
