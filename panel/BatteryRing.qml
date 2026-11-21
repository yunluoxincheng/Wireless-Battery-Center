pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: ring
    enum Size { Small, Medium, Large }
    property int size: BatteryRing.Medium
    required property int percent
    required property bool cached
    required property bool dimmed
    required property bool charging
    required property bool dark
    required property color arcStart
    required property color arcEnd
    required property color iconColor
    required property url iconSource

    implicitWidth: size === BatteryRing.Large ? 164 : size === BatteryRing.Small ? 84 : 104
    implicitHeight: implicitWidth
    readonly property real stroke: size === BatteryRing.Large ? 8.5 : size === BatteryRing.Small ? 5.5 : 6.5
    readonly property int iconSize: size === BatteryRing.Large ? 52 : size === BatteryRing.Small ? 28 : 34
    readonly property int boltSize: size === BatteryRing.Large ? 21 : size === BatteryRing.Small ? 13 : 16

    Canvas {
        id: dial
        anchors.fill: parent
        readonly property real stroke: ring.stroke
        readonly property color track: ring.dark ? "#1affffff" : "#160f172a"
        readonly property color bezel: ring.dark ? "#14ffffff" : "#0d0f172a"
        readonly property color start: ring.arcStart
        readonly property color end: ring.arcEnd
        readonly property bool cached: ring.cached
        readonly property bool dimmed: ring.dimmed
        property real progress: ring.percent > 0 ? Math.max(0.02, Math.min(1, ring.percent / 100)) : 0
        Behavior on progress { NumberAnimation { duration: 580; easing.type: Easing.OutCubic } }

        onProgressChanged: requestPaint()
        onStrokeChanged: requestPaint()
        onTrackChanged: requestPaint()
        onBezelChanged: requestPaint()
        onStartChanged: requestPaint()
        onEndChanged: requestPaint()
        onCachedChanged: requestPaint()
        onDimmedChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            const center = width / 2
            const radius = center - stroke - 6
            const top = -Math.PI / 2
            const full = 2 * Math.PI
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            ctx.lineWidth = 1.25
            ctx.strokeStyle = bezel
            ctx.beginPath()
            ctx.arc(center, center, center - 1.5, 0, full)
            ctx.stroke()
            ctx.lineWidth = stroke
            ctx.strokeStyle = track
            ctx.beginPath()
            ctx.arc(center, center, radius, 0, full)
            ctx.stroke()

            if (progress > 0) {
                const gradient = ctx.createLinearGradient(0, height, width, 0)
                gradient.addColorStop(0, start)
                gradient.addColorStop(1, end)
                ctx.strokeStyle = gradient
                ctx.lineCap = "round"
                ctx.globalAlpha = dimmed ? 0.58 : 1
                ctx.beginPath()
                ctx.arc(center, center, radius, top, top + full * progress)
                ctx.stroke()
            }
            if (cached) {
                ctx.globalAlpha = 0.45
                ctx.strokeStyle = start
                ctx.lineWidth = 1
                ctx.lineCap = "butt"
                ctx.setLineDash([2.5, 3.5])
                ctx.beginPath()
                ctx.arc(center, center, radius - stroke - 3.5, 0, full)
                ctx.stroke()
            }
        }
    }

    Glyph {
        anchors.centerIn: parent
        width: ring.iconSize
        height: width
        source: ring.iconSource
        tint: ring.iconColor
    }
    Rectangle {
        visible: ring.charging
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: ring.size === BatteryRing.Large ? 8 : 4
        width: ring.boltSize + 6
        height: width
        radius: width / 2
        color: ring.dark ? "#172331" : "#f8fffc"
        border.color: ring.dark ? "#375e55" : "#b5ddcd"
        Glyph {
            anchors.centerIn: parent
            width: ring.boltSize
            height: width
            source: "assets/icons/lucide/zap.svg"
            tint: ring.dark ? "#52e5a3" : "#137e51"
        }
    }
}
