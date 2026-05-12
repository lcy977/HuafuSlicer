import QtQuick
import QtQuick.Controls

/**
 * 工作区 UI 响应中心（公共入口）
 *
 * 职责：
 * - 统一连接 HuafuWorkspaceMessageHub 转发的视口信号，并驱动弹窗 / Toast / 列表状态；
 * - 管理多文件导入队列、批量导出与单模型保存/另存为等业务响应；
 * - 通过 workspaceHub.viewport 调用 OpenGLViewport（与 Main 中 Timer 赋值 scene3d 一致），
 *   通过 windowHelper 调用原生保存对话框与调试日志。
 *
 * Main.qml 中的按钮、菜单、快捷键应优先调用本对象的接口，避免在界面各处散落重复逻辑。
 */
Item {
    id: workspaceResponses
    width: 0
    height: 0
    visible: false

    // ----- 依赖注入：由 Main 在实例化时绑定 -----

    /** 主 ApplicationWindow：用于提窗、坐标映射、根级状态（如 modelListPanelOpen） */
    required property var appRoot

    /** 视口消息中转（已连接 OpenGLViewport 的信号） */
    required property var workspaceHub

    /** 原生对话框、路径与日志等桌面集成 */
    required property var windowHelper

    /** 保存成功轻提示 */
    property var meshExportSuccessToast: null

    /** 保存失败对话框 */
    property var meshExportFailDialog: null

    /** 模型导入失败对话框 */
    property var importModelErrorDialog: null

    /** 视口/列表右键浮层菜单 */
    property var meshContextSheet: null

    /** 当前 OpenGL 视口（与 scene3d 同源：来自 hub.viewport） */
    readonly property var scene: workspaceHub ? workspaceHub.viewport : null

    /** 网格异步写盘时主线程遮罩（由 Main 根对象持有，便于与布局层绑定） */
    function setMeshFileExportBusy(v) {
        if (appRoot)
            appRoot.meshFileExportBusy = v
    }

    function meshExportClearBusy() {
        setMeshFileExportBusy(false)
    }

    /** 侧栏「保存全部」：选中文件夹后触发 C++ 异步导出 */
    function runBulkMeshSaveExport(folderUrl) {
        setMeshFileExportBusy(true)
        if (scene)
            scene.exportAllMeshesToFolderAsync(folderUrl, 0)
    }

    // ----- Hub 信号 → UI 反馈 -----

    Connections {
        target: workspaceHub

        /** 单模型导出完成：成功 Toast，失败弹窗 */
        function onMeshSingleExportFinished(ok, meshIndex, message) {
            workspaceResponses.meshExportClearBusy()
            if (ok) {
                if (meshExportSuccessToast)
                    meshExportSuccessToast.open()
            } else if (message && message.length > 0) {
                if (meshExportFailDialog) {
                    meshExportFailDialog.text = message
                    meshExportFailDialog.open()
                }
            }
        }

        /** 批量导出完成 */
        function onMeshBulkExportFinished(ok, message) {
            workspaceResponses.meshExportClearBusy()
            if (ok) {
                if (meshExportSuccessToast)
                    meshExportSuccessToast.open()
            } else {
                const fallback = qsTr("未能将全部可见模型导出到所选文件夹，请检查路径、磁盘空间与写入权限后重试。")
                if (meshExportFailDialog) {
                    meshExportFailDialog.text = (message && message.length > 0) ? message : fallback
                    meshExportFailDialog.open()
                }
            }
        }

        /** 导入完成：失败弹窗；成功展开模型列表；并推进导入队列 */
        function onModelImportFinished(ok, message) {
            if (!ok) {
                if (importModelErrorDialog) {
                    importModelErrorDialog.text = message
                    importModelErrorDialog.open()
                }
            } else {
                if (appRoot)
                    appRoot.modelListPanelOpen = true
            }
            if (importQueue && importQueueIndex < importQueue.length) {
                importQueueIndex += 1
                importNext()
            }
        }

        /** 视口内请求上下文菜单：映射到窗口坐标后弹出 sheet */
        function onContextMenuRequested(modelIndex, lx, ly) {
            if (modelIndex >= 0)
                workspaceResponses.openSceneMeshContextFromView(modelIndex, lx, ly)
        }
    }

    // ----- 多文件导入队列（串行，避免 C++ 端并发导入冲突） -----

    property var importQueue: []
    property int importQueueIndex: 0

    /** 文件对话框多选确定后由 Main 调用 */
    function enqueueModelImports(urlList) {
        importQueue = urlList ? urlList.slice(0) : []
        importQueueIndex = 0
        importNext()
    }

    function importNext() {
        if (!importQueue || importQueueIndex >= importQueue.length)
            return
        if (!scene || scene.importInProgress)
            return
        scene.importModel(importQueue[importQueueIndex])
    }

    // ----- 右键菜单定位 -----

    function openMeshContextMenuAtWindow(meshIndex, winX, winY) {
        if (meshIndex < 0 || !meshContextSheet)
            return
        meshContextSheet.openAtWindow(winX, winY, meshIndex)
    }

    /** 视口局部坐标 → 全局 → 主内容区局部坐标 */
    function openSceneMeshContextFromView(modelIndex, sceneLocalX, sceneLocalY) {
        if (modelIndex < 0 || !scene || !appRoot)
            return
        const g = scene.mapToGlobal(Qt.point(sceneLocalX, sceneLocalY))
        const lp = appRoot.contentItem.mapFromGlobal(g.x, g.y)
        openMeshContextMenuAtWindow(modelIndex, lp.x, lp.y)
    }

    /** 模型列表行右键（已是全局坐标） */
    function openListMeshContextFromRow(meshIndex, globalX, globalY) {
        if (meshIndex < 0 || !appRoot)
            return
        const lp = appRoot.contentItem.mapFromGlobal(globalX, globalY)
        openMeshContextMenuAtWindow(meshIndex, lp.x, lp.y)
    }

    // ----- 自测入口（main.cpp 通过 QMetaObject::invokeMethod 调用根窗口方法再转调此处） -----

    function selfTestOpenNativeSaveDialog() {
        meshSaveDbg("SELFTEST selfTestOpenNativeSaveDialog()")
        const m = scene ? scene.meshModels : null
        if (!m || m.length < 1) {
            meshSaveDbg("SELFTEST abort: no mesh (import one model first)")
            return
        }
        openMeshSaveAsSystemDialog(0)
    }

    // ----- 导出路径与文件名辅助 -----

    function fileCompleteBaseFromPath(path) {
        const s = String(path || "").replace(/\\/g, "/")
        const slash = s.lastIndexOf("/")
        const file = slash >= 0 ? s.substring(slash + 1) : s
        const dot = file.lastIndexOf(".")
        return dot > 0 ? file.substring(0, dot) : file
    }

    /** 与 C++ sanitizedExportBaseName 一致：去掉非法文件名字符 */
    function sanitizeMeshFileBaseName(s) {
        let t = String(s || "").replace(/[<>:"/\\|?*\u0000-\u001f]/g, "_").trim()
        return t.length > 0 ? t : "mesh"
    }

    function joinDirAndFile(dir, fileName) {
        const d = String(dir || "")
        const f = String(fileName || "")
        if (!d)
            return f
        if (d.endsWith("/") || d.endsWith("\\"))
            return d + f
        return d + (d.indexOf("\\") >= 0 ? "\\" : "/") + f
    }

    function parentDirOfFile(localPath) {
        const s = String(localPath || "")
        const li = Math.max(s.lastIndexOf("/"), s.lastIndexOf("\\"))
        if (li <= 0)
            return ""
        return s.substring(0, li)
    }

    function meshSuggestExportBaseName(meshIndex) {
        const m = scene ? scene.meshModels : null
        let hint = "mesh"
        if (m && meshIndex >= 0 && meshIndex < m.length) {
            const row = m[meshIndex]
            const fp = String(row.filePath || "")
            const nm = String(row.name || "")
            if (fp.length > 0)
                hint = fileCompleteBaseFromPath(fp)
            else
                hint = fileCompleteBaseFromPath(nm) || nm
            if (!hint || hint.length === 0)
                hint = "mesh"
        }
        return sanitizeMeshFileBaseName(hint)
    }

    /** 根据系统「保存类型」或扩展名得到导出格式（0/1/2） */
    function meshSaveFormatFromNameFilter(filterText, fileUrl) {
        const f = String(filterText || "").toUpperCase()
        const path = String(fileUrl || "").toUpperCase()
        if (f.indexOf("OBJ") >= 0 || path.indexOf(".OBJ") >= 0)
            return 2
        if (f.indexOf("文本") >= 0 || f.indexOf("ASCII") >= 0 || f.indexOf("TEXT") >= 0)
            return 1
        if (f.indexOf("二进制") >= 0 || f.indexOf("BINARY") >= 0)
            return 0
        if (path.indexOf(".OBJ") >= 0)
            return 2
        if (path.indexOf(".STL") >= 0)
            return 0
        return 0
    }

    function meshSaveDbg(msg) {
        if (windowHelper)
            windowHelper.writeDebugLog(String(msg))
    }

    /**
     * 从右键菜单打开文件对话框：须在菜单项 onClicked 之后延迟再 open()。
     * 不能用 Menu.onClosed：在 Qt 中 closed 往往早于 MenuItem.onClicked，pending 尚未设置，导致永远不弹窗。
     */
    Timer {
        id: meshFileDialogFromMenuDeferTimer
        interval: 220
        repeat: false
        /** 0 未使用 1 另存为 2 首次导出（无磁盘路径） */
        property int pendingOp: 0
        property int pendingMeshIndex: -1
        onRunningChanged: {
            if (running)
                workspaceResponses.meshSaveDbg("deferTimer START interval=" + interval + "ms op=" + pendingOp + " mesh=" + pendingMeshIndex)
        }
        onTriggered: {
            const op = pendingOp
            const idx = pendingMeshIndex
            workspaceResponses.meshSaveDbg("deferTimer FIRED op=" + op + " meshIndex=" + idx)
            pendingOp = 0
            pendingMeshIndex = -1
            if (idx < 0) {
                workspaceResponses.meshSaveDbg("deferTimer ABORT meshIndex<0")
                return
            }
            workspaceResponses.meshSaveDbg("deferTimer raise+activate then branch op=" + op)
            if (appRoot) {
                appRoot.raise()
                appRoot.requestActivate()
            }
            if (op === 1) {
                workspaceResponses.meshSaveDbg("deferTimer -> openMeshSaveAsSystemDialog(" + idx + ")")
                workspaceResponses.openMeshSaveAsSystemDialog(idx)
            } else if (op === 2) {
                workspaceResponses.meshSaveDbg("deferTimer -> openMeshExportNative(" + idx + ")")
                workspaceResponses.openMeshExportNative(idx)
            } else {
                workspaceResponses.meshSaveDbg("deferTimer UNKNOWN op=" + op + " (expected 1 or 2)")
            }
        }
    }

    function scheduleMeshFileDialogFromMenu(op, meshIndex) {
        meshSaveDbg("scheduleMeshFileDialogFromMenu op=" + op + " meshIndex=" + meshIndex)
        if (meshIndex < 0) {
            meshSaveDbg("schedule ABORT meshIndex<0")
            return
        }
        meshFileDialogFromMenuDeferTimer.pendingOp = op
        meshFileDialogFromMenuDeferTimer.pendingMeshIndex = meshIndex
        meshSaveDbg("schedule restart deferTimer running=" + meshFileDialogFromMenuDeferTimer.running)
        meshFileDialogFromMenuDeferTimer.restart()
        meshSaveDbg("schedule after restart running=" + meshFileDialogFromMenuDeferTimer.running)
    }

    /** 保存：有原文件则覆盖；无原路径则延迟打开导出对话框 */
    function openMeshPrimarySave(meshIndex) {
        meshSaveDbg("openMeshPrimarySave meshIndex=" + meshIndex + " hasStorage=" + (meshIndex >= 0 && scene ? scene.meshHasPersistentStorage(meshIndex) : false))
        if (meshIndex < 0 || !scene)
            return
        if (!scene.meshHasPersistentStorage(meshIndex)) {
            meshSaveDbg("openMeshPrimarySave -> schedule first-export op=2")
            scheduleMeshFileDialogFromMenu(2, meshIndex)
            return
        }
        meshSaveDbg("openMeshPrimarySave -> saveMeshInPlaceAsync")
        setMeshFileExportBusy(true)
        scene.saveMeshInPlaceAsync(meshIndex)
    }

    /** 首次保存 / 无原路径：系统「导出模型」对话框 */
    function openMeshExportNative(meshIndex) {
        meshSaveDbg("openMeshExportNative enter meshIndex=" + meshIndex)
        if (meshIndex < 0 || !scene || !windowHelper)
            return
        const safe = meshSuggestExportBaseName(meshIndex)
        const docs = windowHelper.standardDocumentsPath()
        const fullPath = joinDirAndFile(docs, safe + ".stl")
        meshSaveDbg("openMeshExportNative fullPath=" + fullPath)
        openMeshNativeSaveWithTitleAndPath(meshIndex, qsTr("导出模型"), fullPath)
    }

    /** 另存为：系统保存对话框 */
    function openMeshSaveAsSystemDialog(meshIndex) {
        meshSaveDbg("openMeshSaveAsSystemDialog enter meshIndex=" + meshIndex)
        if (meshIndex < 0 || !scene || !windowHelper)
            return
        const safe = meshSuggestExportBaseName(meshIndex)
        const m = scene.meshModels
        let dir = windowHelper.standardDocumentsPath()
        if (meshIndex >= 0 && meshIndex < m.length) {
            const fp = String(m[meshIndex].filePath || "")
            const pdir = parentDirOfFile(fp)
            if (pdir.length > 0)
                dir = pdir
        }
        const fullPath = joinDirAndFile(dir, safe + ".stl")
        meshSaveDbg("openMeshSaveAsSystemDialog fullPath=" + fullPath)
        openMeshNativeSaveWithTitleAndPath(meshIndex, qsTr("选择导出文件夹"), fullPath)
    }

    /** 使用 C++ QFileDialog 阻塞式另存为/导出 */
    function openMeshNativeSaveWithTitleAndPath(meshIndex, dialogTitle, localFullPath) {
        if (meshIndex < 0 || !scene || !windowHelper || !appRoot)
            return
        meshSaveDbg("openMeshNativeSaveWithTitleAndPath idx=" + meshIndex + " title=" + dialogTitle + " path=" + localFullPath)
        appRoot.raise()
        appRoot.requestActivate()
        const r = windowHelper.openMeshSaveNativeDialog(appRoot, dialogTitle, localFullPath)
        const filePath = String(r.filePath || "")
        meshSaveDbg("openMeshNativeSaveWithTitleAndPath filePath=" + filePath)
        if (filePath.length === 0)
            return
        const url = windowHelper.urlFromLocalFile(filePath)
        let fmt = Number(r.format)
        if (!Number.isFinite(fmt) || fmt < 0 || fmt > 2)
            fmt = meshSaveFormatFromNameFilter("", url)
        setMeshFileExportBusy(true)
        scene.exportMeshToFileAsync(meshIndex, url, fmt)
    }

    /** 右键「另存为」：延迟后打开系统保存对话框 */
    function openMeshSaveAsNative(meshIndex) {
        meshSaveDbg("openMeshSaveAsNative meshIndex=" + meshIndex)
        scheduleMeshFileDialogFromMenu(1, meshIndex)
    }

    /** 是否存在任意激活模型（供快捷键等使用） */
    function anyMeshActive() {
        const m = scene ? scene.meshModels : null
        if (!m || m.length === 0)
            return false
        for (let i = 0; i < m.length; ++i) {
            if (m[i].active)
                return true
        }
        return false
    }
}
