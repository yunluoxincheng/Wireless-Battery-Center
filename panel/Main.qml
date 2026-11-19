pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import WirelessBattery

ApplicationWindow {
    id: window
    required property DeviceModel devices
    property alias titleDragArea: caption
    property alias maximizeButton: maximizeControl
    signal toggleMaximizeRequested()
    property bool backgroundStartFailed: false
    property bool connectedOnce: false
    property bool connectionWaitElapsed: false
    property bool nativeBackdrop: false
    width: 860
    height: 648
    minimumWidth: 520
    minimumHeight: 460
    title: "Wireless Battery"
    color: "transparent"

    // Qt 6.8 exposes QStyleHints at runtime and declares it as QObject to qmllint.
    // qmllint disable missing-property
    readonly property bool dark: appearance.themeMode === "dark"
        || (appearance.themeMode !== "light" && Qt.styleHints.colorScheme === Qt.Dark)
    // qmllint enable missing-property
    readonly property color canvas: dark ? "#131822" : "#e8eff6"
    readonly property color surface: dark ? "#242e3e" : "#f5f8fc"
    readonly property color hoverSurface: dark ? "#23ffffff" : "#160f2845"
    readonly property color ink: dark ? "#f4f7fc" : "#152238"
    readonly property color muted: dark ? "#a8b7cc" : "#50627a"
    readonly property color line: dark ? "#20ffffff" : "#190f172a"
    readonly property color accent: dark ? "#7bd9fc" : "#11698e"
    readonly property color accentSoft: dark ? "#283c50" : "#dcedf7"
    readonly property color good: dark ? "#34d38b" : "#137e51"
    readonly property color danger: dark ? "#fb7886" : "#b52b41"
    readonly property int edge: 24
    readonly property bool ready: devices.connected && devices.snapshot.poll > 0
    readonly property bool canEdit: devices.connected && !devices.busy
    readonly property bool connectionNotice: !devices.connected
        && (backgroundStartFailed || connectedOnce || connectionWaitElapsed)
    readonly property PanelPage subPage: pages.currentItem as PanelPage
    property int rowsRevision: 0

    palette.window: surface
    palette.base: surface
    palette.button: surface
    palette.text: ink
    palette.windowText: ink
    palette.buttonText: ink
    palette.placeholderText: muted
    palette.highlight: accent
    palette.highlightedText: dark ? canvas : "white"
    palette.mid: line
    palette.dark: line

    Settings {
        id: appearance
        category: "Appearance"
        property string themeMode: "system"
    }

    background: Rectangle {
        color: window.nativeBackdrop ? (window.dark ? "#cc131822" : "#bdeef4fb") : window.canvas
        // A shared field of diffused light supplies depth when the Windows backdrop is flat or unavailable.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeWidth: -1
                fillGradient: RadialGradient {
                    centerX: window.width * 0.84
                    centerY: window.height * 0.24
                    centerRadius: window.width * 0.64
                    focalX: centerX
                    focalY: centerY
                    GradientStop { position: 0; color: window.dark ? "#35526c91" : "#55b4cbe0" }
                    GradientStop { position: 1; color: "transparent" }
                }
                PathRectangle { width: window.width; height: window.height }
            }
            ShapePath {
                strokeWidth: -1
                fillGradient: RadialGradient {
                    centerX: window.width * 0.11
                    centerY: window.height * 0.69
                    centerRadius: window.width * 0.50
                    focalX: centerX
                    focalY: centerY
                    GradientStop { position: 0; color: window.dark ? "#294d6581" : "#c7ffffff" }
                    GradientStop { position: 1; color: "transparent" }
                }
                PathRectangle { width: window.width; height: window.height }
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: window.visibility !== Window.Maximized
            radius: 8
            color: "transparent"
            border.color: window.dark ? "#22ffffff" : "#dcffffff"
        }
    }

    function svg(name: string): url {
        return "qrc:/qt/qml/WirelessBattery/panel/assets/icons/lucide/" + name + ".svg"
    }

    function kindText(kind: int): string {
        switch (kind) {
        case 1: return qsTr("Mouse")
        case 2: return qsTr("Headset")
        case 3: return qsTr("Keyboard")
        case 4: return qsTr("Controller")
        case 5: return qsTr("Phone")
        default: return qsTr("Unknown")
        }
    }

    function deviceIcon(device: var): string {
        switch (device.kind) {
        case 1: return "mouse"
        case 2: return "headphones"
        case 3: return "keyboard"
        case 4: return "gamepad-2"
        case 5: return "smartphone"
        default:
            if (device.via === "USB cable")
                return "usb"
            return "circle-question-mark"
        }
    }

    function linkText(via: string): string {
        switch (via) {
        case "2.4 GHz receiver": return qsTr("2.4 GHz receiver")
        case "USB cable": return qsTr("USB cable")
        case "demo": return qsTr("Sample")
        default: return via || qsTr("Unknown")
        }
    }

    function batteryText(device: var): string {
        if (device.percent === undefined || device.percent < 0)
            return qsTr("Unknown")
        switch (device.level) {
        case "Empty": return qsTr("Empty")
        case "Low": return qsTr("Low")
        case "Medium": return qsTr("Medium")
        case "High": return qsTr("High")
        case "Full": return qsTr("Full")
        default: return device.level || qsTr("%1%").arg(device.percent)
        }
    }

    function cached(device: var): bool {
        return device.percent >= 0 && (!devices.connected || device.cached)
    }

    function low(device: var): bool {
        return devices.connected && !device.offline && !device.charging && !cached(device)
            && device.percent >= 0 && device.percent <= devices.snapshot.threshold
    }

    function critical(device: var): bool {
        // The configured alert threshold governs both colors. The lower half is a visual emphasis only.
        return low(device) && device.percent <= Math.floor(devices.snapshot.threshold / 2)
    }

    function statusText(device: var): string {
        const status = []
        if (!devices.connected)
            status.push(qsTr("Connection lost"))
        else if (device.offline)
            status.push(qsTr("Offline"))
        else if (device.asleep)
            status.push(qsTr("Asleep"))
        else if (device.charging)
            status.push(qsTr("Charging"))
        else if (critical(device))
            status.push(qsTr("Critically low"))
        else if (low(device))
            status.push(qsTr("Low battery"))
        if (cached(device))
            status.push(qsTr("Last known"))
        if (device.hidden)
            status.push(qsTr("Hidden"))
        return status.join(" · ")
    }

    function readingColor(device: var): color {
        if (cached(device) || device.percent === undefined || device.percent < 0)
            return muted
        return critical(device) ? danger : low(device) ? (dark ? "#fbbf58" : "#975405") : good
    }

    function idText(value: int): string {
        return value ? ("000" + value.toString(16).toUpperCase()).slice(-4) : qsTr("Unknown")
    }

    function timeText(value: double): string {
        return value ? new Date(value).toLocaleString(Qt.locale(), Locale.ShortFormat) : qsTr("Unknown")
    }

    Connections {
        target: window.devices
        function onRowsChanged() { ++window.rowsRevision }
        function onStateChanged() {
            if (window.devices.connected)
                window.connectedOnce = true
        }
    }

    Timer {
        interval: 3000
        running: true
        onTriggered: window.connectionWaitElapsed = true
    }

    component TextLabel: Label {
        color: window.ink
        textFormat: Text.PlainText
        font.pixelSize: 14
    }

    component Quiet: TextLabel {
        color: window.muted
        font.pixelSize: 13
    }

    component Title: TextLabel {
        font.pixelSize: 21
        font.weight: Font.DemiBold
    }

    component SectionTitle: TextLabel {
        font.pixelSize: 13
        font.weight: Font.DemiBold
    }

    component Divider: Rectangle {
        implicitHeight: 1
        Layout.fillWidth: true
        color: window.line
    }

    component Action: Button {
        id: action
        property string symbol: ""
        property bool filled: false
        implicitHeight: 34
        leftPadding: display === AbstractButton.IconOnly ? 8 : 12
        rightPadding: leftPadding
        spacing: 7
        font.pixelSize: 13
        font.weight: Font.DemiBold
        focusPolicy: Qt.TabFocus
        hoverEnabled: true
        icon.source: symbol ? window.svg(symbol) : ""
        icon.width: 17
        icon.height: 17
        icon.color: !enabled ? window.muted : window.ink
        palette.buttonText: icon.color
        background: Rectangle {
            radius: 10
            color: action.down || (action.enabled && action.hovered) ? window.hoverSurface
            : action.filled && action.enabled ? (window.dark ? "#17ffffff" : "#0c0f172a") : "transparent"
            border.color: action.activeFocus ? window.accent : "transparent"
            Behavior on color { ColorAnimation { duration: 100 } }
        }
        Accessible.name: text
        ToolTip.text: text
        ToolTip.visible: display === AbstractButton.IconOnly && hovered
        Keys.onReturnPressed: clicked()
    }

    component DeviceRing: BatteryRing {
        id: ring
        required property var device
        dark: window.dark
        percent: device.level || device.percent === undefined ? -1 : device.percent
        cached: window.cached(device)
        charging: window.devices.connected && device.charging === true
        iconSource: window.svg(window.deviceIcon(device))
        iconColor: cached || device.percent < 0 ? window.muted : window.ink
        arcStart: cached ? window.muted : window.critical(device) ? (dark ? "#f26470" : "#d93848")
            : window.low(device) ? (dark ? "#f59e0b" : "#d97706") : (dark ? "#34d38b" : "#16a366")
        arcEnd: cached ? arcStart : window.critical(device) ? (dark ? "#fb7185" : "#ef4455")
            : window.low(device) ? (dark ? "#fbbf24" : "#f59e0b") : (dark ? "#52e5a3" : "#22c57e")
    }

    component Toggle: CheckBox {
        id: toggle
        property string description: ""
        Layout.fillWidth: true
        implicitHeight: Math.max(54, contentItem.implicitHeight + 24)
        padding: 0
        rightPadding: 56
        nextCheckState: function() { return checkState }
        contentItem: Item {
            implicitHeight: toggleCopy.implicitHeight
            ColumnLayout {
                id: toggleCopy
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                spacing: 3
                TextLabel {
                    Layout.fillWidth: true
                    text: toggle.text
                    color: toggle.enabled ? window.ink : window.muted
                    wrapMode: Text.Wrap
                    font.weight: toggle.description ? Font.DemiBold : Font.Normal
                }
                Quiet {
                    Layout.fillWidth: true
                    visible: toggle.description !== ""
                    text: toggle.description
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                }
            }
        }
        Accessible.description: description
        indicator: Rectangle {
            x: toggle.width - width
            anchors.verticalCenter: parent.verticalCenter
            width: 46
            height: 26
            radius: height / 2
            color: toggle.checked ? (window.dark ? "#2ec87e" : "#137e51") : window.hoverSurface
            opacity: toggle.enabled ? 1 : 0.52
            border.color: toggle.activeFocus ? window.accent : "transparent"
            Rectangle {
                x: toggle.checked ? 23 : 3
                y: 3
                width: 20
                height: 20
                radius: height / 2
                color: "#ffffff"
                Behavior on x { NumberAnimation { duration: 155; easing.type: Easing.OutCubic } }
            }
        }
        Keys.onReturnPressed: clicked()
    }

    component Fact: RowLayout {
        id: fact
        required property string caption
        required property string value
        Layout.fillWidth: true
        Layout.minimumHeight: 42
        spacing: 16
        TextLabel { text: fact.caption; font.weight: Font.DemiBold }
        TextLabel {
            text: fact.value
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            horizontalAlignment: Text.AlignRight
        }
    }

    component GlassGroup: ColumnLayout {
        id: group
        required property string heading
        default property alias rows: groupRows.data
        Layout.fillWidth: true
        spacing: 10
        children: [
            SectionTitle { text: group.heading; Layout.leftMargin: 4 },
            Control {
                Layout.fillWidth: true
                leftPadding: 17
                rightPadding: 17
                topPadding: 4
                bottomPadding: 4
                background: GlassSurface { dark: window.dark }
                contentItem: ColumnLayout { id: groupRows; spacing: 0 }
            }
        ]
    }

    component PanelPage: ScrollView {
        id: panelPage
        required property int contentLimit
        property int contentTop: 28
        property string breadcrumb
        default property alias rows: form.data

        contentWidth: availableWidth
        contentHeight: contentTop + form.implicitHeight + 32
        clip: true
        leftPadding: Math.max(window.edge, (width - contentLimit) / 2)
        rightPadding: leftPadding
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        contentData: [
            ColumnLayout {
                id: form
                y: panelPage.contentTop
                width: panelPage.availableWidth
                spacing: 24
            }
        ]
    }

    Shortcut {
        sequence: "Alt+Left"
        enabled: pages.depth > 1 && !renameDialog.visible
        onActivated: pages.pop()
    }

    component WindowControl: Button {
        id: control
        required property string mark
        property bool nativeHovered: false
        property bool nativePressed: false
        readonly property bool hot: hovered || nativeHovered
        readonly property bool pressedNow: down || nativePressed
        readonly property bool closing: mark === "close"
        Layout.preferredWidth: 34
        Layout.preferredHeight: 32
        padding: 0
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.name: text
        ToolTip.text: text
        ToolTip.visible: hovered && !nativeHovered
        ToolTip.delay: 700
        Keys.onReturnPressed: clicked()

        background: Rectangle {
            radius: 10
            color: control.closing && (control.hot || control.pressedNow)
                ? (control.pressedNow ? "#AB1722" : "#C42B35")
                : control.pressedNow ? window.accentSoft : control.hot ? window.hoverSurface : "transparent"
            border.color: control.activeFocus ? window.accent : "transparent"
        }
        contentItem: Item {
            Canvas {
                id: markCanvas
                readonly property string mark: control.mark
                readonly property color stroke: control.closing && (control.hot || control.pressedNow)
                    ? "white" : window.ink
                anchors.centerIn: parent
                width: 16
                height: 16
                onStrokeChanged: requestPaint()
                onMarkChanged: requestPaint()
                onPaint: {
                    const context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    context.strokeStyle = stroke
                    context.lineWidth = 1
                    context.beginPath()
                    if (mark === "minimize") {
                        context.moveTo(3.5, 11.5)
                        context.lineTo(12.5, 11.5)
                    } else if (mark === "maximize") {
                        context.rect(3.5, 3.5, 9, 9)
                    } else if (mark === "restore") {
                        context.rect(3.5, 5.5, 7, 7)
                        context.moveTo(5.5, 5.5)
                        context.lineTo(5.5, 3.5)
                        context.lineTo(12.5, 3.5)
                        context.lineTo(12.5, 10.5)
                        context.lineTo(10.5, 10.5)
                    } else {
                        context.moveTo(3.5, 3.5)
                        context.lineTo(12.5, 12.5)
                        context.moveTo(12.5, 3.5)
                        context.lineTo(3.5, 12.5)
                    }
                    context.stroke()
                }
            }
        }
    }

    header: Rectangle {
        color: window.dark ? "#14101620" : "#22ffffff"
        implicitHeight: 48

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 13
            spacing: 2
            Action {
                visible: pages.depth > 1
                text: qsTr("Back to previous page")
                symbol: "arrow-left"
                display: AbstractButton.IconOnly
                onClicked: pages.pop()
            }
            Item {
                id: caption
                Layout.fillWidth: true
                Layout.fillHeight: true
                RowLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 9
                    Glyph {
                        Layout.preferredWidth: 19
                        Layout.preferredHeight: 19
                        source: "qrc:/branding/app.svg"
                        tint: window.ink
                    }
                    TextLabel {
                        text: "Wireless Battery"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Quiet { visible: breadcrumb.visible; text: "/" }
                    Quiet {
                        id: breadcrumb
                        readonly property string current: window.subPage?.breadcrumb || ""
                        visible: current !== ""
                        Layout.maximumWidth: 185
                        Layout.preferredWidth: Math.min(implicitWidth, caption.width - 170)
                        text: current
                        elide: Text.ElideRight
                    }
                    Item { Layout.fillWidth: true }
                }
            }
            Action {
                id: refreshButton
                Layout.leftMargin: 12
                text: window.devices.refreshing ? qsTr("Refreshing…") : qsTr("Refresh")
                display: AbstractButton.IconOnly
                enabled: window.canEdit && !window.devices.refreshing && !refreshSpin.running
                onClicked: {
                    refreshSpin.start()
                    window.devices.refresh()
                }
                contentItem: Item {
                    implicitWidth: 17
                    implicitHeight: 17
                    Glyph {
                        id: refreshGlyph
                        anchors.centerIn: parent
                        width: 17
                        height: width
                        source: window.svg("refresh-cw")
                        tint: refreshButton.enabled || refreshSpin.running ? window.ink : window.muted
                        NumberAnimation on rotation {
                            id: refreshSpin
                            running: false
                            from: 0
                            to: 360
                            duration: 640
                            onFinished: {
                                if (window.devices.refreshing)
                                    refreshSpin.start()
                            }
                        }
                    }
                }
            }
            Action {
                filled: pages.currentItem?.objectName === "globalSettings"
                text: qsTr("Settings")
                symbol: "settings"
                display: AbstractButton.IconOnly
                onClicked: filled ? pages.pop() : pages.push(settingsPage)
            }
            Rectangle {
                Layout.leftMargin: 5
                Layout.rightMargin: 5
                Layout.preferredWidth: 1
                Layout.preferredHeight: 14
                color: window.line
            }
            WindowControl {
                text: qsTr("Minimize")
                mark: "minimize"
                onClicked: window.showMinimized()
            }
            WindowControl {
                id: maximizeControl
                readonly property bool maximized: window.visibility === Window.Maximized
                text: maximized ? qsTr("Restore") : qsTr("Maximize")
                mark: maximized ? "restore" : "maximize"
                onClicked: window.toggleMaximizeRequested()
            }
            WindowControl {
                text: qsTr("Close")
                mark: "close"
                onClicked: window.close()
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: window.line
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            readonly property bool warning: window.devices.error !== "" || window.connectionNotice
            visible: warning || (window.devices.connected && window.devices.snapshot.demo === true)
            implicitHeight: notice.implicitHeight + 18
            color: warning ? window.hoverSurface : window.accentSoft
            Quiet {
                id: notice
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: window.edge
                anchors.rightMargin: window.edge
                wrapMode: Text.Wrap
                color: window.devices.error || (!window.devices.connected && window.backgroundStartFailed)
                    ? window.danger : window.ink
                text: window.devices.error || (window.devices.connected ? qsTr("Demo mode · Sample device readings")
                    : window.backgroundStartFailed ? qsTr("Could not start WirelessBattery.exe from this folder.")
                    : window.connectedOnce ? qsTr("Background disconnected. Reconnecting…")
                    : qsTr("Background connection timed out. Retrying…"))
            }
        }
        StackView {
            id: pages
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            initialItem: overview
            pushEnter: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic }
                NumberAnimation { property: "y"; from: 6; to: 0; duration: 180; easing.type: Easing.OutCubic }
            }
            popEnter: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 155; easing.type: Easing.OutCubic }
            }
            pushExit: Transition {}
            popExit: Transition {}
        }
    }

    Component {
        id: overview
        Item {
            id: home
            readonly property bool single: window.devices.count === 1
            readonly property bool reorderable: window.canEdit && window.devices.count > 1

            ColumnLayout {
                anchors.fill: parent
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: window.edge
                    Layout.rightMargin: window.edge
                    Layout.topMargin: 14
                    Layout.minimumHeight: 36
                    Title { text: qsTr("My devices"); Layout.fillWidth: true }
                    Quiet {
                        visible: window.devices.connected && window.devices.count > 1
                            && home.width >= 720 && !hiddenButton.visible
                        text: qsTr("Drag cards to reorder")
                        font.pixelSize: 12
                    }
                    Action {
                        id: hiddenButton
                        visible: window.devices.hiddenCount > 0 || window.devices.showHidden
                        text: window.devices.showHidden ? qsTr("Hide hidden devices")
                            : qsTr("Show hidden devices (%1)").arg(window.devices.hiddenCount)
                        symbol: window.devices.showHidden ? "eye-off" : "eye"
                        onClicked: window.devices.showHidden = !window.devices.showHidden
                    }
                }

                Flickable {
                    id: scroller
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: Math.max(height, grid.height + 24)
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    clip: true
                    interactive: grid.dragKey === ""
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    Item {
                        id: grid
                        x: (scroller.width - width) / 2
                        y: home.single ? Math.max(8, (scroller.height - height) / 2 - 12) : 0
                        width: home.single ? Math.min(380, scroller.width - 2 * window.edge)
                            : Math.min(1220, scroller.width - 2 * window.edge)
                        height: Math.max(0, Math.ceil(window.devices.count / columns) * stepY - gap)
                        readonly property int columns: {
                            const count = window.devices.count
                            if (count <= 1 || width < 540)
                                return 1
                            return (width >= 960 && count >= 5 || width >= 1040 && count === 3) ? 3 : 2
                        }
                        readonly property int gap: 16
                        readonly property real cardWidth: (width - (columns - 1) * gap) / columns
                        readonly property real cardHeight: home.single ? 326 : scroller.width < 680 ? 208 : 234
                        readonly property real stepX: cardWidth + gap
                        readonly property real stepY: cardHeight + gap
                        property string dragKey: ""
                        property int from: -1
                        property int to: -1
                        property point pointer
                        property point grabOffset
                        property bool saving: false

                        function slotX(slot: int): real { return (slot % columns) * stepX }
                        function slotY(slot: int): real { return Math.floor(slot / columns) * stepY }
                        function displayedSlot(index: int): int {
                            if (from < 0 || index === from)
                                return index
                            if (from < to && index > from && index <= to)
                                return index - 1
                            if (from > to && index >= to && index < from)
                                return index + 1
                            return index
                        }
                        function updateTarget() {
                            const position = mapFromItem(null, pointer)
                            const column = Math.max(0, Math.min(columns - 1, Math.floor((position.x + gap / 2) / stepX)))
                            const row = Math.max(0, Math.floor((position.y + gap / 2) / stepY))
                            to = Math.min(window.devices.count - 1, row * columns + column)
                        }
                        function clearDrag() {
                            dragKey = ""
                            from = -1
                            to = -1
                            saving = false
                        }
                        function drop() {
                            if (from === to || !home.reorderable) {
                                clearDrag()
                                return
                            }
                            saving = true
                            window.devices.moveTo(dragKey, window.devices.deviceAt(to).key)
                        }

                        onColumnsChanged: clearDrag()
                        Connections {
                            target: window.devices
                            function onModelReset() { grid.clearDrag() }
                            function onStateChanged() {
                                if (grid.saving ? !window.devices.busy : !window.canEdit)
                                    grid.clearDrag()
                            }
                        }
                        Timer {
                            interval: 16
                            repeat: true
                            running: grid.dragKey !== "" && !grid.saving && scroller.contentHeight > scroller.height
                            onTriggered: {
                                const y = scroller.mapFromItem(null, grid.pointer).y
                                const delta = y < 48 ? Math.max(-16, (y - 48) / 3)
                                    : y > scroller.height - 48 ? Math.min(16, (y - scroller.height + 48) / 3) : 0
                                if (delta === 0)
                                    return
                                scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height,
                                    scroller.contentY + delta))
                                grid.updateTarget()
                            }
                        }

                        Rectangle {
                            visible: grid.from >= 0
                            x: grid.slotX(Math.max(0, grid.to))
                            y: grid.slotY(Math.max(0, grid.to))
                            width: grid.cardWidth
                            height: grid.cardHeight
                            radius: 22
                            color: window.dark ? "#272496c7" : "#221e93c6"
                            Shape {
                                anchors.fill: parent
                                preferredRendererType: Shape.CurveRenderer
                                ShapePath {
                                    strokeColor: window.accent
                                    strokeWidth: 1
                                    strokeStyle: ShapePath.DashLine
                                    dashPattern: [3, 4]
                                    fillColor: "transparent"
                                    PathRectangle {
                                        x: 1; y: 1
                                        width: grid.cardWidth - 2
                                        height: grid.cardHeight - 2
                                        radius: 21
                                    }
                                }
                            }
                            Quiet {
                                x: 16; y: 12
                                text: qsTr("Drop here")
                                color: window.accent
                                font.pixelSize: 12
                            }
                        }

                        Repeater {
                            model: window.devices
                            delegate: ItemDelegate {
                                id: tile
                                required property int index
                                required property var device
                                readonly property string status: window.statusText(device)
                                readonly property bool floating: grid.dragKey === device.key
                                readonly property int slot: grid.displayedSlot(index)
                                readonly property point pointerInGrid: {
                                    scroller.contentY
                                    return grid.mapFromItem(null, grid.pointer)
                                }
                                readonly property point position: floating && !grid.saving
                                    ? Qt.point(pointerInGrid.x - grid.grabOffset.x, pointerInGrid.y - grid.grabOffset.y)
                                    : Qt.point(grid.slotX(floating ? grid.to : slot), grid.slotY(floating ? grid.to : slot))

                                x: position.x
                                y: position.y
                                width: grid.cardWidth
                                height: grid.cardHeight
                                z: floating ? 2 : hovered || activeFocus ? 1 : 0
                                opacity: floating ? (grid.saving ? 1 : 0.86) : 1
                                scale: floating ? 1.028 : down ? 0.992 : 1
                                transform: Translate { y: tile.hovered && !tile.down && !tile.floating ? -2 : 0
                                    Behavior on y { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                                }
                                Behavior on x {
                                    enabled: grid.from >= 0 && (!tile.floating || grid.saving)
                                    NumberAnimation { duration: 190; easing.type: Easing.OutCubic }
                                }
                                Behavior on y {
                                    enabled: grid.from >= 0 && (!tile.floating || grid.saving)
                                    NumberAnimation { duration: 190; easing.type: Easing.OutCubic }
                                }
                                Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                                Behavior on opacity { NumberAnimation { duration: 140 } }
                                hoverEnabled: true
                                focusPolicy: Qt.TabFocus
                                padding: 12
                                text: device.displayName
                                Accessible.name: text
                                Accessible.description: window.kindText(device.kind) + " · " + window.batteryText(device)
                                    + (status ? " · " + status : "")
                                onClicked: {
                                    if (grid.dragKey === "")
                                        pages.push(detailPage, { deviceKey: device.key })
                                }
                                Keys.onReturnPressed: clicked()
                                onActiveFocusChanged: {
                                    if (!activeFocus)
                                        return
                                    const top = grid.y + grid.slotY(slot)
                                    if (top < scroller.contentY)
                                        scroller.contentY = top
                                    else if (top + height > scroller.contentY + scroller.height)
                                        scroller.contentY = Math.min(scroller.contentHeight - scroller.height,
                                            top + height - scroller.height + 6)
                                }

                                DragHandler {
                                    id: drag
                                    target: null
                                    enabled: home.reorderable || tile.floating
                                    onActiveChanged: {
                                        if (!active)
                                            return
                                        grid.from = tile.index
                                        grid.to = tile.index
                                        grid.pointer = centroid.scenePosition
                                        grid.grabOffset = tile.mapFromItem(null, centroid.scenePressPosition)
                                        grid.dragKey = tile.device.key
                                    }
                                    onActiveTranslationChanged: {
                                        if (!active)
                                            return
                                        grid.pointer = centroid.scenePosition
                                        grid.updateTarget()
                                    }
                                    onGrabChanged: (transition, point) => {
                                        if (!tile.floating || grid.saving)
                                            return
                                        if (transition === PointerDevice.UngrabExclusive)
                                            grid.drop()
                                        else if (transition === PointerDevice.CancelGrabExclusive)
                                            grid.clearDrag()
                                    }
                                }

                                background: GlassSurface {
                                    dark: window.dark
                                    radius: 22
                                    hot: (tile.hovered && !tile.down) || tile.floating
                                    floating: tile.floating
                                    outline: tile.activeFocus || tile.floating ? window.accent : "transparent"
                                }
                                contentItem: Item {
                                    ColumnLayout {
                                        width: parent.width
                                        anchors.centerIn: parent
                                        spacing: 0
                                        Glyph {
                                            opacity: tile.floating || (home.reorderable && tile.hovered) ? 0.75 : 0
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.preferredWidth: 14
                                            Layout.preferredHeight: 14
                                            Layout.topMargin: -12
                                            tint: window.muted
                                            source: window.svg("grip-horizontal")
                                            Behavior on opacity { NumberAnimation { duration: 130 } }
                                        }
                                        DeviceRing {
                                            Layout.alignment: Qt.AlignHCenter
                                            size: home.single ? BatteryRing.Large
                                                : scroller.width < 680 ? BatteryRing.Small : BatteryRing.Medium
                                            device: tile.device
                                        }
                                        TextLabel {
                                            Layout.fillWidth: true
                                            Layout.topMargin: home.single ? 12 : 7
                                            horizontalAlignment: Text.AlignHCenter
                                            text: window.batteryText(tile.device)
                                            color: window.readingColor(tile.device)
                                            font.pixelSize: home.single ? 34 : 27
                                            font.weight: Font.Bold
                                        }
                                        TextLabel {
                                            id: name
                                            Layout.fillWidth: true
                                            Layout.topMargin: 3
                                            horizontalAlignment: Text.AlignHCenter
                                            elide: Text.ElideRight
                                            text: tile.text
                                            font.pixelSize: home.single ? 16 : 15
                                            font.weight: Font.DemiBold
                                        }
                                        Quiet {
                                            Layout.fillWidth: true
                                            Layout.minimumHeight: 18
                                            Layout.topMargin: 4
                                            horizontalAlignment: Text.AlignHCenter
                                            elide: Text.ElideRight
                                            text: tile.status
                                            font.pixelSize: 12
                                        }
                                    }
                                }
                                ToolTip.text: tile.text
                                ToolTip.visible: name.truncated && hovered && !floating
                            }
                        }
                    }

                    GlassSurface {
                        visible: emptyContent.visible
                        anchors.fill: emptyContent
                        anchors.margins: -22
                        dark: window.dark
                        radius: 22
                    }
                    ColumnLayout {
                        id: emptyContent
                        visible: window.devices.count === 0
                        x: (scroller.width - width) / 2
                        y: (scroller.height - height) / 2
                        width: Math.min(scroller.width - 2 * window.edge - 44, 336)
                        spacing: 7
                        Glyph {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 38
                            Layout.preferredHeight: 38
                            tint: window.muted
                            source: window.svg("usb")
                        }
                        SectionTitle {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: window.connectionNotice ? qsTr("Waiting for the background")
                                : !window.ready ? qsTr("Looking for devices…")
                                : window.devices.hiddenCount ? qsTr("All devices are hidden")
                                : qsTr("No battery devices found")
                        }
                        Quiet {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: !window.devices.connected ? qsTr("The panel will connect automatically when the background app starts.")
                                : !window.ready ? qsTr("The background app is scanning your peripherals.")
                                : window.devices.hiddenCount ? qsTr("Enable Show hidden devices to manage them.")
                                : qsTr("Connect a supported device, then select Refresh.")
                        }
                    }
                }
            }
        }
    }

    Component {
        id: detailPage
        PanelPage {
            id: page
            required property string deviceKey
            readonly property var device: {
                window.rowsRevision
                return window.devices.deviceByKey(deviceKey)
            }
            readonly property string displayName: device.displayName || ""
            readonly property int orderIndex: {
                window.rowsRevision
                return window.devices.indexOf(deviceKey)
            }

            contentLimit: 612
            breadcrumb: displayName

            SectionTitle {
                visible: !page.device.key
                text: window.ready ? qsTr("This device is no longer available") : qsTr("Looking for devices…")
            }
            Action {
                visible: !page.device.key
                text: qsTr("Back to devices")
                symbol: "arrow-left"
                onClicked: pages.pop()
            }

            ColumnLayout {
                visible: !!page.device.key
                Layout.fillWidth: true
                spacing: 0
                DeviceRing {
                    Layout.alignment: Qt.AlignHCenter
                    size: BatteryRing.Large
                    device: page.device
                }
                TextLabel {
                    Layout.fillWidth: true
                    Layout.topMargin: 13
                    horizontalAlignment: Text.AlignHCenter
                    text: page.device.key ? window.batteryText(page.device) : ""
                    color: window.readingColor(page.device)
                    font.pixelSize: 42
                    font.weight: Font.Bold
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: Math.min(implicitWidth, page.availableWidth)
                    Layout.maximumWidth: Math.min(implicitWidth, page.availableWidth)
                    spacing: 4
                    TextLabel {
                        Layout.fillWidth: true
                        text: page.displayName
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                        wrapMode: Text.WrapAnywhere
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Action {
                        text: qsTr("Rename")
                        symbol: "pencil"
                        display: AbstractButton.IconOnly
                        enabled: window.canEdit
                        onClicked: renameDialog.begin(page.deviceKey, page.device.alias)
                    }
                }
                Quiet {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: page.device.key
                        ? (window.statusText(page.device) || qsTr("Online")) + " · " + window.linkText(page.device.via)
                        : ""
                }
            }

            GlassGroup {
                visible: !!page.device.key
                heading: qsTr("Connection and status")
                Fact { caption: qsTr("Reported name"); value: page.device.name || qsTr("Unknown") }
                Divider {}
                Fact {
                    caption: qsTr("Connection")
                    value: page.device.key ? window.linkText(page.device.via) : qsTr("Unknown")
                }
                Divider {}
                Fact {
                    caption: qsTr("Battery reading")
                    value: !page.device.key || page.device.percent < 0 ? qsTr("Unknown")
                        : window.cached(page.device) ? qsTr("Last known")
                        : window.devices.snapshot.demo === true ? qsTr("Sample") : qsTr("Live")
                }
                Divider {}
                Fact {
                    caption: qsTr("Last battery update")
                    value: window.timeText(page.device.updated || 0)
                }
                Divider {}
                Fact {
                    caption: qsTr("Charging status")
                    value: window.devices.connected && page.device.charging ? qsTr("Charging") : qsTr("Unknown")
                }
                Divider {}
                Fact {
                    caption: qsTr("Sleep status")
                    value: window.devices.connected && page.device.asleep ? qsTr("Asleep") : qsTr("Unknown")
                }
            }

            GlassGroup {
                visible: !!page.device.key
                heading: qsTr("Visibility and order")
                Toggle {
                    text: qsTr("Show in the panel")
                    checked: !page.device.hidden
                    enabled: window.canEdit
                    onClicked: window.devices.hide(page.deviceKey, checked)
                }
                Divider {}
                Toggle {
                    text: qsTr("Show on the taskbar or in the tray")
                    checked: page.device.surface === true
                    enabled: window.canEdit
                    onClicked: window.devices.setSurface(page.deviceKey, !checked)
                }
                Divider {}
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 54
                    TextLabel { text: qsTr("Device order"); Layout.fillWidth: true }
                    Action {
                        text: qsTr("Move up")
                        symbol: "chevron-up"
                        filled: true
                        enabled: window.canEdit && page.orderIndex > 0
                        onClicked: window.devices.move(page.deviceKey, -1)
                    }
                    Action {
                        text: qsTr("Move down")
                        symbol: "chevron-down"
                        filled: true
                        enabled: window.canEdit && page.orderIndex >= 0 && page.orderIndex < window.devices.count - 1
                        onClicked: window.devices.move(page.deviceKey, 1)
                    }
                }
            }

            GlassGroup {
                visible: !!page.device.key
                heading: qsTr("Diagnostics")
                Fact {
                    caption: "VID / PID"
                    value: window.idText(page.device.vid || 0) + " / " + window.idText(page.device.pid || 0)
                }
                Divider {}
                Fact { caption: qsTr("Driver / protocol"); value: page.device.protocol || qsTr("Unknown") }
            }
        }
    }

    Component {
        id: settingsPage
        PanelPage {
            id: settings
            objectName: "globalSettings"
            readonly property int appliedThreshold: window.devices.snapshot.threshold || 15
            readonly property bool notificationsSupported:
                typeof window.devices.snapshot.lowBatteryNotifications === "boolean"
            property int thresholdDraft: 0 // 0 follows the backend value
            onAppliedThresholdChanged: {
                if (thresholdDraft === appliedThreshold)
                    thresholdDraft = 0
            }
            contentLimit: 632
            contentTop: 21
            breadcrumb: qsTr("Global settings")

            Title { text: settings.breadcrumb }

            GlassGroup {
                heading: qsTr("Appearance")
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 64
                    spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        spacing: 3
                        TextLabel {
                            text: qsTr("Theme mode")
                            font.weight: Font.DemiBold
                        }
                        Quiet {
                            Layout.fillWidth: true
                            text: qsTr("Choose the panel appearance.")
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                        }
                    }
                    Control {
                        padding: 4
                        background: Rectangle {
                            radius: 12
                            color: window.dark ? "#10ffffff" : "#0d0f172a"
                            border.color: window.line
                        }
                        contentItem: RowLayout {
                            spacing: 2
                            Repeater {
                                model: [
                                    { mode: "light", label: qsTr("Light") },
                                    { mode: "dark", label: qsTr("Dark") },
                                    { mode: "system", label: qsTr("Follow system") }
                                ]
                                delegate: Action {
                                    id: themeOption
                                    required property var modelData
                                    readonly property bool selected: appearance.themeMode === modelData.mode
                                    text: modelData.label
                                    implicitHeight: 30
                                    font.pixelSize: 12
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.checkable: true
                                    Accessible.checked: selected
                                    background: Rectangle {
                                        radius: 8
                                        color: themeOption.selected ? (window.dark ? "#29ffffff" : "#edffffff")
                                            : themeOption.hovered || themeOption.down ? window.hoverSurface : "transparent"
                                        border.color: themeOption.activeFocus ? window.accent : "transparent"
                                    }
                                    onClicked: appearance.themeMode = modelData.mode
                                }
                            }
                        }
                    }
                }
            }

            GlassGroup {
                heading: qsTr("Battery alerts")
                Toggle {
                    text: qsTr("Low battery notifications")
                    description: window.devices.connected && !settings.notificationsSupported
                        ? qsTr("Restart Wireless Battery with the updated background app to change this setting.")
                        : qsTr("Show a Windows notification when a device reaches the threshold in tray mode.")
                    checked: window.devices.snapshot.lowBatteryNotifications !== false
                    enabled: window.canEdit && settings.notificationsSupported
                    onClicked: window.devices.setLowBatteryNotifications(!checked)
                }
                Divider {}
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 72
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        spacing: 3
                        TextLabel {
                            Layout.fillWidth: true
                            text: qsTr("Low battery threshold")
                            wrapMode: Text.Wrap
                            font.weight: Font.DemiBold
                        }
                        Quiet {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("The threshold sets battery colors and low battery notifications in tray mode.")
                            font.pixelSize: 12
                        }
                    }
                    Slider {
                        id: threshold
                        Layout.preferredWidth: settings.width < 700 ? 100 : 132
                        from: 1
                        to: 100
                        stepSize: 1
                        enabled: window.canEdit
                        value: settings.thresholdDraft || settings.appliedThreshold
                        onMoved: settings.thresholdDraft = value === settings.appliedThreshold ? 0 : value
                        Accessible.name: qsTr("Low battery threshold (%)")
                        background: Rectangle {
                            x: threshold.leftPadding
                            y: (threshold.height - height) / 2
                            width: threshold.availableWidth
                            height: 5
                            radius: height / 2
                            color: window.line
                            Rectangle {
                                width: parent.width * threshold.visualPosition
                                height: parent.height
                                radius: parent.radius
                                color: window.accent
                            }
                        }
                        handle: Rectangle {
                            x: threshold.leftPadding + threshold.visualPosition * (threshold.availableWidth - width)
                            y: (threshold.height - height) / 2
                            implicitWidth: 16
                            implicitHeight: 16
                            radius: width / 2
                            color: "white"
                            border.color: window.accent
                            border.width: threshold.activeFocus || threshold.pressed ? 3 : 2
                        }
                    }
                    TextLabel {
                        Layout.minimumWidth: 38
                        horizontalAlignment: Text.AlignRight
                        text: qsTr("%1%").arg(threshold.value)
                    }
                    Action {
                        text: qsTr("Apply")
                        filled: true
                        enabled: window.canEdit && settings.thresholdDraft !== 0
                        onClicked: window.devices.setThreshold(settings.thresholdDraft)
                    }
                }
            }

            GlassGroup {
                heading: qsTr("Taskbar and tray")
                Toggle {
                    text: qsTr("Use the taskbar widget")
                    checked: window.devices.snapshot.widget === true
                    enabled: window.canEdit
                    onClicked: window.devices.setWidget(!checked)
                }
                Quiet {
                    visible: window.devices.snapshot.widget === true && window.devices.snapshot.widgetAvailable === false
                    Layout.fillWidth: true
                    Layout.bottomMargin: 10
                    wrapMode: Text.Wrap
                    text: qsTr("The taskbar widget is unavailable. Devices are shown in the tray.")
                }
            }

            Action {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Exit Wireless Battery")
                filled: true
                onClicked: window.devices.exitApplication()
            }
        }
    }

    Dialog {
        id: renameDialog
        property string deviceKey: ""
        anchors.centerIn: parent
        width: 390
        modal: true
        title: qsTr("Rename device")
        standardButtons: Dialog.Save | Dialog.Cancel
        background: Rectangle {
            radius: 16
            color: window.surface
            border.color: window.line
        }

        function begin(key: string, alias: string) {
            deviceKey = key
            aliasField.text = alias
            open()
        }
        onOpened: { aliasField.forceActiveFocus(); aliasField.selectAll() }
        onAccepted: window.devices.rename(deviceKey, aliasField.text)
        contentItem: ColumnLayout {
            TextField {
                id: aliasField
                Layout.fillWidth: true
                maximumLength: 80
                placeholderText: qsTr("Display name")
                Accessible.name: placeholderText
                onAccepted: renameDialog.accept()
            }
            Quiet {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Leave the display name blank to use the reported name.")
            }
        }
    }
}
