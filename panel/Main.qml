pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
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
    width: 990
    height: 690
    minimumWidth: 600
    minimumHeight: 460
    title: "Wireless Battery"
    color: canvas

    // Qt 6.8 exposes QStyleHints at runtime and declares it as QObject to qmllint.
    // qmllint disable missing-property
    readonly property bool dark: Qt.styleHints.colorScheme === Qt.Dark
    // qmllint enable missing-property
    readonly property color canvas: dark ? "#17191D" : "#F5F4F1"
    readonly property color surface: dark ? "#21242A" : "#FDFCFA"
    readonly property color hoverSurface: dark ? "#2A2F37" : "#EFEEE9"
    readonly property color ink: dark ? "#F1F2F4" : "#252A32"
    readonly property color muted: dark ? "#B0B6C0" : "#5D6570"
    readonly property color line: dark ? "#373C45" : "#DBDEE2"
    readonly property color accent: dark ? "#A9C8F0" : "#365F91"
    readonly property color accentSoft: dark ? "#2B394C" : "#E7EDF6"
    readonly property color good: dark ? "#9DD8B9" : "#2C6C51"
    readonly property color danger: dark ? "#F0A99A" : "#A24032"
    readonly property int edge: width < 720 ? 24 : 36
    readonly property bool ready: devices.connected && devices.snapshot.poll > 0
    readonly property bool canEdit: devices.connected && !devices.busy
    readonly property bool connectionNotice: !devices.connected
        && (backgroundStartFailed || connectedOnce || connectionWaitElapsed)
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
        if (device.percent < 0)
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
        else if (low(device))
            status.push(qsTr("Low battery"))
        if (cached(device))
            status.push(qsTr("Last known"))
        if (device.hidden)
            status.push(qsTr("Hidden"))
        return status.join(" · ")
    }

    function readingColor(device: var): color {
        if (cached(device) || device.percent < 0)
            return muted
        return low(device) ? danger : device.charging ? good : ink
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
        font.pixelSize: 30
        font.weight: Font.DemiBold
    }

    component SectionTitle: TextLabel {
        font.pixelSize: 17
        font.weight: Font.DemiBold
    }

    component Divider: Rectangle {
        implicitHeight: 1
        Layout.fillWidth: true
        color: window.line
    }

    component Glyph: Image {
        id: glyph
        property color tint: window.ink
        fillMode: Image.PreserveAspectFit
        sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        layer.enabled: true
        layer.effect: MultiEffect {
            // Lucide's black SVG strokes provide the alpha mask for the theme color.
            brightness: 1
            colorization: 1
            colorizationColor: glyph.tint
        }
    }

    component Action: Button {
        id: action
        property string symbol: ""
        property bool filled: false
        implicitHeight: 38
        leftPadding: display === AbstractButton.IconOnly ? 8 : 12
        rightPadding: leftPadding
        spacing: 7
        font.pixelSize: 13
        font.weight: Font.DemiBold
        hoverEnabled: true
        icon.source: symbol ? window.svg(symbol) : ""
        icon.width: 17
        icon.height: 17
        icon.color: !enabled ? window.muted : filled || hovered ? window.accent : window.ink
        palette.buttonText: icon.color
        background: Rectangle {
            radius: 8
            color: action.down || (action.enabled && action.hovered) ? window.hoverSurface
                : action.filled && action.enabled ? window.accentSoft : "transparent"
            border.color: action.activeFocus ? window.accent : "transparent"
            Behavior on color { ColorAnimation { duration: 100 } }
        }
        Accessible.name: text
        ToolTip.text: text
        ToolTip.visible: display === AbstractButton.IconOnly && hovered
        Keys.onReturnPressed: clicked()
    }

    component Meter: Rectangle {
        id: meter
        required property var device
        implicitHeight: 4
        radius: height / 2
        color: window.line
        Rectangle {
            height: meter.height
            width: meter.width * Math.max(0, Math.min(100, meter.device.percent)) / 100
            radius: meter.radius
            color: window.cached(meter.device) ? window.muted
                : window.low(meter.device) ? window.danger
                : meter.device.charging ? window.good : window.accent
        }
    }

    component Toggle: CheckBox {
        id: toggle
        Layout.fillWidth: true
        implicitHeight: 43
        padding: 0
        rightPadding: 56
        nextCheckState: function() { return checkState }
        contentItem: TextLabel {
            text: toggle.text
            color: toggle.enabled ? window.ink : window.muted
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
        }
        indicator: Rectangle {
            x: toggle.width - width
            anchors.verticalCenter: parent.verticalCenter
            width: 38
            height: 21
            radius: height / 2
            color: toggle.checked && toggle.enabled ? window.accent : window.line
            border.color: toggle.activeFocus ? window.accent : "transparent"
            Rectangle {
                x: toggle.checked ? 20 : 3
                y: 3
                width: 15
                height: 15
                radius: height / 2
                color: toggle.checked && toggle.enabled ? window.palette.highlightedText : window.surface
            }
        }
        Keys.onReturnPressed: clicked()
    }

    component Fact: ColumnLayout {
        id: fact
        required property string caption
        required property string value
        Layout.fillWidth: true
        spacing: 4
        Quiet { text: fact.caption }
        TextLabel {
            text: fact.value
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.weight: Font.DemiBold
        }
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
        Layout.preferredWidth: 44
        Layout.fillHeight: true
        padding: 0
        hoverEnabled: true
        Accessible.name: text
        ToolTip.text: text
        ToolTip.visible: hovered && !nativeHovered
        ToolTip.delay: 700
        Keys.onReturnPressed: clicked()

        background: Rectangle {
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
        color: window.surface
        implicitHeight: 56

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: window.edge
            spacing: 0
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
                        Layout.preferredWidth: 25
                        Layout.preferredHeight: 25
                        source: "qrc:/branding/app.svg"
                    }
                    TextLabel {
                        Layout.fillWidth: true
                        text: "Wireless Battery"
                        elide: Text.ElideRight
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                }
            }
            Action {
                Layout.leftMargin: 12
                text: window.devices.refreshing ? qsTr("Refreshing…") : qsTr("Refresh")
                symbol: "refresh-cw"
                filled: true
                display: window.width < 700 ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
                enabled: window.canEdit && !window.devices.refreshing
                onClicked: window.devices.refresh()
            }
            Action {
                Layout.leftMargin: 6
                visible: pages.currentItem?.objectName !== "globalSettings"
                text: qsTr("Settings")
                symbol: "settings"
                display: AbstractButton.IconOnly
                onClicked: pages.push(settingsPage)
            }
            Item { Layout.preferredWidth: 10 }
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
            pushEnter: null
            pushExit: null
            popEnter: null
            popExit: null
        }
    }

    Component {
        id: overview
        Item {
            ColumnLayout {
                width: Math.min(parent.width - 2 * window.edge, 1140)
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 30
                anchors.bottomMargin: 20
                spacing: 20

                RowLayout {
                    Layout.fillWidth: true
                    Title { text: qsTr("Devices"); Layout.fillWidth: true }
                    Action {
                        visible: window.devices.hiddenCount > 0 || window.devices.showHidden
                        text: window.devices.showHidden ? qsTr("Hide hidden devices")
                            : qsTr("Show hidden devices (%1)").arg(window.devices.hiddenCount)
                        symbol: window.devices.showHidden ? "eye-off" : "eye"
                        onClicked: window.devices.showHidden = !window.devices.showHidden
                    }
                }

                GridView {
                    id: grid
                    readonly property int columns: Math.max(1, Math.floor((width + 14) / 235))
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.rightMargin: -14
                    clip: true
                    cellWidth: Math.floor(width / columns)
                    cellHeight: 168
                    model: window.devices
                    keyNavigationWraps: false
                    ScrollBar.vertical: ScrollBar {
                        policy: grid.contentHeight > grid.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                    }
                    delegate: ItemDelegate {
                        id: tile
                        required property var device
                        readonly property string status: window.statusText(device)
                        width: grid.cellWidth - 14
                        height: grid.cellHeight - 14
                        padding: 16
                        hoverEnabled: true
                        text: device.displayName
                        Accessible.name: tile.text
                        Accessible.description: window.kindText(device.kind) + " · " + window.batteryText(device)
                            + (status ? " · " + status : "")
                        onClicked: pages.push(detailPage, { deviceKey: device.key })
                        Keys.onReturnPressed: clicked()
                        background: Rectangle {
                            radius: 12
                            color: tile.down || tile.hovered ? window.hoverSurface : window.surface
                            border.color: tile.activeFocus ? window.accent : "transparent"
                            Behavior on color { ColorAnimation { duration: 100 } }
                        }
                        contentItem: Item {
                            Glyph {
                                width: 42
                                height: 42
                                tint: window.accent
                                source: window.svg(window.deviceIcon(tile.device))
                            }
                            TextLabel {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.topMargin: 3
                                text: window.batteryText(tile.device)
                                color: window.readingColor(tile.device)
                                font.pixelSize: tile.device.percent >= 0 && !tile.device.level ? 28 : 16
                                font.weight: Font.DemiBold
                            }
                            TextLabel {
                                id: name
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: statusLine.top
                                anchors.bottomMargin: 4
                                text: tile.text
                                elide: Text.ElideRight
                                font.weight: Font.DemiBold
                            }
                            Quiet {
                                id: statusLine
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: level.top
                                anchors.bottomMargin: 8
                                text: tile.status
                                color: window.low(tile.device) ? window.danger : window.muted
                                elide: Text.ElideRight
                            }
                            Meter {
                                id: level
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                visible: tile.device.percent >= 0 && !tile.device.level
                                device: tile.device
                            }
                            ToolTip.text: tile.text
                            ToolTip.visible: name.truncated && tile.hovered
                        }
                    }

                    ColumnLayout {
                        visible: window.devices.count === 0
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 36, 365)
                        spacing: 8
                        Glyph {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: 40
                            tint: window.muted
                            source: window.svg("usb")
                        }
                        SectionTitle {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: !window.ready ? qsTr("Looking for devices…")
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
        ScrollView {
            id: page
            required property string deviceKey
            readonly property var device: {
                window.rowsRevision
                return window.devices.deviceByKey(deviceKey)
            }
            readonly property int orderIndex: {
                window.rowsRevision
                return window.devices.indexOf(deviceKey)
            }

            contentWidth: availableWidth
            clip: true
            leftPadding: Math.max(window.edge, (width - 810) / 2)
            rightPadding: leftPadding
            topPadding: 28
            bottomPadding: 28
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: page.availableWidth
                spacing: 12

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
                    spacing: 12
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Glyph {
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 44
                            tint: window.accent
                            source: window.svg(page.device.key ? window.deviceIcon(page.device) : "circle-question-mark")
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Title {
                                Layout.fillWidth: true
                                text: page.device.displayName || ""
                                wrapMode: Text.Wrap
                            }
                            Quiet {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: page.device.key
                                    ? window.kindText(page.device.kind) + " · " + window.linkText(page.device.via) : ""
                            }
                        }
                        Action {
                            text: qsTr("Rename")
                            filled: true
                            enabled: window.canEdit
                            onClicked: renameDialog.begin(page.deviceKey, page.device.alias)
                        }
                    }

                    Divider { }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            SectionTitle { text: qsTr("Battery") }
                            TextLabel {
                                text: page.device.key ? window.batteryText(page.device) : ""
                                color: page.device.key ? window.readingColor(page.device) : window.muted
                                font.pixelSize: 40
                                font.weight: Font.DemiBold
                            }
                        }
                        ColumnLayout {
                            Layout.alignment: Qt.AlignBottom
                            spacing: 3
                            Quiet {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignRight
                                text: page.device.key ? window.statusText(page.device) || qsTr("Online") : ""
                                color: page.device.key && window.low(page.device) ? window.danger : window.muted
                            }
                            Quiet {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignRight
                                text: qsTr("Last battery update · %1").arg(window.timeText(page.device.updated || 0))
                            }
                        }
                    }
                    Loader {
                        active: page.device.percent >= 0 && !page.device.level
                        visible: active
                        Layout.fillWidth: true
                        sourceComponent: Meter { device: page.device }
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 20
                        Fact {
                            caption: qsTr("Charging status")
                            value: window.devices.connected && page.device.charging ? qsTr("Charging") : qsTr("Unknown")
                        }
                        Fact {
                            caption: qsTr("Sleep status")
                            value: window.devices.connected && page.device.asleep ? qsTr("Asleep") : qsTr("Unknown")
                        }
                    }

                    Divider { Layout.topMargin: 6 }
                    SectionTitle { text: qsTr("Visibility and order") }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Toggle {
                            text: qsTr("Show in the panel")
                            checked: !page.device.hidden
                            enabled: window.canEdit
                            onClicked: window.devices.hide(page.deviceKey, checked)
                        }
                        Toggle {
                            text: qsTr("Show on the taskbar or in the tray")
                            checked: page.device.surface === true
                            enabled: window.canEdit
                            onClicked: window.devices.setSurface(page.deviceKey, !checked)
                        }
                    }
                    RowLayout {
                        Action {
                            text: qsTr("Move up")
                            symbol: "chevron-up"
                            enabled: window.canEdit && page.orderIndex > 0
                            onClicked: window.devices.move(page.deviceKey, -1)
                        }
                        Action {
                            text: qsTr("Move down")
                            symbol: "chevron-down"
                            enabled: window.canEdit && page.orderIndex >= 0 && page.orderIndex < window.devices.count - 1
                            onClicked: window.devices.move(page.deviceKey, 1)
                        }
                    }

                    Divider { Layout.topMargin: 6 }
                    SectionTitle { text: qsTr("Diagnostics") }
                    Fact {
                        visible: !!page.device.alias
                        caption: qsTr("Reported name")
                        value: page.device.name || qsTr("Unknown")
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 3
                        columnSpacing: 20
                        Fact { caption: "VID"; value: window.idText(page.device.vid || 0) }
                        Fact { caption: "PID"; value: window.idText(page.device.pid || 0) }
                        Fact { caption: qsTr("Driver / protocol"); value: page.device.protocol || qsTr("Unknown") }
                    }
                }
            }
        }
    }

    Component {
        id: settingsPage
        ScrollView {
            id: settings
            objectName: "globalSettings"
            readonly property int appliedThreshold: window.devices.snapshot.threshold || 15
            property int thresholdDraft: 0 // 0 follows the backend value
            onAppliedThresholdChanged: {
                if (thresholdDraft === appliedThreshold)
                    thresholdDraft = 0
            }
            contentWidth: availableWidth
            clip: true
            leftPadding: Math.max(window.edge, (width - 660) / 2)
            rightPadding: leftPadding
            topPadding: 28
            bottomPadding: 28
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: settings.availableWidth
                spacing: 16
                Title { text: qsTr("Settings") }
                Divider { }
                SectionTitle { text: qsTr("Taskbar and alerts") }
                Toggle {
                    text: qsTr("Use the taskbar widget")
                    checked: window.devices.snapshot.widget === true
                    enabled: window.canEdit
                    onClicked: window.devices.setWidget(!checked)
                }
                Quiet {
                    visible: window.devices.snapshot.widget === true && window.devices.snapshot.widgetAvailable === false
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("The taskbar widget is unavailable. Devices are shown in the tray.")
                }
                TextLabel { text: qsTr("Low battery threshold (%)") }
                RowLayout {
                    SpinBox {
                        from: 1
                        to: 100
                        value: settings.thresholdDraft || settings.appliedThreshold
                        editable: true
                        live: true
                        onValueModified: settings.thresholdDraft = value === settings.appliedThreshold ? 0 : value
                        Accessible.name: qsTr("Low battery threshold (%)")
                    }
                    Action {
                        text: qsTr("Apply")
                        filled: true
                        enabled: window.canEdit && settings.thresholdDraft !== 0
                        onClicked: window.devices.setThreshold(settings.thresholdDraft)
                    }
                }
                Quiet {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("The threshold sets battery colors and low battery notifications in tray mode.")
                }
                Divider { Layout.topMargin: 6 }
                Action {
                    text: qsTr("Exit Wireless Battery")
                    onClicked: window.devices.exitApplication()
                }
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
