import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Effects
import HuafuSlicer 1.0

ApplicationWindow {
    id: root
    width: 1360
    height: 840
    minimumWidth: 1024
    minimumHeight: 640
    visible: true
    title: qsTr("HuafuSlicer")
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

    /** 中文苹方优先；勿用泛名 sans-serif，在部分环境下会落到仿宋/宋体 */
    readonly property string appSansFontStack: "PingFang SC, PingFang TC, PingFang HK, Microsoft YaHei UI, Microsoft YaHei, Segoe UI"
    /** 模型列表面板内统一使用微软雅黑 */
    readonly property string modelListFontFamily: "Microsoft YaHei"
    font.family: appSansFontStack
    font.pixelSize: 14

    /** 与 libslic3r::Model 同步时：删除/清空走 slicerWorkspace，由 C++ 触发视口重建 */
    function meshListUsesSlicerModel() {
        return typeof slicerWorkspace !== "undefined" && slicerWorkspace !== null
    }
    function removeModelRowAt(idx) {
        if (idx < 0)
            return
        if (meshListUsesSlicerModel())
            slicerWorkspace.removeModelObject(idx)
        else
            scene3d.deleteModelAt(idx)
    }
    function clearAllModelsViaDataSource() {
        if (meshListUsesSlicerModel())
            slicerWorkspace.clearAllModelObjects()
        else
            scene3d.clearAllModels()
    }
    function deleteActiveModelsViaDataSource() {
        if (meshListUsesSlicerModel()) {
            const m = scene3d.meshModels
            const arr = []
            for (let i = 0; i < m.length; ++i) {
                if (m[i].active)
                    arr.push(i)
            }
            slicerWorkspace.removeModelObjectsByIndices(arr)
        } else {
            scene3d.deleteActiveModels()
        }
    }

    function panelRadius() {
        return 12
    }

    /** 竖排标签：将文案拆成单字（用于「显示设置」等） */
    function charsForVerticalLabel(s) {
        if (!s || s.length === 0)
            return []
        const out = []
        for (let i = 0; i < s.length; ++i)
            out.push(s.charAt(i))
        return out
    }

    /** 右上角竖条按钮：绿色渐变（与参考「显示代码」标签一致） */
    readonly property color cornerTabGreenTop: "#32d698"
    readonly property color cornerTabGreenBottom: "#0f8f5c"
    readonly property color cornerTabGreenBorder: "#0a6b45"

    // 全局圆角半径
    readonly property int windowRadius: 14

    property bool modelListPanelOpen: false
    /** 右侧「打印设置」栏展开宽度（须容纳「配置文件」+ 加宽下拉 + 保存/编辑/删除 同行） */
    readonly property int printSettingsDockOpenWidth: 468
    /** 侧栏与主区上下留白 */
    readonly property int printDockOuterMarginV: 14
    /** 右侧栏底边上收：为 OpenGL 底部提示文案留出高度，避免遮挡 */
    readonly property int printDockBottomReserve: 56
    /** 与 OpenGLViewport 中视图立方体占位一致：kGizmoMargin(10)+kGizmoSize(130)+余量，避免左侧栏盖住左下角立方体 */
    readonly property int viewCubeBottomReserve: 152
    property bool printSettingsPanelOpen: true
    /** 打印设置子页：0 基础 1 高级 2 支撑 3 材料 4 G-code */
    property int printSettingsTabIndex: 0
    /** 0：模型准备  1：G-code 轨迹预览仿真 */
    property int workspaceMode: 0
    /** 左侧工具：0 默认移动 1 模型旋转 2 测量 3 镜像 */
    property int sideToolMode: 0
    /** OpenGLViewport 已构造完成后再绑定 scene3d.*，避免顶栏/快捷键早于 scene3d 求值导致启动失败 */
    property bool viewportGlReady: false

    /** 整窗 QML 树完成后再置位（比嵌套 Component.onCompleted 更稳，避免部分环境下启动即 ReferenceError） */
    Timer {
        id: viewportGlReadyTimer
        interval: 0
        running: true
        repeat: false
        // 主线程：绑定 viewport；setViewport 内会在已关联 slicerWorkspace 时自动 applySlicerModelToViewport
        onTriggered: {
            workspaceHub.viewport = scene3d
            root.viewportGlReady = true
        }
    }

    /** 视口相关信号与撤销/恢复的统一入口（QML 仅连接本对象） */
    HuafuWorkspaceMessageHub {
        id: workspaceHub
        objectName: "workspaceMessageHub"
    }

    WindowHelper {
        id: windowHelper
    }

    FileDialog {
        id: importModelDialog
        parentWindow: root
        title: qsTr("导入模型")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("3D 网格") + " (*.stl *.obj)",
            qsTr("STL") + " (*.stl)",
            qsTr("Wavefront OBJ") + " (*.obj)",
            qsTr("所有文件") + " (*)"
        ]
        onAccepted: {
            // 多选导入：由 WorkspaceResponseHub 串行调度（C++ 端拒绝并发导入）
            workspaceResponses.enqueueModelImports(selectedFiles.slice(0))
        }
    }

    FileDialog {
        id: importGcodeDialog
        parentWindow: root
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

    FolderDialog {
        id: saveAllModelsFolderDialog
        parentWindow: root
        title: qsTr("选择文件夹以保存全部模型")
        acceptLabel: qsTr("确定")
        rejectLabel: qsTr("取消")
        onAccepted: {
            workspaceResponses.runBulkMeshSaveExport(selectedFolder)
        }
    }

    /** 网格写盘：右键保存 / 另存为 / 侧栏保存全部（主线程仅快照顶点，写盘在工作线程）；由 WorkspaceResponseHub 置位 */
    property bool meshFileExportBusy: false

    /** main.cpp 自测入口：须保留于根对象供 QMetaObject::invokeMethod 调用 */
    function selfTestOpenNativeSaveDialog() {
        workspaceResponses.selfTestOpenNativeSaveDialog()
    }

    Rectangle {
        id: meshFileSaveBusyLayer
        parent: root.contentItem
        anchors.fill: parent
        visible: root.meshFileExportBusy
        z: 100000
        color: Qt.rgba(0, 0, 0, 0.38)
        Column {
            anchors.centerIn: parent
            spacing: 14
            width: 300
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("保存中…")
                color: "#ffffff"
                font.pixelSize: 15
                font.family: root.appSansFontStack
            }
            ProgressBar {
                width: parent.width
                from: 0
                to: 1
                indeterminate: true
            }
        }
    }

    Popup {
        id: meshExportSuccessToast
        parent: root.contentItem
        padding: 18
        width: Math.min(360, root.width - 80)
        modal: false
        dim: false
        focus: false
        closePolicy: Popup.CloseOnEscape
        anchors.centerIn: parent
        z: 100001
        background: Rectangle {
            radius: 12
            color: Qt.rgba(0.07, 0.09, 0.12, 0.78)
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.18)
        }
        contentItem: Column {
            spacing: 10
            width: Math.min(324, root.width - 80 - meshExportSuccessToast.padding * 2)
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("保存成功")
                color: accentGreen
                font.pixelSize: 17
                font.bold: true
                font.family: root.appSansFontStack
            }
        }
        onOpened: meshExportSuccessCloseTimer.restart()
    }

    Timer {
        id: meshExportSuccessCloseTimer
        interval: 1600
        repeat: false
        onTriggered: meshExportSuccessToast.close()
    }

    MessageDialog {
        id: meshExportFailDialog
        title: qsTr("保存失败")
        text: ""
        buttons: MessageDialog.Ok
    }

    /** 模型另存为/导出：使用 C++ QFileDialog（QML FileDialog 在无边框主窗下常无法弹出） */

    /**
     * 模型右键菜单：Popup + MouseArea（避免 MenuItem 吞点击）；毛玻璃透明背景，删除行悬停红色。
     */
    Popup {
        id: meshContextSheet
        parent: root.contentItem
        padding: 0
        width: 168
        height: 108
        modal: false
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property int targetMeshIndex: -1

        function positionAtWindow(winX, winY) {
            const margin = 8
            const pw = meshContextSheet.parent ? meshContextSheet.parent.width : root.width
            const ph = meshContextSheet.parent ? meshContextSheet.parent.height : root.height
            const maxX = pw - width - margin
            const maxY = ph - height - margin
            x = Math.round(Math.min(Math.max(margin, winX), maxX))
            y = Math.round(Math.min(Math.max(margin, winY), maxY))
        }

        function openAtWindow(winX, winY, meshIndex) {
            targetMeshIndex = meshIndex
            positionAtWindow(winX, winY)
            open()
        }

        background: Item {
            implicitWidth: 168
            implicitHeight: 108
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

        contentItem: Column {
            spacing: 0
            Rectangle {
                width: 168
                height: 36
                color: meshCtxSaveRow.containsMouse ? Qt.rgba(0.2, 0.55, 0.4, 0.22) : "transparent"
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("保存")
                    font.family: root.appSansFontStack
                    font.pixelSize: 13
                    color: "#a8e6cf"
                }
                MouseArea {
                    id: meshCtxSaveRow
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        const idx = meshContextSheet.targetMeshIndex
                        workspaceResponses.meshSaveDbg("meshContextSheet 保存 row idx=" + idx)
                        meshContextSheet.close()
                        if (idx >= 0)
                            workspaceResponses.openMeshPrimarySave(idx)
                    }
                }
            }
            Rectangle {
                width: 168
                height: 36
                color: meshCtxSaveAsRow.containsMouse ? Qt.rgba(0.3, 0.5, 1, 0.15) : "transparent"
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("另存为…")
                    font.family: root.appSansFontStack
                    font.pixelSize: 13
                    color: "#d8dee9"
                }
                MouseArea {
                    id: meshCtxSaveAsRow
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        const idx = meshContextSheet.targetMeshIndex
                        workspaceResponses.meshSaveDbg("meshContextSheet 另存为 row idx=" + idx)
                        meshContextSheet.close()
                        if (idx >= 0)
                            workspaceResponses.openMeshSaveAsNative(idx)
                    }
                }
            }
            Rectangle {
                width: 168
                height: 36
                color: meshCtxDelRow.containsMouse ? Qt.rgba(1, 0.3, 0.3, 0.15) : "transparent"
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("删除")
                    font.family: root.appSansFontStack
                    font.pixelSize: 13
                    color: "#ff6b6b"
                }
                MouseArea {
                    id: meshCtxDelRow
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        const idx = meshContextSheet.targetMeshIndex
                        workspaceResponses.meshSaveDbg("meshContextSheet 删除 row idx=" + idx)
                        meshContextSheet.close()
                        if (idx >= 0)
                            root.removeModelRowAt(idx)
                    }
                }
            }
        }
    }

    /** 模型列表名称展示：最多 18 个字符，超出加省略号 */
    function meshNameDisplay18(s) {
        if (!s)
            return ""
        if (s.length <= 18)
            return s
        return s.substring(0, 18) + "..."
    }

    Shortcut {
        sequence: StandardKey.Delete
        enabled: root.workspaceMode === 0 && workspaceResponses.anyMeshActive()
        onActivated: confirmDeleteActiveModelsDialog.open()
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
                font.pixelSize: 14
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

        onAccepted: root.clearAllModelsViaDataSource()
    }

    Dialog {
        id: confirmDeleteActiveModelsDialog
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
                text: qsTr("确定要删除当前已激活的模型吗？该操作无法撤销。")
                wrapMode: Text.Wrap
                color: "#ffffff"
                font.pixelSize: 14
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                Layout.topMargin: 8
                spacing: 10

                Button {
                    id: btnDelActiveDlgNo
                    text: qsTr("否")
                    focusPolicy: Qt.NoFocus
                    implicitWidth: 88
                    implicitHeight: 36
                    onClicked: confirmDeleteActiveModelsDialog.reject()
                    background: Rectangle {
                        radius: 8
                        color: btnDelActiveDlgNo.hovered ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.06)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.2)
                    }
                    contentItem: Label {
                        text: btnDelActiveDlgNo.text
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Button {
                    id: btnDelActiveDlgYes
                    text: qsTr("是")
                    focusPolicy: Qt.NoFocus
                    implicitWidth: 88
                    implicitHeight: 36
                    onClicked: confirmDeleteActiveModelsDialog.accept()
                    background: Rectangle {
                        radius: 8
                        color: btnDelActiveDlgYes.hovered ? "#a83232" : "#7a2828"
                        border.width: 1
                        border.color: Qt.rgba(1, 0.4, 0.35, 0.5)
                    }
                    contentItem: Label {
                        text: btnDelActiveDlgYes.text
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        onAccepted: root.deleteActiveModelsViaDataSource()
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
                        text: "Huafu"
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
                            text: "HuafuSlicer"
                            color: textPrimary
                            font.pixelSize: 12
                        }
                    }

                    RowLayout {
                        spacing: 2
                        Layout.alignment: Qt.AlignVCenter
                        Button {
                            id: btnUndo
                            implicitHeight: 28
                            implicitWidth: 52
                            Layout.alignment: Qt.AlignVCenter
                            focusPolicy: Qt.NoFocus
                            enabled: root.viewportGlReady ? workspaceHub.undoAvailable : false
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("撤销") + " (Ctrl+Z)"
                            onClicked: workspaceHub.undo()
                            background: Rectangle {
                                radius: 4
                                color: !btnUndo.enabled ? titleBarColorBottom
                                       : (btnUndo.hovered ? "#2a3140" : "#252a33")
                                border.width: 1
                                border.color: panelBorder
                            }
                            contentItem: Label {
                                text: qsTr("撤销")
                                font.pixelSize: 12
                                color: btnUndo.enabled ? textPrimary : textMuted
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Button {
                            id: btnRedo
                            implicitHeight: 28
                            implicitWidth: 52
                            Layout.alignment: Qt.AlignVCenter
                            focusPolicy: Qt.NoFocus
                            enabled: root.viewportGlReady ? workspaceHub.redoAvailable : false
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("恢复") + " (Ctrl+Shift+Z)"
                            onClicked: workspaceHub.redo()
                            background: Rectangle {
                                radius: 4
                                color: !btnRedo.enabled ? titleBarColorBottom
                                       : (btnRedo.hovered ? "#2a3140" : "#252a33")
                                border.width: 1
                                border.color: panelBorder
                            }
                            contentItem: Label {
                                text: qsTr("恢复")
                                font.pixelSize: 12
                                color: btnRedo.enabled ? textPrimary : textMuted
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
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

            // ----- 中间 OpenGL 渲染（全宽；准备模式下竖工具条透明叠在视口上）-----
            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.left: parent.left
                anchors.leftMargin: 0
                radius: panelRadius()
                clip: true
                color: panelBg
                // 去掉边框，与周围融为一体
                border.width: 0
                border.color: "transparent"

                // 顶部圆角遮罩去掉：避免顶边出现 1px 分界线/黑线

                OpenGLViewport {
                    id: scene3d
                    objectName: "scene3d"
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

                // 右键菜单：HuafuWorkspaceMessageHub 转发信号，由 WorkspaceResponseHub 统一响应

                // 右下角缩放提示
                Label {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 14
                    visible: root.workspaceMode === 0
                    text: root.sideToolMode === 4
                          ? qsTr("滚轮: 缩放已激活模型 | 右键: 旋转视图 | Ctrl+右键: 平移")
                          : qsTr("滚轮: 缩放视图 | 右键: 旋转 | Ctrl+右键: 平移")
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

            // ----- 右侧「打印设置」栏（可折叠）-----
            Item {
                id: printSettingsDock
                z: 44
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.topMargin: root.printDockOuterMarginV
                anchors.bottomMargin: root.printDockOuterMarginV + 10 + root.printDockBottomReserve
                width: (printSettingsPanelOpen && root.workspaceMode === 0) ? printSettingsDockOpenWidth : 0
                Behavior on width {
                    NumberAnimation {
                        duration: 220
                        easing.type: Easing.OutCubic
                    }
                }

                /** 侧栏主底与标题栏一致；控件格略提亮以便区分 */
                readonly property color printBarBg: root.titleBarColorBottom
                readonly property color printBarBorder: root.panelBorder
                readonly property color printBarHover: "#2a2d34"
                readonly property color printBarCell: "#222222"
                readonly property color printPanelBg: printBarBg
                readonly property color printPanelBorder: printBarBorder
                readonly property color printInputBg: printBarCell
                readonly property color printInputBorder: printBarBorder
                /** 右侧栏可点控件：淡白描边；按下时整面+描边（底 #15AC70） */
                readonly property color printCtrlBorder: Qt.rgba(1, 1, 1, 0.32)
                readonly property color printCtrlPressFill: "#15AC70"
                readonly property color printCtrlPressBorder: "#0f8a5a"
                /** 页签选中：与按下同色 #15AC70 + 同色描边 */
                readonly property color printTabActiveBg: printCtrlPressFill
                readonly property color printTabAccent: printCtrlPressBorder
                /** 下拉列表：黑底微透明 */
                readonly property color printComboPopupBg: Qt.rgba(0, 0, 0, 0.88)
                readonly property color printComboPopupBorder: Qt.rgba(1, 1, 1, 0.18)
                readonly property color printComboPopupHighlight: Qt.rgba(1, 1, 1, 0.12)
                /** 右侧栏文案统一纯白 */
                readonly property color printPanelText: "#ffffff"
                readonly property int printDockCornerRadius: root.panelRadius()
                /** 数字框、下拉等统一宽度（在原先基础上加宽约 1/3） */
                readonly property int printInputControlWidth: 185
                /** 行尾单位列宽，无单位时用占位对齐 */
                readonly property int printInputSuffixColumnWidth: 48
                /** 右侧栏内容区统一水平内边距（与标题、滚动区、底栏对齐） */
                readonly property int printPanelPaddingH: 8
                /** 数据行左侧参数名最大宽度：勿用 fillWidth，否则会挤占数字框并把单位顶出裁切区 */
                readonly property int printSettingParamColumnWidth: 120
                readonly property int printSettingRowLeadInset: 22
                readonly property int printGroupGap: 18
                readonly property int printRowGap: 8
                /** 右侧面板内全部文字（标题/分区/标签/数值/单位/页签/按钮）统一字号 */
                readonly property int printPanelFontPx: 14

                /** 数据行左侧缩进（分区标题不缩进，形成层次） */
                component PrintSettingRowLead : Item {
                    Layout.preferredWidth: printSettingsDock.printSettingRowLeadInset
                    Layout.minimumWidth: printSettingsDock.printSettingRowLeadInset
                    implicitHeight: 1
                }

                /** 可键盘输入的数字框 + 右侧上下微调（替代默认加减样式） */
                component PrintNumSpin : SpinBox {
                    id: psNum
                    editable: true
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    implicitWidth: printSettingsDock.printInputControlWidth
                    implicitHeight: 36
                    Layout.alignment: Qt.AlignVCenter
                    wheelEnabled: true
                    padding: 0
                    leftPadding: printSettingsDock.printPanelPaddingH
                    rightPadding: 24
                    topPadding: 3
                    bottomPadding: 3
                    font.family: root.appSansFontStack
                    font.pixelSize: printSettingsDock.printPanelFontPx
                    font.hintingPreference: Font.PreferFullHinting
                    palette.text: printSettingsDock.printPanelText
                    background: Rectangle {
                        implicitWidth: printSettingsDock.printInputControlWidth
                        implicitHeight: 36
                        radius: 4
                        color: psNum.pressed ? printSettingsDock.printCtrlPressFill
                              : (psNum.hovered ? printSettingsDock.printBarHover : printSettingsDock.printInputBg)
                        border.width: 1
                        border.color: psNum.pressed ? printSettingsDock.printCtrlPressBorder : printSettingsDock.printCtrlBorder
                    }
                    up.indicator: Rectangle {
                        implicitWidth: 20
                        implicitHeight: Math.max(1, (psNum.height - 2) * 0.5)
                        x: psNum.width - width - 1
                        y: 1
                        color: up.pressed ? printSettingsDock.printCtrlPressFill
                              : (up.hovered ? printSettingsDock.printBarHover : printSettingsDock.printBarCell)
                        border.width: 1
                        border.color: up.pressed ? printSettingsDock.printCtrlPressBorder : printSettingsDock.printCtrlBorder
                        Text {
                            anchors.centerIn: parent
                            text: "\u25b2"
                            font.family: root.appSansFontStack
                            font.pixelSize: printSettingsDock.printPanelFontPx
                            color: printSettingsDock.printPanelText
                        }
                    }
                    down.indicator: Rectangle {
                        implicitWidth: 20
                        implicitHeight: Math.max(1, (psNum.height - 2) * 0.5)
                        x: psNum.width - width - 1
                        y: psNum.height * 0.5
                        color: down.pressed ? printSettingsDock.printCtrlPressFill
                              : (down.hovered ? printSettingsDock.printBarHover : printSettingsDock.printBarCell)
                        border.width: 1
                        border.color: down.pressed ? printSettingsDock.printCtrlPressBorder : printSettingsDock.printCtrlBorder
                        Text {
                            anchors.centerIn: parent
                            text: "\u25bc"
                            font.family: root.appSansFontStack
                            font.pixelSize: printSettingsDock.printPanelFontPx
                            color: printSettingsDock.printPanelText
                        }
                    }
                }

                Item {
                    id: printDockShell
                    anchors.fill: parent
                    clip: true

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: -3
                        anchors.rightMargin: -3
                        anchors.topMargin: -3
                        anchors.bottomMargin: -3
                        radius: printSettingsDock.printDockCornerRadius + 3
                        color: Qt.rgba(0, 0, 0, 0.35)
                        z: -1
                    }

                    Item {
                        id: printDockGlassLayer
                        anchors.fill: parent
                        z: 0
                        layer.enabled: true
                        layer.samples: 4
                        layer.smooth: true
                        layer.effect: MultiEffect {
                            blurEnabled: true
                            blur: 1.0
                            blurMax: 56
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: printSettingsDock.printDockCornerRadius
                            color: Qt.rgba(0.09, 0.10, 0.13, 0.68)
                        }
                    }

                    /** Basic 主题下 Label 会从「无 font 的父级」拿到系统衬线/仿宋；用 Pane 统一下发无衬线栈 */
                    Pane {
                        id: printDockFontScope
                        anchors.fill: parent
                        z: 1
                        padding: 0
                        background: Rectangle {
                            radius: printSettingsDock.printDockCornerRadius
                            color: "transparent"
                            border.width: 1
                            border.color: Qt.rgba(1, 1, 1, 0.12)
                        }
                        font.family: root.appSansFontStack
                        font.pixelSize: printSettingsDock.printPanelFontPx
                        font.hintingPreference: Font.PreferFullHinting

                    ColumnLayout {
                        id: printDockColumn
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        anchors.topMargin: 10
                        anchors.bottomMargin: 10
                        spacing: 0

                        ColumnLayout {
                            id: printPanelMainColumn
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 0
                            spacing: 0

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 0
                                Layout.leftMargin: printSettingsDock.printPanelPaddingH
                                Layout.rightMargin: printSettingsDock.printPanelPaddingH
                                spacing: 12

                                Label {
                                    text: qsTr("打印设置")
                                    font.family: "Microsoft YaHei"
                                    font.pixelSize: 14
                                    font.bold: true
                                    color: printSettingsDock.printPanelText
                                    Layout.fillWidth: true
                                    //renderType: Text.NativeRendering
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label {
                                        text: qsTr("配置文件")
                                        font.family: "Microsoft YaHei"
                                        font.pixelSize: 14
                                        font.bold: false
                                        color: printSettingsDock.printPanelText
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    ComboBox {
                                        id: profileCombo
                                        Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: printSettingsDock.printInputControlWidth
                                        Layout.alignment: Qt.AlignVCenter
                                        font.family: "Microsoft YaHei"
                                        font.pixelSize: 12
                                        font.bold: false
                                        implicitWidth: printSettingsDock.printInputControlWidth
                                        implicitHeight: 36
                                        palette.text: printSettingsDock.printPanelText
                                        palette.buttonText: printSettingsDock.printPanelText
                                        model: [qsTr("Default Profile")]
                                        currentIndex: 0
                                        leftPadding: printSettingsDock.printPanelPaddingH
                                        rightPadding: 28
                                        topPadding: 4
                                        bottomPadding: 4
                                        background: Rectangle {
                                            radius: 4
                                            color: profileCombo.pressed ? printSettingsDock.printCtrlPressFill
                                                  : (profileCombo.hovered ? printSettingsDock.printBarHover : printSettingsDock.printInputBg)
                                            border.width: 1
                                            border.color: profileCombo.pressed ? printSettingsDock.printCtrlPressBorder : printSettingsDock.printCtrlBorder
                                        }
                                        contentItem: Label {
                                            text: profileCombo.displayText
                                            font.family: root.appSansFontStack
                                            font.pixelSize: printSettingsDock.printPanelFontPx
                                            font.hintingPreference: Font.PreferFullHinting
                                            color: printSettingsDock.printPanelText
                                            verticalAlignment: Text.AlignVCenter
                                            rightPadding: printSettingsDock.printPanelPaddingH
                                            renderType: Text.NativeRendering
                                            elide: Text.ElideRight
                                        }
                                        popup: Popup {
                                            y: profileCombo.height
                                            width: profileCombo.width
                                            padding: 1
                                            implicitHeight: Math.min(contentItem.implicitHeight, 160)
                                            contentItem: ListView {
                                                clip: true
                                                implicitHeight: contentHeight
                                                width: parent.width
                                                model: profileCombo.count
                                                currentIndex: profileCombo.highlightedIndex
                                                delegate: MenuItem {
                                                    required property int index
                                                    width: profileCombo.width
                                                    text: profileCombo.textAt(index)
                                                    font.family: root.appSansFontStack
                                                    font.pixelSize: printSettingsDock.printPanelFontPx
                                                    palette.text: printSettingsDock.printPanelText
                                                    palette.buttonText: printSettingsDock.printPanelText
                                                    highlighted: profileCombo.highlightedIndex === index
                                                    background: Rectangle {
                                                        implicitWidth: profileCombo.width
                                                        implicitHeight: 32
                                                        color: parent.highlighted ? printSettingsDock.printComboPopupHighlight : "transparent"
                                                    }
                                                    onClicked: {
                                                        profileCombo.currentIndex = index
                                                        profileCombo.popup.close()
                                                    }
                                                }
                                            }
                                            background: Rectangle {
                                                color: printSettingsDock.printComboPopupBg
                                                border.color: printSettingsDock.printComboPopupBorder
                                                border.width: 1
                                                radius: 4
                                            }
                                        }
                                    }
                                    Repeater {
                                        model: [
                                            { icon: "qrc:/huafuslicer/resources/ic_profile_save.png", tip: qsTr("保存") },
                                            { icon: "qrc:/huafuslicer/resources/ic_profile_edit.png", tip: qsTr("编辑") },
                                            { icon: "qrc:/huafuslicer/resources/ic_profile_delete.png", tip: qsTr("删除") }
                                        ]
                                        delegate: ToolButton {
                                            required property var modelData
                                            Layout.alignment: Qt.AlignVCenter
                                            implicitWidth: 30
                                            implicitHeight: 36
                                            text: ""
                                            focusPolicy: Qt.NoFocus
                                            ToolTip.visible: hovered
                                            ToolTip.text: modelData.tip
                                            ToolTip.delay: 400
                                            onClicked: { }
                                            contentItem: Image {
                                                anchors.centerIn: parent
                                                width: Math.min(18, Math.floor(parent.width * 0.55))
                                                height: Math.min(18, Math.floor(parent.height * 0.55))
                                                fillMode: Image.PreserveAspectFit
                                                source: modelData.icon
                                                asynchronous: true
                                            }
                                            background: Rectangle {
                                                radius: 4
                                                color: parent.pressed ? printSettingsDock.printCtrlPressFill
                                                      : (parent.hovered ? printSettingsDock.printBarHover : printSettingsDock.printInputBg)
                                                border.width: 1
                                                border.color: parent.pressed ? printSettingsDock.printCtrlPressBorder : printSettingsDock.printCtrlBorder
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                id: printTabBar
                                Layout.fillWidth: true
                                Layout.leftMargin: printSettingsDock.printPanelPaddingH
                                Layout.rightMargin: printSettingsDock.printPanelPaddingH
                                Layout.topMargin: 10

                                spacing: 0
                                property var tabLabels: [qsTr("基础"), qsTr("高级"), qsTr("支撑"), qsTr("材料"), qsTr("G-code")]
                                Repeater {
                                    model: printTabBar.tabLabels.length
                                    delegate: Button {
                                        required property int index
                                        readonly property bool tabSelected: printSettingsTabIndex === index
                                        Layout.fillWidth: true

                                        implicitHeight: 40
                                        leftPadding: printSettingsDock.printPanelPaddingH
                                        rightPadding: printSettingsDock.printPanelPaddingH
                                        topPadding: 6
                                        bottomPadding: 6
                                        focusPolicy: Qt.NoFocus
                                        text: printTabBar.tabLabels[index]
                                        font.family: "Microsoft YaHei"
                                        font.pixelSize: 12
                                        font.bold: false
                                        onClicked: printSettingsTabIndex = index
                                        background: Rectangle {
                                            implicitWidth: 40
                                            color: parent.pressed ? printSettingsDock.printCtrlPressFill
                                                  : (tabSelected ? printSettingsDock.printTabActiveBg
                                                     : (parent.hovered ? printSettingsDock.printBarHover : printSettingsDock.printBarCell))
                                            border.width: 1
                                            border.color: parent.pressed ? printSettingsDock.printCtrlPressBorder
                                                  : (tabSelected ? printSettingsDock.printTabAccent : printSettingsDock.printCtrlBorder)
                                            Rectangle {
                                                anchors.top: parent.top
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                height: 2
                                                visible: tabSelected
                                                color: printSettingsDock.printTabAccent
                                            }
                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                height: 2
                                                visible: tabSelected
                                                color: printSettingsDock.printTabAccent
                                            }
                                        }
                                        contentItem: Label {

                                            text: parent.text
                                            //font.family: root.appSansFontStack
                                            font.family: "Microsoft YaHei"
                                            font.pixelSize: 14
                                            font.bold: false
                                            font.hintingPreference: Font.PreferFullHinting
                                            color: printSettingsDock.printPanelText
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                            wrapMode: Text.WordWrap
                                            renderType: Text.NativeRendering
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.leftMargin: printSettingsDock.printPanelPaddingH
                                Layout.rightMargin: printSettingsDock.printPanelPaddingH
                                height: 1
                                color: Qt.rgba(1, 1, 1, 0.06)
                            }
                        }

                        ScrollView {
                            id: printSettingsScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                            Column {
                                width: printSettingsScroll.availableWidth
                                spacing: 0
                                topPadding: 8
                                bottomPadding: 10
                                leftPadding: printSettingsDock.printPanelPaddingH
                                rightPadding: printSettingsDock.printPanelPaddingH

                                Column {
                                    width: parent.width
                                    spacing: printSettingsDock.printGroupGap
                                    visible: printSettingsTabIndex === 0

                                    Column {
                                        width: parent.width
                                        spacing: printSettingsDock.printRowGap
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                source: "qrc:/huafuslicer/resources/ic_print_layer_height.png"
                                                width: 18
                                                height: 18
                                                sourceSize: Qt.size(18, 18)
                                                fillMode: Image.PreserveAspectFit
                                                asynchronous: true
                                            }
                                            Label {
                                                text: qsTr("层高")
                                                font.pixelSize: 14
                                                font.family: "Microsoft YaHei"
                                                font.bold: true
                                                color: printSettingsDock.printPanelText
                                                Layout.fillWidth: true
                                            }
                                        }
                                        Rectangle {
                                            x: printSettingsDock.printSettingRowLeadInset
                                            width: parent.width - printSettingsDock.printSettingRowLeadInset
                                            height: 1
                                            color: Qt.rgba(1, 1, 1, 0.08)
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("层高")
                                                font.family: "Microsoft YaHei"
                                                font.pixelSize: 14
                                                color: printSettingsDock.printPanelText
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 5
                                                to: 100
                                                value: 20
                                                stepSize: 5
                                                textFromValue: function (val, locale) {
                                                    return Number(val / 100).toLocaleString(locale, "f", 2)
                                                }
                                                valueFromText: function (text, locale) {
                                                    return Math.round(Number.fromLocaleString(locale, text) * 100)
                                                }
                                            }
                                            Label {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.alignment: Qt.AlignLeft
                                                text: "mm"
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                horizontalAlignment: Text.AlignLeft
                                                renderType: Text.NativeRendering
                                            }
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("首层高度")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                 font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 5
                                                to: 100
                                                value: 20
                                                stepSize: 5
                                                textFromValue: function (val, locale) {
                                                    return Number(val / 100).toLocaleString(locale, "f", 2)
                                                }
                                                valueFromText: function (text, locale) {
                                                    return Math.round(Number.fromLocaleString(locale, text) * 100)
                                                }
                                            }
                                            Label {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.alignment: Qt.AlignVCenter
                                                text: "mm"
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                horizontalAlignment: Text.AlignLeft
                                                renderType: Text.NativeRendering
                                            }
                                        }
                                    }

                                    Column {
                                        width: parent.width
                                        spacing: printSettingsDock.printRowGap
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                source: "qrc:/huafuslicer/resources/ic_print_wall.png"
                                                width: 18
                                                height: 18
                                                sourceSize: Qt.size(18, 18)
                                                fillMode: Image.PreserveAspectFit
                                                asynchronous: true
                                            }
                                            Label {
                                                text: qsTr("壁")
                                                font.family: "Microsoft YaHei"

                                                font.pixelSize: 14
                                                font.bold: true
                                                color: printSettingsDock.printPanelText
                                                Layout.fillWidth: true
                                            }
                                        }
                                        Rectangle {
                                            x: printSettingsDock.printSettingRowLeadInset
                                            width: parent.width - printSettingsDock.printSettingRowLeadInset
                                            height: 1
                                            color: Qt.rgba(1, 1, 1, 0.08)
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("壁厚")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                 font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 80
                                                to: 500
                                                value: 120
                                                stepSize: 5
                                                textFromValue: function (val, locale) {
                                                    return Number(val / 100).toLocaleString(locale, "f", 2)
                                                }
                                                valueFromText: function (text, locale) {
                                                    return Math.round(Number.fromLocaleString(locale, text) * 100)
                                                }
                                            }
                                            Label {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.alignment: Qt.AlignVCenter
                                                text: "mm"
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                horizontalAlignment: Text.AlignLeft
                                                renderType: Text.NativeRendering
                                            }
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("壁线数量")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 1
                                                to: 16
                                                value: 3
                                                stepSize: 1
                                            }
                                            Item {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                            }
                                        }
                                    }

                                    Column {
                                        width: parent.width
                                        spacing: printSettingsDock.printRowGap
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                source: "qrc:/huafuslicer/resources/ic_print_top_bottom.png"
                                                width: 18
                                                height: 18
                                                sourceSize: Qt.size(18, 18)
                                                fillMode: Image.PreserveAspectFit
                                                asynchronous: true
                                            }
                                            Label {
                                                text: qsTr("顶部/底部")
                                                font.family: "Microsoft YaHei"
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                font.bold: true
                                                color: printSettingsDock.printPanelText
                                                Layout.fillWidth: true
                                            }
                                        }
                                        Rectangle {
                                            x: printSettingsDock.printSettingRowLeadInset
                                            width: parent.width - printSettingsDock.printSettingRowLeadInset
                                            height: 1
                                            color: Qt.rgba(1, 1, 1, 0.08)
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("顶部层数")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                 font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 0
                                                to: 99
                                                value: 5
                                                stepSize: 1
                                            }
                                            Item {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                            }
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("底部层数")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                 font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 0
                                                to: 99
                                                value: 5
                                                stepSize: 1
                                            }
                                            Item {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                            }
                                        }
                                    }

                                    Column {
                                        width: parent.width
                                        spacing: printSettingsDock.printRowGap
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                source: "qrc:/huafuslicer/resources/ic_print_infill.png"
                                                width: 18
                                                height: 18
                                                sourceSize: Qt.size(18, 18)
                                                fillMode: Image.PreserveAspectFit
                                                asynchronous: true
                                            }
                                            Label {
                                                text: qsTr("填充")
                                                font.family: "Microsoft YaHei"
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                font.bold: true
                                                color: printSettingsDock.printPanelText
                                                Layout.fillWidth: true
                                            }
                                        }
                                        Rectangle {
                                            x: printSettingsDock.printSettingRowLeadInset
                                            width: parent.width - printSettingsDock.printSettingRowLeadInset
                                            height: 1
                                            color: Qt.rgba(1, 1, 1, 0.08)
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("填充密度")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                 font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 0
                                                to: 100
                                                value: 15
                                                stepSize: 1
                                            }
                                            Label {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.alignment: Qt.AlignVCenter
                                                text: "%"
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                horizontalAlignment: Text.AlignLeft
                                                renderType: Text.NativeRendering
                                            }
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("填充图案")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                 font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            ComboBox {
                                                id: infillPatternCombo
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                Layout.alignment: Qt.AlignVCenter
                                                implicitWidth: printSettingsDock.printInputControlWidth
                                                implicitHeight: 36
                                                leftPadding: printSettingsDock.printPanelPaddingH
                                                rightPadding: 28
                                                topPadding: 4
                                                bottomPadding: 4
                                                font.family: root.appSansFontStack
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                palette.text: printSettingsDock.printPanelText
                                                palette.buttonText: printSettingsDock.printPanelText
                                                model: [qsTr("Grid"), qsTr("Lines"), qsTr("Triangles")]
                                                currentIndex: 0
                                                background: Rectangle {
                                                    radius: 4
                                                    color: infillPatternCombo.pressed ? printSettingsDock.printCtrlPressFill
                                                          : (infillPatternCombo.hovered ? printSettingsDock.printBarHover : printSettingsDock.printInputBg)
                                                    border.width: 1
                                                    border.color: infillPatternCombo.pressed ? printSettingsDock.printCtrlPressBorder : printSettingsDock.printCtrlBorder
                                                }
                                                contentItem: Label {
                                                    text: infillPatternCombo.displayText
                                                    font.family: root.appSansFontStack
                                                    font.pixelSize: printSettingsDock.printPanelFontPx
                                                    font.hintingPreference: Font.PreferFullHinting
                                                    color: printSettingsDock.printPanelText
                                                    verticalAlignment: Text.AlignVCenter
                                                    rightPadding: printSettingsDock.printPanelPaddingH
                                                    renderType: Text.NativeRendering
                                                }
                                                popup: Popup {
                                                    y: infillPatternCombo.height
                                                    width: infillPatternCombo.width
                                                    padding: 1
                                                    implicitHeight: Math.min(contentItem.implicitHeight, 160)
                                                    contentItem: ListView {
                                                        clip: true
                                                        implicitHeight: contentHeight
                                                        width: parent.width
                                                        model: infillPatternCombo.count
                                                        currentIndex: infillPatternCombo.highlightedIndex
                                                        delegate: MenuItem {
                                                            required property int index
                                                            width: infillPatternCombo.width
                                                            text: infillPatternCombo.textAt(index)
                                                            font.family: root.appSansFontStack
                                                            font.pixelSize: printSettingsDock.printPanelFontPx
                                                            palette.text: printSettingsDock.printPanelText
                                                            palette.buttonText: printSettingsDock.printPanelText
                                                            highlighted: infillPatternCombo.highlightedIndex === index
                                                            background: Rectangle {
                                                                implicitWidth: infillPatternCombo.width
                                                                implicitHeight: 32
                                                                color: parent.highlighted ? printSettingsDock.printComboPopupHighlight : "transparent"
                                                            }
                                                            onClicked: {
                                                                infillPatternCombo.currentIndex = index
                                                                infillPatternCombo.popup.close()
                                                            }
                                                        }
                                                    }
                                                    background: Rectangle {
                                                        color: printSettingsDock.printComboPopupBg
                                                        border.color: printSettingsDock.printComboPopupBorder
                                                        border.width: 1
                                                        radius: 4
                                                    }
                                                }
                                            }
                                            Item {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                            }
                                        }
                                    }

                                    Column {
                                        width: parent.width
                                        spacing: printSettingsDock.printRowGap
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                source: "qrc:/huafuslicer/resources/ic_print_other.png"
                                                width: 18
                                                height: 18
                                                sourceSize: Qt.size(18, 18)
                                                fillMode: Image.PreserveAspectFit
                                                asynchronous: true
                                            }
                                            Label {
                                                text: qsTr("其他")
                                                font.family: "Microsoft YaHei"
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                font.bold: true
                                                color: printSettingsDock.printPanelText
                                                Layout.fillWidth: true
                                            }
                                        }
                                        Rectangle {
                                            x: printSettingsDock.printSettingRowLeadInset
                                            width: parent.width - printSettingsDock.printSettingRowLeadInset
                                            height: 1
                                            color: Qt.rgba(1, 1, 1, 0.08)
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("打印温度")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                 font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 0
                                                to: 350
                                                value: 200
                                                stepSize: 1
                                            }
                                            Label {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.alignment: Qt.AlignVCenter
                                                text: "°C"
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                horizontalAlignment: Text.AlignLeft
                                                renderType: Text.NativeRendering
                                            }
                                        }
                                        RowLayout {
                                            width: parent.width
                                            spacing: 0
                                            PrintSettingRowLead {}
                                            Label {
                                                text: qsTr("热床温度")
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                Layout.preferredWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.maximumWidth: printSettingsDock.printSettingParamColumnWidth
                                                Layout.minimumWidth: 0
                                                font.family: "Microsoft YaHei"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                renderType: Text.NativeRendering
                                            }
                                            PrintNumSpin {
                                                Layout.preferredWidth: printSettingsDock.printInputControlWidth
                                                Layout.maximumWidth: printSettingsDock.printInputControlWidth
                                                Layout.fillWidth: false
                                                from: 0
                                                to: 150
                                                value: 60
                                                stepSize: 1
                                            }
                                            Label {
                                                Layout.preferredWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.minimumWidth: printSettingsDock.printInputSuffixColumnWidth
                                                Layout.alignment: Qt.AlignVCenter
                                                text: "°C"
                                                color: printSettingsDock.printPanelText
                                                font.pixelSize: printSettingsDock.printPanelFontPx
                                                horizontalAlignment: Text.AlignLeft
                                                renderType: Text.NativeRendering
                                            }
                                        }
                                    }
                                }

                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: parent.width - printSettingsDock.printPanelPaddingH * 2
                                    visible: printSettingsTabIndex !== 0
                                    wrapMode: Text.WordWrap
                                    horizontalAlignment: Text.AlignHCenter
                                    text: printTabBar.tabLabels[printSettingsTabIndex] + qsTr("（占位，后续可接切片引擎参数）")
                                    color: printSettingsDock.printPanelText
                                    font.pixelSize: printSettingsDock.printPanelFontPx
                                    topPadding: 24
                                }
                            }
                        }

                        Button {
                            id: btnSliceSettingsFooter
                            Layout.fillWidth: true
                            Layout.leftMargin: printSettingsDock.printPanelPaddingH
                            Layout.rightMargin: printSettingsDock.printPanelPaddingH
                            Layout.bottomMargin: 10
                            Layout.topMargin: 6
                            implicitHeight: 48
                            text: qsTr("切片设置")
                            font.family: root.appSansFontStack
                            focusPolicy: Qt.NoFocus
                            onClicked: { }
                            background: Rectangle {
                                radius: 8
                                color: btnSliceSettingsFooter.pressed ? printSettingsDock.printCtrlPressFill
                                      : (btnSliceSettingsFooter.hovered ? printSettingsDock.printBarHover : printSettingsDock.printBarCell)
                                border.width: 1
                                border.color: btnSliceSettingsFooter.pressed ? printSettingsDock.printCtrlPressBorder : printSettingsDock.printCtrlBorder
                            }
                            contentItem: Label {
                                text: btnSliceSettingsFooter.text
                                color: printSettingsDock.printPanelText
                                font.family: root.appSansFontStack
                                font.pixelSize: printSettingsDock.printPanelFontPx
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                    }
                }
            }

            // 右上角：显示/隐藏设置（竖排单字 + 绿色渐变竖条，同「显示代码」风格）
            Button {
                id: btnPrintSettingsCorner
                visible: root.workspaceMode === 0
                z: 46
                anchors.top: mainContent.top
                anchors.right: printSettingsDock.left
                anchors.topMargin: root.printDockOuterMarginV
                anchors.rightMargin: 10
                focusPolicy: Qt.NoFocus
                implicitWidth: 36
                implicitHeight: 92
                leftPadding: 5
                rightPadding: 5
                topPadding: 12
                bottomPadding: 12
                onClicked: printSettingsPanelOpen = !printSettingsPanelOpen
                background: Rectangle {
                    radius: 10
                    border.width: 1
                    border.color: btnPrintSettingsCorner.pressed ? Qt.darker(root.cornerTabGreenBorder, 1.15)
                                                                 : root.cornerTabGreenBorder
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop {
                            position: 0.0
                            color: btnPrintSettingsCorner.pressed ? Qt.darker(root.cornerTabGreenTop, 1.18)
                                  : (btnPrintSettingsCorner.hovered ? Qt.lighter(root.cornerTabGreenTop, 1.05)
                                                                    : root.cornerTabGreenTop)
                        }
                        GradientStop {
                            position: 1.0
                            color: btnPrintSettingsCorner.pressed ? Qt.darker(root.cornerTabGreenBottom, 1.12)
                                                                  : root.cornerTabGreenBottom
                        }
                    }
                }
                contentItem: Item {
                    anchors.fill: parent

                    Column {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        spacing: 0

                        Repeater {
                            model: root.charsForVerticalLabel(printSettingsPanelOpen ? qsTr("隐藏设置") : qsTr("显示设置"))
                            delegate: Label {
                                required property var modelData
                                width: parent.width
                                height: Math.ceil(font.pixelSize * 1.12)
                                text: modelData
                                color: "#ffffff"
                                font.family: root.appSansFontStack
                                font.pixelSize: 13
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                renderType: Text.NativeRendering
                            }
                        }
                    }
                }
            }

            // 轨迹预览：显示/隐藏代码侧栏（竖排「显示代码」/「隐藏代码」）
            Button {
                id: btnGcodePanelCorner
                visible: root.workspaceMode === 1
                z: 46
                anchors.top: mainContent.top
                anchors.right: mainContent.right
                anchors.topMargin: root.printDockOuterMarginV
                anchors.rightMargin: previewSim.gcodeSidePanelOpen ? (previewSim.rightPanelWidth + 24) : 10
                focusPolicy: Qt.NoFocus
                implicitWidth: 36
                implicitHeight: 92
                leftPadding: 5
                rightPadding: 5
                topPadding: 12
                bottomPadding: 12
                onClicked: previewSim.gcodeSidePanelOpen = !previewSim.gcodeSidePanelOpen
                background: Rectangle {
                    radius: 10
                    border.width: 1
                    border.color: btnGcodePanelCorner.pressed ? Qt.darker(root.cornerTabGreenBorder, 1.15)
                                                              : root.cornerTabGreenBorder
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop {
                            position: 0.0
                            color: btnGcodePanelCorner.pressed ? Qt.darker(root.cornerTabGreenTop, 1.18)
                                  : (btnGcodePanelCorner.hovered ? Qt.lighter(root.cornerTabGreenTop, 1.05)
                                                                 : root.cornerTabGreenTop)
                        }
                        GradientStop {
                            position: 1.0
                            color: btnGcodePanelCorner.pressed ? Qt.darker(root.cornerTabGreenBottom, 1.12)
                                                               : root.cornerTabGreenBottom
                        }
                    }
                }
                contentItem: Item {
                    anchors.fill: parent

                    Column {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        spacing: 0

                        Repeater {
                            model: root.charsForVerticalLabel(previewSim.gcodeSidePanelOpen ? qsTr("隐藏代码") : qsTr("显示代码"))
                            delegate: Label {
                                required property var modelData
                                width: parent.width
                                height: Math.ceil(font.pixelSize * 1.12)
                                text: modelData
                                color: "#ffffff"
                                font.family: root.appSansFontStack
                                font.pixelSize: 13
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                renderType: Text.NativeRendering
                            }
                        }
                    }
                }
            }

            // ----- 模型列表（右侧第 3 个按钮打开，毛玻璃浮层）-----
            Item {
                id: modelListFlyout
                visible: modelListPanelOpen && root.workspaceMode === 0
                z: 55
                anchors.left: toolSideBar.right
                anchors.leftMargin: 10
                anchors.top: toolSideBar.top
                width: 300
                height: Math.min(540, mainContent.height - 160)
                opacity: (modelListPanelOpen && root.workspaceMode === 0) ? 1 : 0
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
                    anchors.margins: 10
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: qsTr("模型列表")
                            font.family: root.modelListFontFamily
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
                            font.family: root.modelListFontFamily
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
                            Layout.preferredWidth: 40
                            text: qsTr("显示")
                            font.family: root.modelListFontFamily
                            font.pixelSize: 11
                            color: textMuted
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("名称")
                            font.family: root.modelListFontFamily
                            font.pixelSize: 11
                            color: textMuted
                        }
                        Label {
                            Layout.preferredWidth: 64
                            text: qsTr("顶点/面")
                            font.family: root.modelListFontFamily
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
                            required property bool active
                            required property bool sceneVisible

                            width: modelListView.width
                            height: 40
                            radius: 8
                            color: !sceneVisible
                                   ? Qt.rgba(0.12, 0.12, 0.14, 0.55)
                                   : (meshRowHover.hovered
                                      ? (active ? Qt.rgba(0.14, 0.38, 0.24, 0.22)
                                               : Qt.rgba(1, 1, 1, 0.10))
                                      : (active ? Qt.rgba(0.11, 0.32, 0.20, 0.14) : Qt.rgba(1, 1, 1, 0.03)))
                            border.width: sceneVisible && active ? 1 : (!sceneVisible ? 1 : 0)
                            border.color: !sceneVisible ? Qt.rgba(1, 1, 1, 0.06)
                                         : (active ? Qt.rgba(0.24, 0.92, 0.52, 0.42) : "transparent")

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
                                    font.family: root.modelListFontFamily
                                    font.pixelSize: 12
                                    wrapMode: Text.WrapAnywhere
                                    width: Math.min(360, Math.max(180, meshRowDelegate.width + 48))
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 8
                                spacing: 6

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
                                        z: 2
                                        onClicked: scene3d.setModelSceneVisible(meshIndex, !sceneVisible)
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.meshNameDisplay18(name)
                                    color: !sceneVisible ? textMuted : (active ? accentGreen : textPrimary)
                                    font.family: root.modelListFontFamily
                                    font.bold: active
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                }

                                Label {
                                    Layout.preferredWidth: 64
                                    text: vertices + " / " + faces
                                    color: textMuted
                                    font.family: root.modelListFontFamily
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                    elide: Text.ElideRight
                                }
                            }

                            // 整行交互：左键在「显示」列外切换激活；左键在显示列交给下层勾选；右键菜单
                            MouseArea {
                                id: rowInteractArea
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: function (mouse) {
                                    if (mouse.button === Qt.RightButton) {
                                        workspaceResponses.openListMeshContextFromRow(meshRowDelegate.meshIndex,
                                                                                        mouse.globalX,
                                                                                        mouse.globalY)
                                    }
                                }
                                onPressed: function (mouse) {
                                    if (mouse.button === Qt.RightButton) {
                                        mouse.accepted = true
                                        return
                                    }
                                    if (mouse.button === Qt.LeftButton && mouse.x < 52) {
                                        mouse.accepted = false
                                        return
                                    }
                                    if (mouse.button !== Qt.LeftButton)
                                        return
                                    const ctrl = (mouse.modifiers & Qt.ControlModifier) !== 0
                                    if (ctrl) {
                                        scene3d.setModelActive(meshIndex, true)
                                        mouse.accepted = true
                                        return
                                    }
                                    const m = scene3d.meshModels
                                    let activeCount = 0
                                    for (let i = 0; i < m.length; ++i) {
                                        if (m[i].active)
                                            activeCount++
                                    }
                                    if (active && activeCount === 1)
                                        scene3d.setModelActive(-1, false)
                                    else
                                        scene3d.setModelActive(meshIndex, false)
                                    mouse.accepted = true
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("单击行激活；仅一项激活时再次单击可取消。Ctrl+单击多选切换。Delete 删除已激活模型")
                            font.family: root.modelListFontFamily
                            font.pixelSize: 10
                            color: textMuted
                            wrapMode: Text.WordWrap
                        }

                        Button {
                            id: btnDeleteAllMeshes
                            Layout.alignment: Qt.AlignRight
                            text: qsTr("全部删除")
                            focusPolicy: Qt.NoFocus
                            font.family: root.modelListFontFamily
                            enabled: scene3d.meshModels.length > 0
                            implicitHeight: 30
                            padding: 8
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
                        font.family: root.modelListFontFamily
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            // ----- 竖工具条（左侧透明悬浮，叠在 3D 视口上）-----
            Rectangle {
                id: toolSideBar
                visible: root.workspaceMode === 0
                z: 50
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 8
                anchors.bottomMargin: root.viewCubeBottomReserve
                width: 54
                radius: panelRadius()
                color: Qt.rgba(0.09, 0.10, 0.13, 0.48)
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.1)

                // 轻阴影（悬浮感）
                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: -3
                    anchors.rightMargin: -3
                    anchors.topMargin: -3
                    anchors.bottomMargin: -3
                    radius: parent.radius + 3
                    color: Qt.rgba(0, 0, 0, 0.35)
                    z: -1
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 8

                    Repeater {
                        model: [
                            { tip: qsTr("模型列表") },
                            { tip: qsTr("导入模型") },
                            { tip: qsTr("导入 G-code") },
                            { tip: qsTr("重置视图") },
                            { tip: qsTr("自动摆放") },
                            { tip: qsTr("模型旋转") },
                            { tip: qsTr("缩放") },
                            { t: "📏", tip: qsTr("测量") },
                            { tip: qsTr("保存") },
                            { t: "⇄", tip: qsTr("镜像") },
                            { t: "△", tip: qsTr("支撑") },
                            { t: "⋯", tip: qsTr("更多") }
                        ]

                        delegate: Button {
                            id: toolSideBarBtn
                            required property int index
                            required property var modelData
                            readonly property bool showIcon: index === 0 || index === 1 || index === 2
                                                            || index === 3 || index === 4 || index === 5
                                                            || index === 6 || index === 8
                            readonly property url iconSource: index === 0 ? "qrc:/huafuslicer/resources/import_model.png"
                                                                 : (index === 1 ? "image://huafuslicer/import_model"
                                                                                : (index === 2 ? "qrc:/huafuslicer/resources/gcode.png"
                                                                                               : (index === 3 ? "qrc:/huafuslicer/resources/rotate_model.png"
                                                                                                              : (index === 4 ? "qrc:/huafuslicer/resources/arrange_model.png"
                                                                                                                             : (index === 5 ? "qrc:/huafuslicer/resources/rotate_model.png"
                                                                                                                                            : (index === 6 ? "qrc:/huafuslicer/resources/arrange_model.png"
                                                                                                                                                            : (index === 8 ? "qrc:/huafuslicer/resources/ic_profile_save.png" : "")))))))
                            readonly property bool activeTool: (index === 5 && root.sideToolMode === 1)
                                                               || (index === 6 && root.sideToolMode === 4)
                                                               || (index === 7 && root.sideToolMode === 2)
                                                               || (index === 9 && root.sideToolMode === 3)
                            Layout.fillWidth: true
                            implicitHeight: toolSideBar.width - 8
                            focusPolicy: Qt.NoFocus
                            padding: 0
                            text: modelData.t !== undefined ? modelData.t : ""
                            ToolTip.visible: hovered
                            ToolTip.text: modelData.tip
                            ToolTip.delay: 400

                            // 半透明底，与侧栏一起叠在 3D 上
                            background: Rectangle {
                                radius: 0
                                color: toolSideBarBtn.activeTool ? Qt.rgba(0.22, 0.38, 0.62, 0.78)
                                      : (parent.hovered ? Qt.rgba(0.18, 0.20, 0.26, 0.72)
                                      : Qt.rgba(0.08, 0.09, 0.11, 0.4)
                                      )
                                border.width: 0
                            }
                            contentItem: Item {
                                id: barBtnContent
                                anchors.fill: parent
                                // 侧栏图标统一尺寸，避免不同来源图标视觉大小不一致
                                readonly property int iconSide: 24
                                // 模型旋转图标固定 24px（避免后续样式改动影响该工具）
                                readonly property int displayIconSide: toolSideBarBtn.index === 5 ? 24 : iconSide

                                Image {
                                    visible: toolSideBarBtn.showIcon
                                    anchors.centerIn: parent
                                    width: barBtnContent.displayIconSide
                                    height: width
                                    sourceSize: toolSideBarBtn.index === 5 ? Qt.size(24, 24) : Qt.size(64, 64)
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    mipmap: true
                                    source: toolSideBarBtn.iconSource
                                    asynchronous: false
                                }
                                Label {
                                    anchors.fill: parent
                                    visible: !toolSideBarBtn.showIcon
                                    text: parent.parent.text
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    color: textPrimary
                                    font.pixelSize: barBtnContent.iconSide
                                }
                            }
                            onClicked: {
                                if (index === 0)
                                    modelListPanelOpen = !modelListPanelOpen
                                if (index === 1)
                                    importModelDialog.open()
                                if (index === 2)
                                    importGcodeDialog.open()
                                // 重置视图：回到默认打开时的视角
                                if (index === 3)
                                    scene3d.resetView()
                                if (index === 4)
                                    scene3d.autoArrangeModels()
                                if (index === 8)
                                    saveAllModelsFolderDialog.open()
                                if (index === 5)
                                    root.sideToolMode = (root.sideToolMode === 1 ? 0 : 1)
                                if (index === 6)
                                    root.sideToolMode = (root.sideToolMode === 4 ? 0 : 4)
                                if (index === 7)
                                    root.sideToolMode = (root.sideToolMode === 2 ? 0 : 2)
                                if (index === 9)
                                    root.sideToolMode = (root.sideToolMode === 3 ? 0 : 3)

                                if (index !== 5 && index !== 6 && index !== 7 && index !== 9)
                                    root.sideToolMode = 0
                                scene3d.interactionTool = root.sideToolMode
                            }
                        }
                    }

                    Item {
                        Layout.fillHeight: true
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.sideToolMode === 2 || root.sideToolMode === 3 || root.sideToolMode === 4
                                 || scene3d.measurementTraceCount > 0
                        text: root.sideToolMode === 3
                              ? qsTr("镜像：点击模型，沿其本地 X 轴翻转（再点一次还原）")
                              : (root.sideToolMode === 4
                                 ? qsTr("缩放：滚轮放大/缩小（支持多选已激活模型）")
                                 : (scene3d.measurementTraceCount > 0
                                    ? (scene3d.measurementTraceCount === 1
                                       ? qsTr("距离 %1 mm").arg(scene3d.measuredDistanceMm.toFixed(2))
                                       : qsTr("最近 %1 mm · 共 %2 条").arg(scene3d.measuredDistanceMm.toFixed(2)).arg(scene3d.measurementTraceCount))
                                    : qsTr("测量：请依次点击两点")))
                        color: "#d8dee9"
                        font.pixelSize: 10
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        padding: 4
                    }
                }
            }

            // 平面上已有测量线段时：在测量工具按钮右侧浮现清除按钮
            Button {
                id: clearMeasurementBtn
                visible: scene3d.measurementTraceCount > 0 && root.workspaceMode === 0
                z: 55
                height: toolSideBar.width - 8
                width: height
                focusPolicy: Qt.NoFocus
                padding: 0
                text: qsTr("清除")
                font.pixelSize: 10
                ToolTip.visible: hovered
                ToolTip.text: qsTr("清除测量痕迹")
                ToolTip.delay: 400
                anchors.left: toolSideBar.right
                anchors.leftMargin: 6
                anchors.top: toolSideBar.top
                anchors.topMargin: 4 + 7 * toolSideBar.width + (toolSideBar.width - 8) * 0.5 - height * 0.5
                onClicked: scene3d.clearMeasurementTraces()
                background: Rectangle {
                    radius: 6
                    color: clearMeasurementBtn.hovered ? Qt.rgba(0.35, 0.22, 0.22, 0.92)
                          : Qt.rgba(0.12, 0.13, 0.16, 0.88)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.12)
                }
                contentItem: Label {
                    text: clearMeasurementBtn.text
                    font: clearMeasurementBtn.font
                    color: "#e8c4c4"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Item {
            width: 0
            height: 0
            visible: false
            Shortcut {
                sequence: "Ctrl+Z"
                context: Qt.ApplicationShortcut
                enabled: root.viewportGlReady ? workspaceHub.undoAvailable : false
                onActivated: workspaceHub.undo()
            }
            Shortcut {
                sequence: "Ctrl+Shift+Z"
                context: Qt.ApplicationShortcut
                enabled: root.viewportGlReady ? workspaceHub.redoAvailable : false
                onActivated: workspaceHub.redo()
            }
        }
    }

    /**
     * 工作区响应中心：集中处理 HuafuWorkspaceMessageHub 信号、导入队列、网格保存/导出与右键菜单逻辑。
     * 侧栏按钮等请通过 workspaceResponses 调用对应方法，便于维护与检索。
     */
    WorkspaceResponseHub {
        id: workspaceResponses
        appRoot: root
        workspaceHub: workspaceHub
        guiWorkspaceHub: typeof slicerWorkspace !== "undefined" ? slicerWorkspace : null
        windowHelper: windowHelper
        meshExportSuccessToast: meshExportSuccessToast
        meshExportFailDialog: meshExportFailDialog
        importModelErrorDialog: importModelErrorDialog
        meshContextSheet: meshContextSheet
    }
}
