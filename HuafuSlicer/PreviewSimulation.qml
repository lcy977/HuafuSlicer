import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import HuafuSlicer 1.0

/**
 * 轨迹仿真：参考切片软件「预览」布局 — 中央 OpenGL、左右侧信息面板、底部分播放条 + 细进度条。
 */
Item {
    id: previewRoot
    anchors.fill: parent

    required property var viewport

    readonly property color accentOrange: "#ff9a3c"
    readonly property color accentRed: "#e74c3c"
    readonly property color textPrimary: "#e8eaed"
    readonly property color textMuted: "#8b93a3"
    readonly property color glassBorder: Qt.rgba(1, 1, 1, 0.12)

    property real playSpeed: 1.0
    /** 已播放的墙钟时间（秒）× playSpeed，用于按 G-code F 推算的仿真时间轴 */
    property real simElapsedWallSec: 0
    property bool loopPlayback: false
    property bool gcodeSidePanelOpen: true
    readonly property int rightPanelWidth: gcodeSidePanelOpen ? 360 : 0

    function syncSimElapsedFromProgress() {
        const T = viewport.trajectoryDurationSec
        simElapsedWallSec = (T > 1e-9) ? (viewport.progress * T) : 0
    }

    function syncGcodeViewToPlaybackLine() {
        if (!viewport.sourceLineCount || !viewport.playbackGcodeWindowText)
            return

        const line = Math.max(viewport.playbackGcodeWindowStartLine,
                              Math.min(viewport.playbackGcodeWindowEndLine, viewport.playbackLine || 1))
        const relativeLine = line - viewport.playbackGcodeWindowStartLine
        if (relativeLine < 0)
            return

        const t = viewport.playbackGcodeWindowText
        let pos = 0
        let lineNum = 0
        while (lineNum < relativeLine && pos < t.length) {
            const n = t.indexOf("\n", pos)
            if (n < 0)
                return
            pos = n + 1
            lineNum++
        }
        let endPos = t.indexOf("\n", pos)
        if (endPos < 0)
            endPos = t.length
        gcodeArea.cursorPosition = pos
        gcodeArea.select(pos, endPos)
        gcodeScroll.ensureVisible(gcodeArea, 8, 100)
    }

    // 穿透到 OpenGL：空白区域旋转/缩放
    MouseArea {
        z: -1
        anchors.fill: parent
        propagateComposedEvents: true
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onPressed: function (mouse) { mouse.accepted = false }
        onReleased: function (mouse) { mouse.accepted = false }
        onPositionChanged: function (mouse) { mouse.accepted = false }
        onClicked: function (mouse) { mouse.accepted = false }
    }

    FileDialog {
        id: gcodeDialog
        title: qsTr("打开 G-code")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("G-code") + " (*.gcode *.gco *.nc *.txt)",
            qsTr("所有文件") + " (*)"
        ]
        onAccepted: viewport.loadGcode(selectedFile)
    }

    Timer {
        id: gcodeFollowTimer
        interval: 90
        repeat: true
        running: playTimer.running && previewRoot.gcodeSidePanelOpen && viewport.pathLoaded
        onTriggered: previewRoot.syncGcodeViewToPlaybackLine()
    }

    Timer {
        id: playTimer
        interval: 33
        repeat: true
        running: false
        onTriggered: {
            const dt = playTimer.interval / 1000.0
            previewRoot.simElapsedWallSec += dt * previewRoot.playSpeed
            const T = viewport.trajectoryDurationSec
            if (T > 1e-9) {
                let p = previewRoot.simElapsedWallSec / T
                if (p >= 1.0) {
                    if (previewRoot.loopPlayback) {
                        previewRoot.simElapsedWallSec = 0
                        viewport.progress = 0
                    } else {
                        viewport.progress = 1
                        previewRoot.simElapsedWallSec = T
                        playTimer.running = false
                    }
                } else {
                    viewport.progress = p
                }
            } else if (viewport.pathLoaded) {
                // 无有效时长时退回按段均分
                const n = Math.max(1, viewport.totalSegments)
                viewport.progress = Math.min(1, viewport.progress + (dt * previewRoot.playSpeed) / (n * 0.05))
                if (viewport.progress >= 1.0 - 1e-9 && !previewRoot.loopPlayback)
                    playTimer.running = false
            }
        }
    }

    // ----- 左侧：面板（特征颜色 + 概要）-----
    Item {
        id: leftGlass
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: bottomChrome.top
        anchors.margins: 14
        anchors.bottomMargin: 10
        width: 300
        z: 4

        Rectangle {
            anchors.fill: parent
            radius: 14
            color: Qt.rgba(0.06, 0.07, 0.10, 0.92)
            border.width: 1
            border.color: glassBorder
        }

        ScrollView {
            id: leftScroll
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            ColumnLayout {
                width: Math.max(180, leftGlass.width - 28)
                x: 8
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Button {
                        text: qsTr("打开")
                        Layout.preferredWidth: 72
                        implicitHeight: 32
                        font.pixelSize: 12
                        onClicked: gcodeDialog.open()
                        background: Rectangle {
                            radius: 8
                            color: parent.down ? accentOrange : (parent.hovered ? Qt.lighter(accentOrange, 1.1) : accentOrange)
                        }
                        contentItem: Label {
                            text: parent.text
                            font.pixelSize: 12
                            font.bold: true
                            color: "#1a1a1a"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Button {
                        text: qsTr("视角")
                        Layout.preferredWidth: 72
                        implicitHeight: 32
                        font.pixelSize: 12
                        onClicked: viewport.resetPreviewCamera()
                        flat: true
                        contentItem: Label {
                            text: parent.text
                            font: parent.font
                            color: textPrimary
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 8
                            color: parent.hovered ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(1, 1, 1, 0.04)
                            border.width: 1
                            border.color: glassBorder
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }

                Label {
                    text: qsTr("特征颜色")
                    font.bold: true
                    font.pixelSize: 14
                    color: textPrimary
                    Layout.topMargin: 4
                }

                Repeater {
                    model: [
                        { n: qsTr("空走"), c: "#ffffff" },
                        { n: qsTr("外壁"), c: "#1e6eff" },
                        { n: qsTr("内壁"), c: "#32c85a" },
                        { n: qsTr("塑料实心填充"), c: "#ff963c" },
                        { n: qsTr("塑料稀疏填充"), c: "#ff963c" },
                        { n: qsTr("表皮/顶底"), c: "#ffd250" },
                        { n: qsTr("支撑"), c: "#82878c" },
                        { n: qsTr("界面支撑"), c: "#22d3ee" },
                        { n: qsTr("裙边/边缘"), c: "#38bdf8" },
                        { n: qsTr("纤维"), c: "#6a6a72" },
                        { n: qsTr("其他挤出"), c: "#ff785a" }
                    ]

                    delegate: RowLayout {
                        required property var modelData
                        spacing: 10
                        Layout.fillWidth: true

                        Rectangle {
                            width: 12
                            height: 12
                            radius: 6
                            color: modelData.c
                            border.width: 1
                            border.color: Qt.rgba(1, 1, 1, 0.2)
                        }
                        Label {
                            text: modelData.n
                            color: Qt.rgba(0.92, 0.93, 0.95, 0.92)
                            font.pixelSize: 12
                            Layout.fillWidth: true
                        }
                        Label {
                            text: "—"
                            color: textMuted
                            font.pixelSize: 11
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    visible: viewport.pathLoaded
                    Layout.topMargin: 6

                    Label {
                        text: qsTr("显示空走")
                        color: textPrimary
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                    Switch {
                        focusPolicy: Qt.NoFocus
                        checked: viewport.previewShowTravel
                        onClicked: viewport.previewShowTravel = checked
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Qt.rgba(1, 1, 1, 0.08)
                    Layout.topMargin: 4
                }

                Label {
                    text: qsTr("仿真概要")
                    font.bold: true
                    font.pixelSize: 13
                    color: textPrimary
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: summaryCol.implicitHeight + 20
                    radius: 10
                    color: Qt.rgba(0, 0, 0, 0.22)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.06)

                    ColumnLayout {
                        id: summaryCol
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 6

                        Label {
                            visible: viewport.pathLoaded
                            text: qsTr("G-code 行数：%1").arg(viewport.sourceLineCount)
                            color: textMuted
                            font.pixelSize: 11
                            Layout.fillWidth: true
                        }
                        Label {
                            visible: viewport.pathLoaded
                            text: qsTr("轨迹段数：%1").arg(viewport.totalSegments)
                            color: textMuted
                            font.pixelSize: 11
                        }
                        Label {
                            visible: viewport.pathLoaded
                            text: qsTr("层数：%1").arg(viewport.layerCount)
                            color: textMuted
                            font.pixelSize: 11
                        }
                        Label {
                            visible: !viewport.pathLoaded
                            text: qsTr("尚未加载 G-code")
                            color: textMuted
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                }

                Label {
                    visible: viewport.pathLoaded && viewport.trajectoryFeatureStats.length > 0
                    text: qsTr("打印耗时（按进给推算）")
                    font.bold: true
                    font.pixelSize: 13
                    color: textPrimary
                    Layout.topMargin: 8
                }

                ColumnLayout {
                    visible: viewport.pathLoaded && viewport.trajectoryFeatureStats.length > 0
                    Layout.fillWidth: true
                    spacing: 8

                    Repeater {
                        model: viewport.trajectoryFeatureStats.length

                        delegate: ColumnLayout {
                            readonly property var row: viewport.trajectoryFeatureStats[index]
                            spacing: 4
                            Layout.fillWidth: true

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Rectangle {
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: row ? row.color : "#888"
                                    border.width: 1
                                    border.color: Qt.rgba(1, 1, 1, 0.2)
                                }
                                Label {
                                    text: row ? row.label : ""
                                    color: textPrimary
                                    font.pixelSize: 12
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: (row && row.timeMin !== undefined ? row.timeMin.toFixed(1) : "0")
                                          + qsTr(" 分钟") + "  (" + (row && row.percent !== undefined
                                                                      ? row.percent.toFixed(1)
                                                                      : "0") + "%)"
                                    color: textMuted
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 6
                                radius: 3
                                color: Qt.rgba(1, 1, 1, 0.08)
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: Math.max(0, parent.width * (row && row.ratio !== undefined ? row.ratio : 0))
                                    radius: 3
                                    color: row ? row.color : "#888"
                                }
                            }
                        }
                    }
                }

                Label {
                    text: qsTr("当前行 %1").arg(viewport.playbackLine)
                    color: accentOrange
                    font.pixelSize: 12
                    font.family: "Consolas, Cascadia Mono, monospace"
                }
            }
        }
    }

    // ----- 层滑块（视口内右侧、G-code 左侧）-----
    ColumnLayout {
        anchors.right: rightGlass.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.rightMargin: gcodeSidePanelOpen ? 16 : 24
        spacing: 6
        z: 3
        width: 56

        Label {
            text: qsTr("层")
            color: textMuted
            font.pixelSize: 11
            Layout.alignment: Qt.AlignHCenter
        }

        Slider {
            id: layerSlider
            Layout.alignment: Qt.AlignHCenter
            orientation: Qt.Vertical
            implicitHeight: 260
            from: 0
            to: Math.max(0, viewport.layerCount - 1)
            stepSize: 1
            enabled: viewport.layerCount > 0
            value: viewport.displayLayer
            onMoved: viewport.displayLayer = Math.round(value)

            Connections {
                target: viewport
                function onPathDataChanged() {
                    layerSlider.to = Math.max(0, viewport.layerCount - 1)
                    layerSlider.value = viewport.displayLayer
                }
            }
        }

        Label {
            text: viewport.layerCount ? String(Math.round(layerSlider.value) + 1) : "—"
            color: textPrimary
            font.pixelSize: 18
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }
        Label {
            text: viewport.layerCount ? viewport.displayLayerHeightMm.toFixed(2) + " mm" : "—"
            color: textMuted
            font.pixelSize: 11
            Layout.alignment: Qt.AlignHCenter
        }
    }

    // ----- 右侧：G-code 面板 -----
    Item {
        id: rightGlass
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: bottomChrome.top
        anchors.margins: 14
        anchors.bottomMargin: 10
        width: previewRoot.rightPanelWidth
        visible: width > 0
        clip: true
        z: 4

        Rectangle {
            anchors.fill: parent
            radius: 14
            color: Qt.rgba(0.03, 0.04, 0.06, 0.94)
            border.width: 1
            border.color: glassBorder
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: qsTr("G-code")
                    font.bold: true
                    font.pixelSize: 14
                    color: textPrimary
                }
                Item {
                    Layout.fillWidth: true
                }
                Label {
                    text: viewport.sourceLineCount
                          ? (qsTr("行 %1 / %2   显示 %3-%4")
                             .arg(viewport.playbackLine)
                             .arg(viewport.sourceLineCount)
                             .arg(viewport.playbackGcodeWindowStartLine)
                             .arg(viewport.playbackGcodeWindowEndLine))
                          : ""
                    color: textMuted
                    font.pixelSize: 11
                    font.family: "Consolas, monospace"
                }
            }

            ScrollView {
                id: gcodeScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                TextArea {
                    id: gcodeArea
                    readOnly: true
                    wrapMode: Text.NoWrap
                    color: "#c8cdd5"
                    selectedTextColor: "#1a1a1a"
                    selectionColor: accentOrange
                    font.family: "Consolas, Cascadia Mono, monospace"
                    font.pixelSize: 11
                    selectByMouse: true
                    background: Rectangle {
                        radius: 8
                        color: Qt.rgba(0, 0, 0, 0.25)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.05)
                    }
                    text: viewport.playbackGcodeWindowText
                }
            }

            Connections {
                target: viewport
                function onPlaybackLineChanged() {
                    previewRoot.syncGcodeViewToPlaybackLine()
                }
            }
        }
    }

    // ----- 底部：播放条 + 红线进度 -----
    Item {
        id: bottomChrome
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 92
        z: 6

        // 中间悬浮播放控制（药丸条）
        Rectangle {
            id: playPill
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: progressRail.top
            anchors.bottomMargin: 12
            implicitWidth: playRow.implicitWidth + 28
            implicitHeight: 48
            radius: height * 0.5
            color: Qt.rgba(0.06, 0.07, 0.10, 0.78)
            border.width: 1
            border.color: glassBorder

            RowLayout {
                id: playRow
                anchors.centerIn: parent
                spacing: 6

                ToolButton {
                    text: "⏮"
                    implicitWidth: 34
                    implicitHeight: 34
                    padding: 0
                    font.pixelSize: 13
                    onClicked: {
                        playTimer.running = false
                        previewRoot.simElapsedWallSec = 0
                        viewport.progress = 0
                    }
                    background: Rectangle {
                        radius: width * 0.5
                        color: parent.down ? Qt.rgba(1, 1, 1, 0.2) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent")
                        border.width: 1
                        border.color: parent.hovered ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.1)
                    }
                    contentItem: Label {
                        text: parent.text
                        color: textPrimary
                        font: parent.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("回到起点")
                    ToolTip.delay: 400
                }
                ToolButton {
                    text: "‹"
                    implicitWidth: 34
                    implicitHeight: 34
                    padding: 0
                    font.pixelSize: 18
                    font.bold: true
                    onClicked: {
                        playTimer.running = false
                        const k = viewport.completedSegmentCountAtProgress(viewport.progress)
                        viewport.progress = viewport.progressAtCompletedSegmentCount(Math.max(0, k - 1))
                        previewRoot.syncSimElapsedFromProgress()
                    }
                    background: Rectangle {
                        radius: width * 0.5
                        color: parent.down ? Qt.rgba(1, 1, 1, 0.2) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent")
                        border.width: 1
                        border.color: parent.hovered ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.1)
                    }
                    contentItem: Label {
                        text: parent.text
                        color: textPrimary
                        font: parent.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("上一段")
                    ToolTip.delay: 400
                }
                ToolButton {
                    text: playTimer.running ? "⏸" : "▶"
                    implicitWidth: 38
                    implicitHeight: 38
                    padding: 0
                    font.pixelSize: 16
                    font.bold: true
                    onClicked: {
                        const willStart = !playTimer.running
                        if (willStart && viewport.pathLoaded && !previewRoot.loopPlayback
                                && viewport.progress >= 1.0 - 1e-9) {
                            viewport.progress = 0
                            previewRoot.simElapsedWallSec = 0
                        }
                        playTimer.running = !playTimer.running
                        if (playTimer.running && willStart)
                            previewRoot.simElapsedWallSec = viewport.progress * viewport.trajectoryDurationSec
                    }
                    background: Rectangle {
                        radius: width * 0.5
                        color: parent.down ? Qt.darker(accentOrange, 1.2) : (parent.hovered ? Qt.lighter(accentOrange, 1.08) : accentOrange)
                        border.width: 1
                        border.color: Qt.rgba(0, 0, 0, 0.25)
                    }
                    contentItem: Label {
                        text: parent.text
                        color: "#171717"
                        font: parent.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: playTimer.running ? qsTr("暂停") : qsTr("播放")
                    ToolTip.delay: 400
                }
                ToolButton {
                    text: "›"
                    implicitWidth: 34
                    implicitHeight: 34
                    padding: 0
                    font.pixelSize: 18
                    font.bold: true
                    onClicked: {
                        playTimer.running = false
                        const n = viewport.totalSegments
                        const k = viewport.completedSegmentCountAtProgress(viewport.progress)
                        viewport.progress = viewport.progressAtCompletedSegmentCount(Math.min(n, k + 1))
                        previewRoot.syncSimElapsedFromProgress()
                    }
                    background: Rectangle {
                        radius: width * 0.5
                        color: parent.down ? Qt.rgba(1, 1, 1, 0.2) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent")
                        border.width: 1
                        border.color: parent.hovered ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.1)
                    }
                    contentItem: Label {
                        text: parent.text
                        color: textPrimary
                        font: parent.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("下一段")
                    ToolTip.delay: 400
                }
                ToolButton {
                    text: "⏭"
                    implicitWidth: 34
                    implicitHeight: 34
                    padding: 0
                    font.pixelSize: 13
                    onClicked: {
                        playTimer.running = false
                        viewport.progress = 1
                        previewRoot.syncSimElapsedFromProgress()
                    }
                    background: Rectangle {
                        radius: width * 0.5
                        color: parent.down ? Qt.rgba(1, 1, 1, 0.2) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent")
                        border.width: 1
                        border.color: parent.hovered ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.1)
                    }
                    contentItem: Label {
                        text: parent.text
                        color: textPrimary
                        font: parent.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("到终点")
                    ToolTip.delay: 400
                }
                ToolButton {
                    text: "🔁"
                    implicitWidth: 34
                    implicitHeight: 34
                    padding: 0
                    font.pixelSize: 13
                    checked: previewRoot.loopPlayback
                    onClicked: previewRoot.loopPlayback = !previewRoot.loopPlayback
                    background: Rectangle {
                        radius: width * 0.5
                        color: parent.checked ? Qt.rgba(1, 0.6, 0.24, 0.28)
                                              : (parent.down ? Qt.rgba(1, 1, 1, 0.2)
                                                             : (parent.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent"))
                        border.width: 1
                        border.color: parent.checked ? Qt.rgba(1, 0.65, 0.35, 0.8)
                                                     : (parent.hovered ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.1))
                    }
                    contentItem: Label {
                        text: parent.text
                        color: parent.checked ? accentOrange : textPrimary
                        font: parent.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("循环播放")
                    ToolTip.delay: 400
                }

                Rectangle {
                    Layout.leftMargin: 6
                    width: 1
                    height: 22
                    color: Qt.rgba(1, 1, 1, 0.12)
                }

                ComboBox {
                    id: speedBox
                    implicitWidth: 76
                    implicitHeight: 32
                    font.pixelSize: 12
                    model: [qsTr("0.5×"), qsTr("1.0×"), qsTr("2.0×"), qsTr("4.0×")]
                    currentIndex: 1
                    background: Rectangle {
                        radius: 8
                        color: Qt.rgba(1, 1, 1, 0.08)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.15)
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("相对真实进给的播放速度")
                    onActivated: function (index) {
                        previewRoot.playSpeed = [0.5, 1, 2, 4][index]
                    }
                }

                Rectangle {
                    width: 1
                    height: 22
                    color: Qt.rgba(1, 1, 1, 0.12)
                }

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: "X " + viewport.trajTipXMm.toFixed(2) + "   Y " + viewport.trajTipYMm.toFixed(2) + "   Z " + viewport.trajTipZMm.toFixed(2) + " mm"
                        color: textPrimary
                        font.pixelSize: 11
                        font.family: "Consolas, monospace"
                    }
                    Label {
                        text: (viewport.pathLoaded && viewport.playbackFeedMmMin > 1)
                              ? (qsTr("进给 F %1").arg(Math.round(viewport.playbackFeedMmMin)))
                              : qsTr("进给 F —")
                        color: textMuted
                        font.pixelSize: 10
                        font.family: "Consolas, monospace"
                    }
                }
            }
        }

        // 底边细进度条（红线）
        Rectangle {
            id: progressRail
            width: Math.min(parent.width - 32, Math.max(420, parent.width * 0.5))
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 10
            height: 30
            radius: 6
            color: Qt.rgba(0.05, 0.06, 0.08, 0.75)
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.08)

            RowLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 10

                Label {
                    text: viewport.pathLoaded ? String(viewport.playbackLine) : "0"
                    color: textMuted
                    font.pixelSize: 11
                    font.family: "Consolas, monospace"
                    Layout.preferredWidth: 52
                }

                Slider {
                    id: lineProgSlider
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    from: 0
                    to: 1
                    live: true
                    wheelEnabled: false
                    stepSize: 0
                    snapMode: Slider.NoSnap
                    padding: 0
                    topPadding: 0
                    bottomPadding: 0
                    implicitHeight: 28
                    value: viewport.progress
                    onMoved: {
                        viewport.progress = value
                        playTimer.running = false
                        previewRoot.syncSimElapsedFromProgress()
                    }
                    onValueChanged: {
                        if (!pressed)
                            return
                        viewport.progress = value
                        playTimer.running = false
                        previewRoot.syncSimElapsedFromProgress()
                    }

                    background: Rectangle {
                        y: lineProgSlider.topPadding + lineProgSlider.availableHeight / 2 - height / 2
                        width: lineProgSlider.availableWidth
                        height: 4
                        radius: 2
                        color: "#252a33"

                        Rectangle {
                            width: lineProgSlider.visualPosition * parent.width
                            height: parent.height
                            radius: 2
                            color: accentRed
                        }
                    }

                    handle: Rectangle {
                        x: lineProgSlider.leftPadding + lineProgSlider.visualPosition * (lineProgSlider.availableWidth - width)
                        y: lineProgSlider.topPadding + lineProgSlider.availableHeight / 2 - height / 2
                        width: 14
                        height: 14
                        radius: 7
                        color: "#f5f5f5"
                        border.width: 2
                        border.color: accentRed
                    }

                    Connections {
                        target: viewport
                        function onProgressChanged() {
                            if (!lineProgSlider.pressed)
                                lineProgSlider.value = viewport.progress
                            if (!lineProgSlider.pressed && !playTimer.running)
                                previewRoot.syncSimElapsedFromProgress()
                        }
                    }
                }

                Label {
                    text: viewport.pathLoaded ? String(viewport.sourceLineCount) : "0"
                    color: textMuted
                    font.pixelSize: 11
                    font.family: "Consolas, monospace"
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: 56
                }
            }

            Connections {
                target: lineProgSlider
                function onPressedChanged() {
                    if (lineProgSlider.pressed)
                        playTimer.running = false
                }
            }
        }
    }

    Connections {
        target: viewport
        function onPathDataChanged() {
            previewRoot.syncSimElapsedFromProgress()
            if (viewport.pathLoaded)
                previewRoot.syncGcodeViewToPlaybackLine()
        }
        function onPlaybackGcodeWindowChanged() {
            previewRoot.syncGcodeViewToPlaybackLine()
        }
    }

    Component.onCompleted: {
        if (viewport.pathLoaded)
            previewRoot.syncGcodeViewToPlaybackLine()
    }
}
