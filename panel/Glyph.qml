pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

Item {
    id: glyph
    required property color tint
    required property url source
    implicitWidth: 20
    implicitHeight: implicitWidth

    Image {
        anchors.fill: parent
        source: glyph.source
        fillMode: Image.PreserveAspectFit
        sourceSize: Qt.size(glyph.width * Screen.devicePixelRatio, glyph.height * Screen.devicePixelRatio)
        layer.enabled: true
        layer.effect: MultiEffect {
            brightness: 1
            colorization: 1
            colorizationColor: glyph.tint
        }
    }
}
