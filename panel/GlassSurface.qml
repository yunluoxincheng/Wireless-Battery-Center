pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes

Item {
    id: glass
    required property bool dark
    property bool floating: false
    property bool hot: false
    property color outline: "transparent"
    property int radius: 16
    property real sheen: hot ? 1 : 0
    Behavior on sheen { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

    // All surfaces share one nine-sliced shadow texture; DWM composes the backdrop once per window.
    BorderImage {
        anchors.fill: parent
        anchors.margins: -16
        anchors.topMargin: -12
        anchors.bottomMargin: glass.floating ? -24 : -20
        source: "assets/glass-shadow.png"
        border { left: 40; top: 40; right: 40; bottom: 40 }
        opacity: glass.dark ? (glass.floating ? 0.42 : 0.26) : (glass.floating ? 0.23 : 0.13)
    }
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: -1
            fillRule: ShapePath.OddEvenFill
            fillGradient: LinearGradient {
                x2: glass.width * 0.27
                y2: glass.height
                GradientStop { position: 0; color: glass.dark ? "#40ffffff" : "#ffffff" }
                GradientStop { position: 0.38; color: glass.dark ? "#23ffffff" : "#eaffffff" }
                GradientStop { position: 1; color: glass.dark ? "#13ffffff" : "#8affffff" }
            }
            PathRectangle { width: glass.width; height: glass.height; radius: glass.radius }
            PathRectangle {
                x: 1; y: 1
                width: Math.max(0, glass.width - 2); height: Math.max(0, glass.height - 2)
                radius: glass.radius - 1
            }
        }
        ShapePath {
            strokeWidth: -1
            fillGradient: LinearGradient {
                x2: glass.width * 0.33
                y2: glass.height
                GradientStop {
                    position: 0
                    color: glass.dark ? Qt.rgba(0.17, 0.21, 0.29, 0.78 + glass.sheen * 0.10)
                                      : Qt.rgba(1, 1, 1, 0.74 + glass.sheen * 0.14)
                }
                GradientStop {
                    position: 1
                    color: glass.dark ? Qt.rgba(0.10, 0.13, 0.19, 0.74 + glass.sheen * 0.06)
                                      : Qt.rgba(0.95, 0.97, 1, 0.48 + glass.sheen * 0.18)
                }
            }
            PathRectangle {
                x: 1; y: 1
                width: Math.max(0, glass.width - 2); height: Math.max(0, glass.height - 2)
                radius: glass.radius - 1
            }
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: glass.radius
        color: "transparent"
        border.color: glass.outline
    }
}
