import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Effects
import Youtian3D 1.0

ApplicationWindow {
    id: root
    width: 1360
    height: 840
    minimumWidth: 1024
    minimumHeight: 640
    visible: true
    title: qsTr("YOUTIAN3D")
    color: "#0b0d10"
    //关闭焦点
    // 去掉系统标题栏（最小化 / 最大化 / 关闭在顶栏「设置」右侧）
    flags: Qt.Window | Qt.FramelessWindowHint

    readonly property color accentGreen: "#2ee676"
    readonly property color accentGreenDim: "#1a9e52"
    readonly property color panelBg: "#151820"
    readonly property color titleBarColorBottom: "#1A1A1A"
    readonly property color panelBorder: "#252a33"
    readonly property color textPrimary: "#e8eaed"
    readonly property color textMuted: "#8b93a3"

    font.family: "Microsoft YaHei UI, Segoe UI, sans-serif"
    font.pixelSize: 13

    function panelRadius() {
        return 12
    }

    // 全局圆角半径
    readonly property int windowRadius: 14

    property bool modelListPanelOpen: false
    /** 0：模型准备  1：G-code 轨迹预览仿真 */
    property int workspaceMode: 0

    WindowHelper {
        id: windowHelper
    }

    FileDialog {
        id: importModelDialog
        title: qsTr("导入模型")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("3D 网格") + " (*.stl *.obj)",
            qsTr("STL") + " (*.stl)",
            qsTr("Wavefront OBJ") + " (*.obj)",
            qsTr("所有文件") + " (*)"
        ]
        onAccepted: {
            // 多选导入：用队列串行调用 C++ 导入（C++ 端导入中会拒绝并发请求）
            importQueue = selectedFiles.slice(0)
            importQueueIndex = 0
            importNext()
        }
    }

    FileDialog {
        id: importGcodeDialog
        title: qsTr("导入 G-code")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("G-code") + " (*.gcode *.gco *.nc *.txt)",
            qsTr("所有文件") + " (*)"
        ]
        onAccepted: {
            scene3d.loadGcode(selectedFile)
            root.workspaceMode = 1
        }
    }

    // 多文件导入队列（串行）
    property var importQueue: []
    property int importQueueIndex: 0
    function importNext() {
        if (!importQueue || importQueueIndex >= importQueue.length)
            return
        if (scene3d.importInProgress)
            return
        scene3d.importModel(importQueue[importQueueIndex])
    }

    MessageDialog {
        id: importModelErrorDialog
        title: qsTr("导入失败")
        text: ""
        buttons: MessageDialog.Ok
    }

    Dialog {
        id: confirmDeleteAllModelsDialog
        parent: Overlay.overlay
        modal: true
        title: ""
        width: Math.min(420, root.width - 80)
        anchors.centerIn: parent
        padding: 22
        standardButtons: Dialog.NoButton

        background: Item {
            anchors.fill: parent

            Item {
                id: delDlgGlass
                anchors.fill: parent
                layer.enabled: true
                layer.samples: 4
                layer.smooth: true
                layer.effect: MultiEffect {
                    blurEnabled: true
                    blur: 1.0
                    blurMax: 64
                }

                Rectangle {
                    anchors.fill: parent
                    radius: panelRadius()
                    color: Qt.rgba(0.06, 0.07, 0.09, 0.78)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.14)
                }
            }
        }

        contentItem: ColumnLayout {
            spacing: 18
            width: Math.min(380, root.width - 120)

            Label {
                text: qsTr("确认删除")
                font.pixelSize: 17
                font.bold: true
                color: "#ffffff"
                Layout.topMargin: 4
            }

            Label {
                text: qsTr("确定要删除列表中的全部模型吗？该操作无法撤销。")
                wrapMode: Text.Wrap
                color: "#ffffff"
                font.pixelSize: 13
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                Layout.topMargin: 8
                spacing: 10

                Button {
                    id: btnDelDlgNo
                    text: qsTr("否")
                    focusPolicy: Qt.NoFocus
                    implicitWidth: 88
                    implicitHeight: 36
                    onClicked: confirmDeleteAllModelsDialog.reject()
                    background: Rectangle {
                        radius: 8
                        color: btnDelDlgNo.hovered ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.06)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.2)
                    }
                    contentItem: Label {
                        text: btnDelDlgNo.text
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Button {
                    id: btnDelDlgYes
                    text: qsTr("是")
                    focusPolicy: Qt.NoFocus
                    implicitWidth: 88
                    implicitHeight: 36
                    onClicked: confirmDeleteAllModelsDialog.accept()
                    background: Rectangle {
                        radius: 8
                        color: btnDelDlgYes.hovered ? "#a83232" : "#7a2828"
                        border.width: 1
                        border.color: Qt.rgba(1, 0.4, 0.35, 0.5)
                    }
                    contentItem: Label {
                        text: btnDelDlgYes.text
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        onAccepted: scene3d.clearAllModels()
    }

    Connections {
        target: scene3d
        function onModelImportFinished(ok, message) {
            if (!ok) {
                importModelErrorDialog.text = message
                importModelErrorDialog.open()
            } else {
                modelListPanelOpen = true
            }
            // 无论成功失败都继续导入下一个
            if (importQueue && importQueueIndex < importQueue.length) {
                importQueueIndex += 1
                importNext()
            }
        }
    }

    // ---------- 全窗口背景（圆角由本层一次绘制，避免四角叠小矩形产生毛边/多出一块）----------
    Rectangle {
        anchors.fill: parent
        radius: windowRadius
        color: root.color
        z: -2
    }

    Item {
        anchors.fill: parent
        anchors.margins: 0

        // ---------- 顶栏（可拖动窗口）----------
        Rectangle {
            id: titleBar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 46
            color: titleBarColorBottom
            border.color: panelBorder
            border.width: 0

            // 窗口按钮默认显示（不做 hover 显隐）
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: windowRadius
                radius: windowRadius
                color: titleBarColorBottom
            }

            MouseArea {
                id: titleDragArea
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                z: -1
                hoverEnabled: true
                // 不要用 onPositionChanged 改 root.x/y：在 Windows + 无边框 + OpenGL 下会与 DWM
                // 合成不同步，导致拖动时整块界面闪烁；改用系统级拖动。
                onPressed: function (mouse) {
                    if (mouse.button === Qt.LeftButton)
                        windowHelper.startSystemMove(root)
                }
                onDoubleClicked: function (mouse) {
                    if (mouse.button === Qt.LeftButton) {
                        if (root.visibility === Window.Maximized)
                            root.showNormal()
                        else
                            root.showMaximized()
                    }
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 10
                spacing: 12
                z: 1

                RowLayout {
                    spacing: 10
                    Label {
                        text: "YOUTIAN"
                        font.pixelSize: 14
                        font.bold: true
                        font.letterSpacing: 1.2
                        color: "#e6e6e6"
                    }

                    Rectangle {
                        width: 1
                        height: 20
                        color: panelBorder
                        Layout.alignment: Qt.AlignVCenter
                    }

                    RowLayout {
                        spacing: 4
                        Button {
                            id: tabPrepare
                            text: qsTr("准备")
                            focusPolicy: Qt.NoFocus
                            checkable: true
                            checked: root.workspaceMode === 0
                            implicitHeight: 30
                            padding: 12
                            onClicked: root.workspaceMode = 0
                            background: Rectangle {
                                radius: 6
                                color: tabPrepare.checked ? Qt.rgba(1, 0.6, 0.24, 0.22)
                                      : (tabPrepare.hovered ? "#2a3140" : "transparent")
                                border.width: tabPrepare.checked ? 1 : 0
                                border.color: Qt.rgba(1, 0.6, 0.24, 0.45)
                            }
                            contentItem: Label {
                                text: tabPrepare.text
                                color: tabPrepare.checked ? "#ff9a3c" : textPrimary
                                font.bold: tabPrepare.checked
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Button {
                            id: tabPreview
                            text: qsTr("预览")
                            focusPolicy: Qt.NoFocus
                            checkable: true
                            checked: root.workspaceMode === 1
                            implicitHeight: 30
                            padding: 12
                            onClicked: root.workspaceMode = 1
                            background: Rectangle {
                                radius: 6
                                color: tabPreview.checked ? Qt.rgba(1, 0.6, 0.24, 0.22)
                                      : (tabPreview.hovered ? "#2a3140" : "transparent")
                                border.width: tabPreview.checked ? 1 : 0
                                border.color: Qt.rgba(1, 0.6, 0.24, 0.45)
                            }
                            contentItem: Label {
                                text: tabPreview.text
                                color: tabPreview.checked ? "#ff9a3c" : textPrimary
                                font.bold: tabPreview.checked
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Label {
                        anchors.centerIn: parent
                        text: qsTr("Undefined")
                        color: textMuted
                        font.pixelSize: 13
                    }
                }

                RowLayout {
                    spacing: 8
                    RowLayout {
                        spacing: 6
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: accentGreen
                        }
                        Label {
                            text: "YOUTIAN3D"
                            color: textPrimary
                            font.pixelSize: 12
                        }
                    }
                    Button {
                        id: btnSettings
                        implicitWidth: 46
                        implicitHeight: titleBar.height
                        Layout.preferredHeight: titleBar.height
                        Layout.preferredWidth: implicitWidth
                        Layout.alignment: Qt.AlignVCenter
                        focusPolicy: Qt.NoFocus
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("设置")
                        onClicked: { }
                        background: Rectangle {
                            radius: 0
                            color: btnSettings.hovered ? "#2a3140" : titleBarColorBottom
                            border.width: 0
                        }
                        contentItem: Label {
                            text: "⚙"
                            font.pixelSize: 16
                            color: textPrimary
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Rectangle {
                        width: 1
                        height: 22
                        color: panelBorder
                        Layout.alignment: Qt.AlignVCenter
                    }

                    RowLayout {
                        spacing: 4

                        Button {
                            id: btnMinimize
                            implicitWidth: 46
                            implicitHeight: titleBar.height
                            Layout.preferredHeight: titleBar.height
                            Layout.preferredWidth: implicitWidth
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("—")
                            font.pixelSize: 12
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("最小化")
                            focusPolicy: Qt.NoFocus
                            onClicked: root.showMinimized()
                            background: Rectangle {
                                radius: 0
                                color: btnMinimize.hovered ? "#2a3140" : titleBarColorBottom
                                border.width: 0
                            }
                            contentItem: Label {
                                text: parent.text
                                font: parent.font
                                color: textPrimary
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Button {
                            id: btnMaximize
                            implicitWidth: 46
                            implicitHeight: titleBar.height
                            Layout.preferredHeight: titleBar.height
                            Layout.preferredWidth: implicitWidth
                            Layout.alignment: Qt.AlignVCenter
                            ToolTip.visible: hovered
                            ToolTip.text: root.visibility === Window.Maximized ? qsTr("还原") : qsTr("最大化")
                            onClicked: {
                                if (root.visibility === Window.Maximized)
                                    root.showNormal()
                                else
                                    root.showMaximized()
                            }
                            background: Rectangle {
                                radius: 0
                                color: btnMaximize.hovered ? "#2a3140" : titleBarColorBottom
                                border.width: 0
                            }
                            contentItem: Item {
                                implicitWidth: 14
                                implicitHeight: 14
                                anchors.fill: parent

                                // Windows 风格：未最大化=单窗；最大化(可还原)=叠窗
                                Rectangle {
                                    visible: root.visibility !== Window.Maximized
                                    anchors.centerIn: parent
                                    width: 12
                                    height: 10
                                    color: "transparent"
                                    border.width: 1
                                    border.color: textPrimary
                                }

                                Rectangle {
                                    visible: root.visibility === Window.Maximized
                                    x: parent.width * 0.5 - 6 + 2
                                    y: parent.height * 0.5 - 5 - 1
                                    width: 10
                                    height: 8
                                    color: "transparent"
                                    border.width: 1
                                    border.color: textPrimary
                                }
                                Rectangle {
                                    visible: root.visibility === Window.Maximized
                                    x: parent.width * 0.5 - 6
                                    y: parent.height * 0.5 - 5 + 1
                                    width: 10
                                    height: 8
                                    color: "transparent"
                                    border.width: 1
                                    border.color: textPrimary
                                }
                            }
                        }

                        Button {
                            id: btnClose
                            implicitWidth: 46
                            implicitHeight: titleBar.height
                            Layout.preferredHeight: titleBar.height
                            Layout.preferredWidth: implicitWidth
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("✕")
                            font.pixelSize: 12
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("关闭")
                            onClicked: root.close()
                            focusPolicy: Qt.NoFocus
                            indicator: null
                            background: Rectangle {
                                radius: 0
                                color: btnClose.hovered ? "#c0392b" : titleBarColorBottom
                                border.width: 0
                            }
                            contentItem: Label {
                                focusPolicy: Qt.NoFocus
                                text: parent.text
                                font: parent.font
                                color: textPrimary
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }

            // 顶栏底部分隔线去掉（避免菜单栏与下方之间出现黑色横线）
        }

        // ---------- 主内容区域 ----------
        Item {
            id: mainContent
            anchors.top: titleBar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            // 顶部导入进度条（导入时显示）
            ProgressBar {
                id: importProgress
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 4
                from: 0
                to: 1
                value: 0.5
                indeterminate: true
                visible: scene3d.importInProgress
                z: 100
            }

            // ----- 中间 OpenGL 渲染（占据整个背景）-----
            Rectangle {
                anchors.fill: parent
                radius: panelRadius()
                clip: true
                color: panelBg
                // 去掉边框，与周围融为一体
                border.width: 0
                border.color: "transparent"

                // 顶部圆角遮罩去掉：避免顶边出现 1px 分界线/黑线

                OpenGLViewport {
                    id: scene3d
                    anchors.fill: parent
                    // GL 纹理为直角；略缩进使圆角 clip 内不露直角，避免四角“多出”像素
                    anchors.margins: 2
                    previewMode: root.workspaceMode === 1
                }

                // G-code 异步导入进度（仅顶部窄条，不遮挡整屏以便 Ctrl+右键 平移等）
                Column {
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 16
                    spacing: 6
                    width: Math.min(420, parent.width * 0.48)
                    visible: scene3d.gcodeImportInProgress
                    z: 45

                    Label {
                        width: parent.width
                        text: qsTr("正在导入 G-code… %1%").arg(Math.min(100, Math.round(scene3d.gcodeImportProgress * 100)))
                        color: textPrimary
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                    ProgressBar {
                        width: parent.width
                        height: 8
                        from: 0
                        to: 1
                        value: scene3d.gcodeImportProgress
                        indeterminate: false
                    }
                }

                PreviewSimulation {
                    id: previewSim
                    anchors.fill: parent
                    visible: root.workspaceMode === 1
                    enabled: visible
                    z: 30
                    viewport: scene3d
                }

                // OpenGL 区域的右键菜单（毛玻璃效果）
                Menu {
                    id: sceneContextMenu
                    modal: false
                    dim: false
                    transformOrigin: Menu.TopLeft
                    padding: 0
                    spacing: 0

                    background: Item {
                        anchors.fill: parent
                        implicitWidth: 110
                        implicitHeight: 72

                        Item {
                            anchors.fill: parent
                            layer.enabled: true
                            layer.samples: 4
                            layer.smooth: true
                            layer.effect: MultiEffect {
                                blurEnabled: true
                                blur: 1.0
                                blurMax: 48
                            }

                            Rectangle {
                                anchors.fill: parent
                                radius: 8
                                color: Qt.rgba(0.08, 0.09, 0.12, 0.85)
                                border.width: 1
                                border.color: Qt.rgba(1, 1, 1, 0.1)
                            }
                        }
                    }

                    property int targetModelIndex: -1

                    MenuItem {
                        implicitHeight: 36
                        padding: 0
                        text: qsTr("删除")
                        onClicked: {
                            if (sceneContextMenu.targetModelIndex >= 0) {
                                scene3d.deleteModelAt(sceneContextMenu.targetModelIndex)
                            }
                        }
                        contentItem: Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.text
                            color: "#ff6b6b"
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignLeft
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: parent.hovered ? Qt.rgba(1, 0.3, 0.3, 0.15) : "transparent"
                        }
                    }
                    MenuItem {
                        implicitHeight: 36
                        padding: 0
                        text: qsTr("旋转")
                        onClicked: {
                            if (sceneContextMenu.targetModelIndex >= 0) {
                                scene3d.startRotateModel(sceneContextMenu.targetModelIndex)
                            }
                        }
                        contentItem: Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.text
                            color: "#ffffff"
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignLeft
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: parent.hovered ? Qt.rgba(0.3, 0.5, 1, 0.15) : "transparent"
                        }
                    }
                }

                // 右键轻点命中模型：由 C++ 发信号请求弹出菜单（避免 MouseArea 吃掉右键拖动）
                Connections {
                    target: scene3d
                    function onContextMenuRequested(modelIndex) {
                        if (modelIndex >= 0) {
                            sceneContextMenu.targetModelIndex = modelIndex
                            sceneContextMenu.popup()
                        }
                    }
                }

                // 左下角 XYZ（QML 叠在 GL 纹理之上）
                Item {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 14
                    width: 56
                    height: 56
                    visible: root.workspaceMode === 0
                    z: 20

                    Rectangle {
                        x: 8
                        y: 36
                        width: 36
                        height: 4
                        radius: 2
                        color: "#e74c3c"
                        rotation: -8
                        transformOrigin: Item.BottomLeft
                    }
                    Rectangle {
                        x: 8
                        y: 36
                        width: 36
                        height: 4
                        radius: 2
                        color: "#2ee676"
                        rotation: 8
                        transformOrigin: Item.BottomLeft
                    }
                    Rectangle {
                        x: 8
                        y: 36
                        width: 4
                        height: 36
                        radius: 2
                        color: "#3498db"
                        transformOrigin: Item.BottomLeft
                    }
                }

                // 右下角缩放提示
                Label {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 14
                    visible: root.workspaceMode === 0
                    text: qsTr("滚轮: 缩放 | 右键: 旋转 | Ctrl+右键: 平移")
                    color: textMuted
                    font.pixelSize: 11
                    z: 20
                }

                // 底部悬浮工具条
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 14
                    visible: false
                    height: 44
                    width: viewBarRow.width + 24
                    radius: 22
                    color: "#151820ee"
                    border.color: panelBorder
                    z: 20

                    RowLayout {
                        id: viewBarRow
                        anchors.centerIn: parent
                        spacing: 8

                        Repeater {
                            model: ["🔍", "📷", "⬜", "⬛"]
                            delegate: ToolButton {
                                required property string modelData
                                text: modelData
                                implicitWidth: 36
                                implicitHeight: 36
                                flat: true
                                background: Rectangle {
                                    radius: 18
                                    color: parent.hovered ? "#2a3140" : "transparent"
                                }
                            }
                        }
                    }
                }
            }

            // ----- 左侧配置面板（悬浮在OpenGL上）-----
            Rectangle {
                id: leftPanel
                property int tabIndex: 0
                visible: false  // 隐藏左侧栏

                anchors {
                    left: parent.left
                    top: parent.top
                    bottom: parent.bottom
                    leftMargin: 10
                    rightMargin: 10
                    topMargin: 0  // 顶部往中间靠拢
                    bottomMargin: 0  // 底部往中间靠拢
                }
                width: 316
                radius: panelRadius()
                color: "#151820aa"  // 与OpenGL背景色一致，增加透明度
                border.width: 0  // 去掉外边框
                // 阴影效果
                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: -4
                    anchors.rightMargin: -4
                    anchors.topMargin: -4
                    anchors.bottomMargin: -4
                    radius: parent.radius + 4
                    color: "#30000000"
                    z: -1
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14

                    // ---------- 准备 / 预览（单独一行，与下方参数区分开）----------
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Button {
                            id: btnPrepare
                            Layout.fillWidth: true
                            implicitHeight: 42
                            checkable: true
                            checked: true
                            hoverEnabled: true
                            focusPolicy: Qt.NoFocus
                            background: Rectangle {
                                radius: 21
                                color: btnPrepare.checked ? accentGreen : "#252b38"
                                border.color: btnPrepare.checked ? accentGreen : panelBorder
                                border.width: btnPrepare.checked ? 2 : 1
                            }
                            contentItem: Label {
                                text: qsTr("设置")
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                color: leftPanel.tabIndex === 0 ? "#fff" : textMuted
                                font.pixelSize: 13
                                font.bold: leftPanel.tabIndex === 0
                            }
                        }
                        Button {
                            id: btnPreview
                            Layout.fillWidth: true
                            implicitHeight: 42
                            checkable: true
                            checked: false
                            hoverEnabled: true
                            focusPolicy: Qt.NoFocus
                            onClicked: {
                                btnPreview.checked = true
                                btnPrepare.checked = false
                            }

                            background: Rectangle {
                                radius: 21
                                color: btnPreview.checked ? accentGreen : "#252b38"
                                border.color: btnPreview.checked ? accentGreen : panelBorder
                                border.width: btnPreview.checked ? 2 : 1
                            }
                            contentItem: Label {
                                text: qsTr("预览")
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                color: leftPanel.tabIndex === 0 ? "#fff" : textMuted
                                font.pixelSize: 13
                                font.bold: leftPanel.tabIndex === 0
                            }
                        }
                    }
                                // ---------- 设置 / 模型 + 参数（同一区域）----------
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 10
                        color: "#10141c"
                        border.color: panelBorder
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Button {
                                    id: tabSettingsBtn
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    flat: true
                                    text: qsTr("设置")
                                    onClicked: leftPanel.tabIndex = 0

                                    background: Rectangle {
                                        radius: 10
                                        color: leftPanel.tabIndex === 0 ? accentGreenDim : "#0c0e12"
                                        border.width: 1
                                        border.color: leftPanel.tabIndex === 0 ? accentGreen : panelBorder
                                    }
                                    contentItem: Label {
                                        text: qsTr("设置")
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        color: leftPanel.tabIndex === 0 ? "#fff" : textMuted
                                        font.pixelSize: 13
                                        font.bold: leftPanel.tabIndex === 0
                                    }
                                }

                                Button {
                                    id: tabModelsBtn
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    flat: true
                                    text: qsTr("模型")
                                    onClicked: leftPanel.tabIndex = 1

                                    background: Rectangle {
                                        radius: 10
                                        color: leftPanel.tabIndex === 1 ? accentGreenDim : "#0c0e12"
                                        border.width: 1
                                        border.color: leftPanel.tabIndex === 1 ? accentGreen : panelBorder
                                    }
                                    contentItem: Label {
                                        text: qsTr("模型")
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        color: leftPanel.tabIndex === 1 ? "#fff" : textMuted
                                        font.pixelSize: 13
                                        font.bold: leftPanel.tabIndex === 1
                                    }
                                }
                            }

                            StackLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                currentIndex: leftPanel.tabIndex

                                ScrollView {
                                    id: settingsScroll
                                    clip: true
                                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                                    ColumnLayout {
                                        width: settingsScroll.availableWidth
                                        spacing: 16

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Label {
                                                text: "🖨"
                                                font.pixelSize: 15
                                                Layout.alignment: Qt.AlignVCenter
                                            }
                                            Label {
                                                text: qsTr("打印机")
                                                font.pixelSize: 13
                                                font.bold: true
                                                color: textPrimary
                                                Layout.alignment: Qt.AlignVCenter
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 10
                                            Label {
                                                text: qsTr("类型")
                                                color: textMuted
                                                font.pixelSize: 12
                                                Layout.preferredWidth: 44
                                            }
                                            ComboBox {
                                                id: printerCombo
                                                Layout.fillWidth: true
                                                model: ["SEEKER 3", "SEEKER 2"]
                                                currentIndex: 0
                                                padding: 8
                                                implicitHeight: 36
                                                background: Rectangle {
                                                    radius: 10
                                                    color: "#1e232c"
                                                    border.color: panelBorder
                                                }
                                                contentItem: Label {
                                                    text: printerCombo.displayText
                                                    color: textPrimary
                                                    verticalAlignment: Text.AlignVCenter
                                                    leftPadding: 6
                                                    font.pixelSize: 13
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 12

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Label {
                                                    text: "L 塑料"
                                                    font.bold: true
                                                    font.pixelSize: 11
                                                    color: accentGreen
                                                }
                                                ComboBox {
                                                    Layout.fillWidth: true
                                                    model: ["PETG (AP)", "PLA", "ABS"]
                                                    currentIndex: 0
                                                    padding: 8
                                                    implicitHeight: 34
                                                    background: Rectangle {
                                                        radius: 10
                                                        color: "#1e232c"
                                                        border.color: panelBorder
                                                    }
                                                    contentItem: Label {
                                                        text: parent.displayText
                                                        color: textPrimary
                                                        font.pixelSize: 12
                                                        verticalAlignment: Text.AlignVCenter
                                                        leftPadding: 4
                                                    }
                                                }
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Label {
                                                    text: "R 纤维/塑料"
                                                    font.bold: true
                                                    font.pixelSize: 11
                                                    color: accentGreen
                                                }

                                                ComboBox {
                                                    Layout.fillWidth: true
                                                    model: ["X-CCF", "PETG (AP)", "PLA", "ABS"]
                                                    currentIndex: 0
                                                    padding: 8
                                                    implicitHeight: 34
                                                    background: Rectangle {
                                                        radius: 10
                                                        color: "#1e232c"
                                                        border.color: panelBorder
                                                    }
                                                    contentItem: Label {
                                                        text: parent.displayText
                                                        color: textPrimary
                                                        font.pixelSize: 12
                                                        verticalAlignment: Text.AlignVCenter
                                                        leftPadding: 4
                                                    }
                                                }
                                            }
                                        }

                                        Label {
                                            text: qsTr("打印模式")
                                            font.pixelSize: 11
                                            color: textMuted
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            implicitHeight: 42
                                            radius: 10
                                            color: "#0c0e12"
                                            border.color: panelBorder
                                            border.width: 1

                                            RowLayout {
                                                id: printModeRow
                                                anchors.fill: parent
                                                anchors.margins: 6
                                                spacing: 6
                                                property int modeIndex: 0

                                                Button {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 72
                                                    implicitHeight: 30
                                                    flat: true
                                                    checked: printModeRow.modeIndex === 0
                                                    checkable: true
                                                    onClicked: printModeRow.modeIndex = 0
                                                    background: Rectangle {
                                                        radius: 8
                                                        color: parent.checked ? accentGreenDim : "transparent"
                                                        border.color: parent.checked ? accentGreen : "transparent"
                                                        border.width: parent.checked ? 1 : 0
                                                    }
                                                    contentItem: RowLayout {
                                                        spacing: 4
                                                        anchors.centerIn: parent
                                                        Label {
                                                            text:printModeRow.modeIndex === 0 ? "✓" : " "
                                                            color: accentGreen
                                                            font.bold: true
                                                            font.pixelSize: 12
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                        Label {
                                                            text: qsTr("快速")
                                                            color: printModeRow.modeIndex === 0 ? "#fff" : textMuted
                                                            font.pixelSize: 12
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                    }
                                                }

                                                Button {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 72
                                                    implicitHeight: 30
                                                    flat: true
                                                    checked: printModeRow.modeIndex === 1
                                                    checkable: true
                                                    onClicked: printModeRow.modeIndex = 1
                                                    background: Rectangle {
                                                        radius: 8
                                                        color: parent.checked ? accentGreenDim : "transparent"
                                                        border.color: parent.checked ? accentGreen : "transparent"
                                                        border.width: parent.checked ? 1 : 0
                                                    }
                                                    contentItem: RowLayout {
                                                        spacing: 4
                                                        anchors.centerIn: parent
                                                        Label {
                                                            text:printModeRow.modeIndex === 1 ? "✓" : " "
                                                            color: accentGreen
                                                            font.bold: true
                                                            font.pixelSize: 12
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                        Label {
                                                            text: qsTr("增强")
                                                            color: printModeRow.modeIndex === 1 ? "#fff" : textMuted
                                                            font.pixelSize: 12
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                    }
                                                }

                                                Button {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 72
                                                    implicitHeight: 30
                                                    flat: true
                                                    checked: printModeRow.modeIndex === 2
                                                    checkable: true
                                                    onClicked: printModeRow.modeIndex = 2
                                                    background: Rectangle {
                                                        radius: 8
                                                        color: parent.checked ? accentGreenDim : "transparent"
                                                        border.color: parent.checked ? accentGreen : "transparent"
                                                        border.width: parent.checked ? 1 : 0
                                                    }
                                                    contentItem: RowLayout {
                                                        spacing: 4
                                                        anchors.centerIn: parent
                                                        Label {
                                                            text:printModeRow.modeIndex === 2 ? "✓" : " "
                                                            color: accentGreen
                                                            font.bold: true
                                                            font.pixelSize: 12
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                        Label {
                                                            text: qsTr("强化")
                                                            color: printModeRow.modeIndex === 2 ? "#fff" : textMuted
                                                            font.pixelSize: 12
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Label {
                                            text: qsTr("配置文件")
                                            font.pixelSize: 11
                                            color: textMuted
                                        }
                                        ComboBox {
                                            Layout.fillWidth: true
                                            model: ["PETG (AP)", qsTr("高强度 PETG"), qsTr("粗层快速")]
                                            currentIndex: 0
                                            padding: 8
                                            implicitHeight: 36
                                            background: Rectangle {
                                                radius: 10
                                                color: "#1e232c"
                                                border.color: panelBorder
                                            }
          
                                            contentItem: Label {
                                                text: parent.displayText
                                                color: textPrimary
                                                verticalAlignment: Text.AlignVCenter
                                                font.pixelSize: 13
                                                leftPadding: 6
                                            }
                                        }

                                        Label {
                                            text: qsTr("模板设置")
                                            color: textMuted
                                            font.pixelSize: 11
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 8

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: qsTr("层高")
                                                    color: textPrimary
                                                }
                                                RowLayout {
                                                    spacing: 4
                                                    ToolButton {
                                                        text: "−"
                                                        implicitWidth: 32
                                                        onClicked: layerSpin.value = Math.max(8, layerSpin.value - 2)
                                                        background: Rectangle {
                                                            radius: 6
                                                            color: parent.hovered ? "#353b48" : "#252a33"
                                                        }
                                                    }
                                                    SpinBox {
                                                        id: layerSpin
                                                        from: 8
                                                        to: 40
                                                        value: 12
                                                        stepSize: 2
                                                        editable: true
                                                        implicitWidth: 110
                                                        textFromValue: function (v) { return (v / 100).toFixed(2) + " mm" }
                                                        valueFromText: function (t) { return Math.round(parseFloat(t) * 100) }

                                                        background: Rectangle {
                                                            implicitHeight: 32
                                                            radius: 6
                                                            color: "#1e232c"
                                                            border.color: panelBorder
                                                        }
                                                        contentItem: TextInput {
                                                            text: layerSpin.displayText
                                                            font: layerSpin.font
                                                            color: textPrimary
                                                            horizontalAlignment: Qt.AlignHCenter
                                                            verticalAlignment: Qt.AlignVCenter
                                                            readOnly: !layerSpin.editable
                                                            validator: layerSpin.validator
                                                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                                                            selectByMouse: true

                                                            Binding on text {
                                                                value: layerSpin.displayText
                                                                when: !layerSpin.contentItem.activeFocus
                                                            }
                                                        }
                                                    }
                                                    ToolButton {
                                                        text: "+"
                                                        implicitWidth: 32
                                                        onClicked: layerSpin.value = Math.min(40, layerSpin.value + 2)
                                                        background: Rectangle {
                                                            radius: 6
                                                            color: parent.hovered ? "#353b48" : "#252a33"
                                                        }
                                                    }
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: qsTr("塑料墙")
                                                    color: textPrimary
                                                }
                                                SpinBox {
                                                    from: 1
                                                    to: 8
                                                    value: 2
                                                    editable: true
                                                    implicitWidth: 100
                                                    up.indicator: null
                                                    down.indicator: null
                                                    background: Rectangle {
                                                        implicitHeight: 32
                                                        radius: 6
                                                        color: "#1e232c"
                                                        border.color: panelBorder
                                                    }
                                                    contentItem: TextInput {
                                                        text: parent.displayText
                                                        font: parent.font
                                                        color: textPrimary
                                                        horizontalAlignment: Qt.AlignHCenter
                                            
                                                        verticalAlignment: Qt.AlignVCenter
                                                        readOnly: !parent.editable
                                                        validator: parent.validator
                                                        inputMethodHints: Qt.ImhDigitsOnly
                                                        selectByMouse: true
                                                        Binding on text {
                                                            value: parent.displayText
                                                            when: !parent.contentItem.activeFocus
                                                        }
                                                    }
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: qsTr("塑料轮廓")
                                                    color: textPrimary
                                                }
                                                SpinBox {
                                                    from: 1
                                                    to: 8
                                                    value: 2
                                                    editable: true
                                                    implicitWidth: 100
                                                    background: Rectangle {
                                                        implicitHeight: 32
                                                        radius: 6
                                                        color: "#1e232c"
                                                        border.color: panelBorder
                                                    }
                                                    contentItem: TextInput {
                                                        text: parent.displayText
                                                        font: parent.font
                                                        color: textPrimary
                                                        horizontalAlignment: Qt.AlignHCenter
                                                        verticalAlignment: Qt.AlignVCenter
                                                        readOnly: !parent.editable
                                                        validator: parent.validator
                                                        Binding on text {
                                                            value: parent.displayText
                                                            when: !parent.contentItem.activeFocus
                                                        }
                                                    }
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: qsTr("打印速度")
                                                    color: textPrimary
                                                }
                                                SpinBox {
                                                    id: speedSpin
                                                    from: 50
                                                    to: 500
                                                    value: 220
                                                    stepSize: 10
                                                    editable: true
                                                    implicitWidth: 120
                                                    textFromValue: function (v) { return v + " mm/s" }
                                                    valueFromText: function (t) {
                                                        var n = parseInt(t, 10)
                                                        return isNaN(n) ? speedSpin.value : n
                                                    }
                                                    background: Rectangle {
                                                        implicitHeight: 32
                                                        radius: 6
                                                        color: "#1e232c"
                                                        border.color: panelBorder
                                                    }
                                                    contentItem: TextInput {
                                                        text: speedSpin.displayText
                                                        font: speedSpin.font
                                                        color: textPrimary
                                                        horizontalAlignment: Qt.AlignHCenter
                                                        verticalAlignment: Qt.AlignVCenter
                                                        readOnly: !speedSpin.editable
                                                        validator: speedSpin.validator
                                                        Binding on text {
                                                            value: speedSpin.displayText
                                                            when: !speedSpin.contentItem.activeFocus
                                                        }
                                                    }
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: qsTr("填充图案")
                                                    color: textPrimary
                                                }
                                                ComboBox {
                                                    Layout.preferredWidth: 140
                                                    model: [qsTr("锯齿"), "Zigzag", "Grid", "Honeycomb"]
                                                    currentIndex: 0
                                                    padding: 6
                                                    background: Rectangle {
                                                        radius: 6
                                                        color: "#1e232c"
                                                        border.color: panelBorder
                                                    }
                                                    contentItem: Label {
                                                        text: parent.displayText
                                                        color: textPrimary
                                                        leftPadding: 6
                                                    }
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: qsTr("支撑类型")
                                                    color: textPrimary
                                                }
                                                ComboBox {
                                                    Layout.preferredWidth: 140
                                                    model: [qsTr("网格"), qsTr("树状"), qsTr("无")]
                                                    currentIndex: 0
                                                    padding: 6
                                                    background: Rectangle {
                                                        radius: 6
                                                        color: "#1e232c"
                                                        border.color: panelBorder
                                                    }
                                                    contentItem: Label {
                                                        text: parent.displayText
                                                        color: textPrimary
                                                        leftPadding: 6
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // 模型 Tab 占位
                                Rectangle {
                                    color: "transparent"
                                    Label {
                                        anchors.centerIn: parent
                                        text: qsTr("模型列表与摆放将显示于此")
                                        color: textMuted
                                    }
                                }
                            }
                        }
                    }   
                    Connections {
                        target: btnPrepare
                        function onClicked() {
                            btnPrepare.checked = true
                            btnPreview.checked = false
                        }
                    }
                }
            }

            // ----- 模型列表（右侧第 3 个按钮打开，毛玻璃浮层）-----
            Item {
                id: modelListFlyout
                visible: modelListPanelOpen
                z: 55
                anchors.right: toolSideBar.left
                anchors.rightMargin: 10
                anchors.top: toolSideBar.top
                width: 508
                height: Math.min(540, mainContent.height - 160)
                opacity: modelListPanelOpen ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }

                Item {
                    id: glassBackdrop
                    anchors.fill: parent
                    layer.enabled: true
                    layer.samples: 4
                    layer.smooth: true
                    layer.effect: MultiEffect {
                        blurEnabled: true
                        blur: 1.0
                        blurMax: 64
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: 14
                        color: Qt.rgba(0.07, 0.09, 0.12, 0.58)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.16)
                    }
                }

                ColumnLayout {
                    z: 1
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: qsTr("模型列表")
                            font.pixelSize: 15
                            font.bold: true
                            color: textPrimary
                            Layout.leftMargin: 4
                            Layout.alignment: Qt.AlignVCenter
                        }

                        Item {
                            Layout.fillWidth: true
                        }

                        ToolButton {
                            id: modelListCloseBtn
                            focusPolicy: Qt.NoFocus
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            text: qsTr("✕")
                            font.pixelSize: 14
                            onClicked: modelListPanelOpen = false
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("关闭")
                            ToolTip.delay: 400
                            background: Rectangle {
                                radius: 8
                                color: modelListCloseBtn.hovered ? "#2e323c" : "transparent"
                            }
                            contentItem: Label {
                                text: modelListCloseBtn.text
                                font: modelListCloseBtn.font
                                color: modelListCloseBtn.hovered ? textPrimary : textMuted
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            Layout.preferredWidth: 36
                            text: qsTr("选中")
                            font.pixelSize: 11
                            color: textMuted
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Label {
                            Layout.preferredWidth: 44
                            text: qsTr("激活")
                            font.pixelSize: 11
                            color: textMuted
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Label {
                            Layout.preferredWidth: 40
                            text: qsTr("显示")
                            font.pixelSize: 11
                            color: textMuted
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("名称")
                            font.pixelSize: 11
                            color: textMuted
                        }
                        Label {
                            Layout.preferredWidth: 108
                            text: qsTr("顶点 / 面片")
                            font.pixelSize: 11
                            color: textMuted
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(1, 1, 1, 0.08)
                    }

                    ListView {
                        id: modelListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 2
                        model: scene3d.meshModels

                        delegate: Rectangle {
                            id: meshRowDelegate
                            required property int meshIndex
                            required property string name
                            required property string filePath
                            required property int vertices
                            required property int faces
                            required property bool selected
                            required property bool active
                            required property bool sceneVisible

                            width: modelListView.width
                            height: 40
                            radius: 8
                            color: !sceneVisible ? Qt.rgba(0.12, 0.12, 0.14, 0.55)
                                 : (selected ? Qt.rgba(0.95, 0.22, 0.18, 0.14)
                                    : (active ? Qt.rgba(0.11, 0.32, 0.20, 0.14) : Qt.rgba(1, 1, 1, 0.03)))
                            border.width: sceneVisible && (selected || active) ? 1 : (!sceneVisible ? 1 : 0)
                            border.color: !sceneVisible ? Qt.rgba(1, 1, 1, 0.06)
                                         : (selected ? Qt.rgba(1.0, 0.35, 0.28, 0.45)
                                            : (active ? Qt.rgba(0.24, 0.92, 0.52, 0.42) : "transparent"))

                            // 右键菜单（毛玻璃效果，紧凑样式）
                            Menu {
                                id: meshContextMenu
                                modal: false
                                dim: false
                                transformOrigin: Menu.TopLeft
                                padding: 0
                                spacing: 0

                                background: Item {
                                    anchors.fill: parent
                                    implicitWidth: 110
                                    implicitHeight: 72

                                    // 毛玻璃层
                                    Item {
                                        id: menuGlass
                                        anchors.fill: parent
                                        layer.enabled: true
                                        layer.samples: 4
                                        layer.smooth: true
                                        layer.effect: MultiEffect {
                                            blurEnabled: true
                                            blur: 1.0
                                            blurMax: 48
                                        }

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 8
                                            color: Qt.rgba(0.08, 0.09, 0.12, 0.85)
                                            border.width: 1
                                            border.color: Qt.rgba(1, 1, 1, 0.1)
                                        }
                                    }
                                }

                                MenuItem {
                                    implicitHeight: 36
                                    padding: 0
                                    text: qsTr("删除")
                                    onClicked: {
                                        scene3d.deleteModelAt(meshRowDelegate.meshIndex)
                                    }
                                    contentItem: Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: parent.text
                                        color: "#ff6b6b"
                                        font.pixelSize: 13
                                        horizontalAlignment: Text.AlignLeft
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: parent.hovered ? Qt.rgba(1, 0.3, 0.3, 0.15) : "transparent"
                                    }
                                }
                                MenuItem {
                                    implicitHeight: 36
                                    padding: 0
                                    text: qsTr("旋转")
                                    onClicked: {
                                        scene3d.startRotateModel(meshRowDelegate.meshIndex)
                                    }
                                    contentItem: Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: parent.text
                                        color: "#ffffff"
                                        font.pixelSize: 13
                                        horizontalAlignment: Text.AlignLeft
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: parent.hovered ? Qt.rgba(0.3, 0.5, 1, 0.15) : "transparent"
                                    }
                                }
                            }

                            // 右键点击处理
                            MouseArea {
                                id: rightClickArea
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onClicked: {
                                    if (mouse.button === Qt.RightButton) {
                                        meshContextMenu.popup()
                                    }
                                }
                            }

                            // HoverHandler：与子项内 MouseArea 共存时仍能检测整行悬停
                            HoverHandler {
                                id: meshRowHover
                            }

                            ToolTip {
                                id: meshPathToolTip
                                parent: meshRowDelegate
                                x: Math.max(0, Math.round((meshRowDelegate.width - width) / 2))
                                y: -implicitHeight - 10
                                delay: 1000
                                visible: meshRowHover.hovered && filePath.length > 0
                                padding: 10
                                background: Rectangle {
                                    color: "#1e232c"
                                    radius: 8
                                    border.width: 1
                                    border.color: panelBorder
                                }
                                contentItem: Label {
                                    text: filePath
                                    color: textPrimary
                                    font.pixelSize: 12
                                    wrapMode: Text.WrapAnywhere
                                    width: Math.min(480, Math.max(200, meshRowDelegate.width + 80))
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 8
                                spacing: 6

                                Item {
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 24
                                    Layout.alignment: Qt.AlignVCenter

                                    Rectangle {
                                        id: chkBoxRect
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18
                                        radius: 3
                                        color: meshPickHit.containsMouse ? "#2a2d34" : "#1e2128"
                                        border.width: 1
                                        border.color: selected ? "#5a5e68" : "#3d4149"
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 10
                                            height: 10
                                            radius: 2
                                            color: "#0a0b0e"
                                            border.width: 1
                                            border.color: "#585c64"
                                            visible: selected
                                        }
                                    }

                                    MouseArea {
                                        id: meshPickHit
                                        anchors.fill: chkBoxRect
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: scene3d.setModelSelected(meshIndex, !selected)
                                    }
                                }

                                Item {
                                    Layout.preferredWidth: 44
                                    Layout.preferredHeight: 24
                                    Layout.alignment: Qt.AlignVCenter

                                    Rectangle {
                                        id: radOuter
                                        anchors.centerIn: parent
                                        width: 16
                                        height: 16
                                        radius: 8
                                        color: radHit.containsMouse ? "#2a2d34" : "#1e2128"
                                        border.width: 1
                                        border.color: active ? accentGreen : "#3d4149"
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 8
                                            height: 8
                                            radius: 4
                                            color: accentGreenDim
                                            visible: active
                                        }
                                    }

                                    MouseArea {
                                        id: radHit
                                        anchors.fill: radOuter
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: scene3d.setModelActive(meshIndex)
                                    }
                                }

                                Item {
                                    Layout.preferredWidth: 40
                                    Layout.preferredHeight: 24
                                    Layout.alignment: Qt.AlignVCenter

                                    Rectangle {
                                        id: visChkRect
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18
                                        radius: 3
                                        color: visPickHit.containsMouse ? "#2a2d34" : "#1e2128"
                                        border.width: 1
                                        border.color: sceneVisible ? "#5a5e68" : "#3d4149"
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 10
                                            height: 10
                                            radius: 2
                                            color: "#0a0b0e"
                                            border.width: 1
                                            border.color: "#585c64"
                                            visible: sceneVisible
                                        }
                                    }

                                    MouseArea {
                                        id: visPickHit
                                        anchors.fill: visChkRect
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: scene3d.setModelSceneVisible(meshIndex, !sceneVisible)
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: name
                                    color: !sceneVisible ? textMuted : (active ? accentGreen : textPrimary)
                                    font.bold: active
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                }

                                Label {
                                    Layout.preferredWidth: 108
                                    text: vertices + " / " + faces
                                    color: textMuted
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        CheckBox {
                            id: cbSelectAllMeshes
                            text: qsTr("全部选中")
                            focusPolicy: Qt.NoFocus
                            enabled: scene3d.meshModels.length > 0
                            checked: scene3d.allModelsSelected
                            onClicked: scene3d.setAllModelsSelected(!scene3d.allModelsSelected)
                            indicator: Rectangle {
                                implicitWidth: 18
                                implicitHeight: 18
                                x: cbSelectAllMeshes.leftPadding
                                y: cbSelectAllMeshes.height / 2 - height / 2
                                radius: 3
                                color: cbSelectAllMeshes.down ? "#2d3038"
                                       : (cbSelectAllMeshes.hovered ? "#2a2d34" : "#1e2128")
                                border.width: 1
                                border.color: cbSelectAllMeshes.checked ? "#5a5e68" : "#3d4149"
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 10
                                    height: 10
                                    radius: 2
                                    color: "#0a0b0e"
                                    border.width: 1
                                    border.color: "#585c64"
                                    visible: cbSelectAllMeshes.checked
                                }
                            }
                            contentItem: Label {
                                text: cbSelectAllMeshes.text
                                font.pixelSize: 13
                                color: textPrimary
                                leftPadding: cbSelectAllMeshes.indicator.width + cbSelectAllMeshes.spacing
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                        }

                        Button {
                            id: btnDeleteAllMeshes
                            text: qsTr("全部删除")
                            focusPolicy: Qt.NoFocus
                            enabled: scene3d.meshModels.length > 0
                            implicitHeight: 32
                            padding: 10
                            onClicked: confirmDeleteAllModelsDialog.open()
                            background: Rectangle {
                                radius: 8
                                color: btnDeleteAllMeshes.hovered ? "#7a2a2a" : "#2a1e1e"
                                border.width: 1
                                border.color: btnDeleteAllMeshes.hovered ? "#c0392b" : "#5c3030"
                            }
                            contentItem: Label {
                                text: btnDeleteAllMeshes.text
                                font: btnDeleteAllMeshes.font
                                color: btnDeleteAllMeshes.hovered ? "#fff" : "#e8a0a0"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    Label {
                        visible: modelListView.count === 0
                        Layout.fillWidth: true
                        Layout.topMargin: 20
                        text: qsTr("暂无模型，请先导入")
                        color: textMuted
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            // ----- 右侧工具条（悬浮在OpenGL上）-----
            Rectangle {
                id: toolSideBar
                visible: root.workspaceMode === 0
                anchors {
                    right: parent.right
                    top: parent.top
                    bottom: parent.bottom
                    rightMargin: 10
                    topMargin: 110  // 避开右上角视角立方体
                    bottomMargin: 70  // 避开底部工具条
                }
                width: 54
                radius: panelRadius()
                color: panelBg
                border.width: 0  // 去掉边框

                // 阴影效果
                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: -4
                    anchors.rightMargin: -4
                    anchors.topMargin: -4
                    anchors.bottomMargin: -4
                    radius: parent.radius + 4
                    color: "#30000000"
                    z: -1
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 8

                    Repeater {
                        model: [
                            { t: "+", tip: qsTr("导入模型") },
                            { t: "+", tip: qsTr("导入 G-code") },
                            { t: "↻", tip: qsTr("还原视图") },
                            { t: "⎆", tip: qsTr("选择") },
                            { t: "⊞", tip: qsTr("自动摆放") },
                            { t: "✥", tip: qsTr("移动") },
                            { t: "⤢", tip: qsTr("缩放") },
                            { t: "📏", tip: qsTr("测量") },
                            { t: "✂", tip: qsTr("切割") },
                            { t: "⇄", tip: qsTr("镜像") },
                            { t: "△", tip: qsTr("支撑") },
                            { t: "⋯", tip: qsTr("更多") }
                        ]

                        delegate: Button {
                            required property int index
                            required property var modelData
                            Layout.fillWidth: true
                            // 与侧栏内宽一致：方形按钮（内宽 = 条宽 − 左右边距）
                            implicitHeight: toolSideBar.width - 8
                            focusPolicy: Qt.NoFocus
                            padding: 0
                            text: modelData.t !== undefined ? modelData.t : ""
                            ToolTip.visible: hovered
                            ToolTip.text: modelData.tip
                            ToolTip.delay: 400

                            // 与标题栏「最小化」按钮相同：底 #1A1A1A、悬停 #2a3140、无圆角无边框
                            background: Rectangle {
                                radius: 0
                                color: parent.hovered ? "#2a3140" : titleBarColorBottom
                                border.width: 0
                            }
                            contentItem: Item {
                                readonly property int _slot: Math.floor(Math.min(parent.width, parent.height))
                                Image {
                                    anchors.centerIn: parent
                                    visible: modelData.icon === true
                                    width: Math.round(_slot * 0.54)
                                    height: width
                                    sourceSize: Qt.size(48, 48)
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    source: modelData.icon === true ? modelData.src : ""
                                    asynchronous: true
                                }
                                Label {
                                    anchors.fill: parent
                                    visible: modelData.icon !== true
                                    text: parent.parent.text
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    color: textPrimary
                                    font.pixelSize: Math.max(20, Math.round(parent.height * 0.58))
                                }
                            }
                            onClicked: {
                                if (index === 0)
                                    importModelDialog.open()
                                if (index === 1)
                                    importGcodeDialog.open()
                                // 还原视图：回到默认打开时的视角
                                if (index === 2)
                                    scene3d.resetView()
                                if (index === 3)
                                    modelListPanelOpen = !modelListPanelOpen
                            }
                        }
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }
            }
        }
    }
}
