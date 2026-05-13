#pragma once

#include <QtGui/QHoverEvent>
#include <QtGui/QMatrix4x4>
#include <QtGui/QQuaternion>
#include <QtGui/QVector3D>
#include <QtQuick/QQuickFramebufferObject>
#include <QPointF>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include <memory>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVector>

#include "GCodeParser.h"

namespace Slic3r {
class Model;
}

class ViewportRenderer;

/** 一条已完成的平面测距线段（世界坐标，米） */
struct MeasureTrace {
    QVector3D pointA;
    QVector3D pointB;
    qreal distanceMm = 0.0;
};

class OpenGLViewport : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_DISABLE_COPY(OpenGLViewport)
    Q_PROPERTY(bool importInProgress READ importInProgress NOTIFY importInProgressChanged)
    Q_PROPERTY(QVariantList meshModels READ meshModels NOTIFY meshModelsChanged)
    /** 智能选择：导入模型时自动激活第一个模型；关闭后可以手动取消激活 */
    Q_PROPERTY(bool smartSelectEnabled READ smartSelectEnabled WRITE setSmartSelectEnabled NOTIFY smartSelectEnabledChanged)
    /** 预览工作区：在主 OpenGL 视图中绘制轨迹并隐藏模型 */
    Q_PROPERTY(bool previewMode READ previewMode WRITE setPreviewMode NOTIFY previewModeChanged)
    Q_PROPERTY(qreal progress READ progress WRITE setProgress NOTIFY progressChanged)
    Q_PROPERTY(int displayLayer READ displayLayer WRITE setDisplayLayer NOTIFY displayLayerChanged)
    Q_PROPERTY(int layerCount READ layerCount NOTIFY pathDataChanged)
    Q_PROPERTY(int totalSegments READ totalSegments NOTIFY pathDataChanged)
    Q_PROPERTY(int sourceLineCount READ sourceLineCount NOTIFY pathDataChanged)
    Q_PROPERTY(QString gcodeText READ gcodeText NOTIFY pathDataChanged)
    Q_PROPERTY(QString pathSummary READ pathSummary NOTIFY pathDataChanged)
    Q_PROPERTY(bool pathLoaded READ pathLoaded NOTIFY pathDataChanged)
    Q_PROPERTY(int playbackLine READ playbackLine NOTIFY playbackLineChanged)
    Q_PROPERTY(QString playbackGcodeWindowText READ playbackGcodeWindowText NOTIFY playbackGcodeWindowChanged)
    Q_PROPERTY(int playbackGcodeWindowStartLine READ playbackGcodeWindowStartLine NOTIFY playbackGcodeWindowChanged)
    Q_PROPERTY(int playbackGcodeWindowEndLine READ playbackGcodeWindowEndLine NOTIFY playbackGcodeWindowChanged)
    Q_PROPERTY(int playbackGcodeWindowSize READ playbackGcodeWindowSize WRITE setPlaybackGcodeWindowSize NOTIFY playbackGcodeWindowSizeChanged)
    /** 按 G-code F 与段长累计的仿真总时长（秒）；无轨迹时为 0 */
    Q_PROPERTY(qreal trajectoryDurationSec READ trajectoryDurationSec NOTIFY pathDataChanged)
    /** 各特征耗时占比：{ label, color, timeSec, ratio, percent }，按段 duration 汇总，ratio 相对整段仿真总时长 */
    Q_PROPERTY(QVariantList trajectoryFeatureStats READ trajectoryFeatureStats NOTIFY pathDataChanged)
    /** 当前仿真位置对应段的进给 F（mm/min） */
    Q_PROPERTY(qreal playbackFeedMmMin READ playbackFeedMmMin NOTIFY playbackFeedChanged)
    /** 当前可见轨迹末端位置（米，与场景一致）；用于预览状态栏 X/Y/Z（mm） */
    Q_PROPERTY(qreal trajTipXMm READ trajTipXMm NOTIFY playbackTipChanged)
    Q_PROPERTY(qreal trajTipYMm READ trajTipYMm NOTIFY playbackTipChanged)
    Q_PROPERTY(qreal trajTipZMm READ trajTipZMm NOTIFY playbackTipChanged)
    /** 当前显示层对应的层高（mm） */
    Q_PROPERTY(qreal displayLayerHeightMm READ displayLayerHeightMm NOTIFY displayLayerZInfoChanged)
    /** G-code 异步导入中（工作线程解析） */
    Q_PROPERTY(bool gcodeImportInProgress READ gcodeImportInProgress NOTIFY gcodeImportProgressChanged)
    /** G-code 解析进度 0~1（按行扫描） */
    Q_PROPERTY(qreal gcodeImportProgress READ gcodeImportProgress NOTIFY gcodeImportProgressChanged)
    /** 预览轨迹是否绘制空走（默认 false，仅影响显示，仿真时间仍含空走） */
    Q_PROPERTY(bool previewShowTravel READ previewShowTravel WRITE setPreviewShowTravel NOTIFY previewShowTravelChanged)
    /** 交互工具：0 默认移动 1 模型旋转 2 测量距离 3 镜像 4 模型缩放（滚轮） */
    Q_PROPERTY(int interactionTool READ interactionTool WRITE setInteractionTool NOTIFY interactionToolChanged)
    /** 最近一次完成的测距结果（mm）；无记录时为 0 */
    Q_PROPERTY(qreal measuredDistanceMm READ measuredDistanceMm NOTIFY measurementChanged)
    /** 平面上是否仍有已完成的测量线段（用于 UI 提示） */
    Q_PROPERTY(bool measurementReady READ measurementReady NOTIFY measurementChanged)
    /** 已完成测量线段条数（>0 时显示清除按钮） */
    Q_PROPERTY(int measurementTraceCount READ measurementTraceCount NOTIFY measurementChanged)
    Q_PROPERTY(bool undoAvailable READ undoAvailable NOTIFY undoAvailableChanged)
    Q_PROPERTY(bool redoAvailable READ redoAvailable NOTIFY redoAvailableChanged)

public:
    explicit OpenGLViewport(QQuickItem *parent = nullptr);

    Q_INVOKABLE void importModel(const QUrl &fileUrl);
    /** 用 libslic3r::Model 全量重建视口网格与 meshModels 列表（与 GuiWorkspaceHub / Plater 同步）。主线程调用。 */
    Q_INVOKABLE void syncMeshesFromSlicerModel(const Slic3r::Model* model, bool forceThroughBusy = false);
    Q_INVOKABLE void resetView();
    /** 激活模型：ctrlModifier 为 true 时切换该模型激活状态（可多选）；否则仅激活该模型并取消其余 */
    Q_INVOKABLE void setModelActive(int index, bool ctrlModifier = false);
    /** 是否在 OpenGL 视图中绘制该模型（false 为隐藏） */
    Q_INVOKABLE void setModelSceneVisible(int index, bool visible);
    /** 删除指定索引的模型 */
    Q_INVOKABLE void deleteModelAt(int index);
    /** 删除当前所有已激活的模型 */
    Q_INVOKABLE void deleteActiveModels();
    /** 清空所有已导入模型与网格数据 */
    Q_INVOKABLE void clearAllModels();
    /** 自动摆放：从热床中心向外排布全部可见模型，自动贴地并尽量利用空间 */
    Q_INVOKABLE void autoArrangeModels();
    /** 启动模型旋转模式 */
    Q_INVOKABLE void startRotateModel(int index);
    /** 停止模型旋转模式 */
    Q_INVOKABLE void stopRotateModel();
    /** 获取指定屏幕坐标下的模型索引，-1表示未命中 */
    Q_INVOKABLE int getModelAt(int x, int y);
    /** 清除平面上所有已完成的测量线段与当前未完成的测距草稿 */
    Q_INVOKABLE void clearMeasurementTraces();
    /** 对指定模型在本地坐标系内沿轴做镜像（axis：0=X 1=Y 2=Z），以几何中心为不动点；再次调用同一轴则还原 */
    Q_INVOKABLE void mirrorModelAt(int meshIndex, int localAxis);
    /** 导出单个模型：format 0=STL 二进制 1=STL ASCII 2=OBJ（含当前平移/旋转/镜像）；成功后会更新该模型的 filePath/name */
    Q_INVOKABLE bool exportMeshToFile(int meshIndex, const QUrl &fileUrl, int format);
    /** 当前模型是否有关联的、存在的 .stl/.obj 磁盘路径（可原地覆盖保存） */
    Q_INVOKABLE bool meshHasPersistentStorage(int meshIndex) const;
    /** 覆盖保存到当前 filePath；调用前应已 meshHasPersistentStorage==true */
    Q_INVOKABLE bool saveMeshInPlace(int meshIndex);
    /** 保存到源文件目录或文档目录；format：0=STL 二进制 1=STL 文本 2=OBJ（文件名带 _saved / _saved_ascii 后缀） */
    Q_INVOKABLE bool exportMeshQuickSave(int meshIndex, int format);
    /** 将每个可见模型保存到 folderUrl，文件名为模型名；format 同 exportMeshToFile */
    Q_INVOKABLE bool exportAllMeshesToFolder(const QUrl &folderUrl, int format);
    /** 异步导出（工作线程中做坐标变换与写盘，主线程仅拷贝顶点子集） */
    Q_INVOKABLE void exportMeshToFileAsync(int meshIndex, const QUrl &fileUrl, int format);
    Q_INVOKABLE void exportAllMeshesToFolderAsync(const QUrl &folderUrl, int format);
    Q_INVOKABLE void saveMeshInPlaceAsync(int meshIndex);
    Q_INVOKABLE void loadGcode(const QUrl &fileUrl);
    /** 撤销 / 恢复上一步工作区状态（模型、视角、轨迹、测量等） */
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    /** 工作线程通过队列调用：更新导入进度（0~1） */
    Q_INVOKABLE void reportGcodeImportProgress(double p);
    /** 工作线程完成后在主线程应用解析结果 */
    void completeGcodeImport(GCodeParser::ParseResult result, QString path);
    /** 预览模式：重置视角并适配轨迹范围（与主界面 resetView 独立） */
    Q_INVOKABLE void resetPreviewCamera();
    /** 已完成 k 段（k∈[0,n]）时对应的进度 0~1（按仿真时间轴） */
    Q_INVOKABLE qreal progressAtCompletedSegmentCount(int k) const;
    /** 当前进度下已完成的段数（用于步进上一段/下一段） */
    Q_INVOKABLE int completedSegmentCountAtProgress(qreal p) const;

    QVariantList meshModels() const;

    bool smartSelectEnabled() const { return m_smartSelectEnabled; }
    void setSmartSelectEnabled(bool enabled);

    float yawDegrees() const { return m_yawDeg; }
    float pitchDegrees() const { return m_pitchDeg; }
    /** 与 lookAt(eye, target, up) 一致的世界空间 up；点击视图立方体面时按面校正使汉字正向 */
    QVector3D lookAtUpWorld() const { return m_lookAtUpWorld; }
    float distance() const { return m_distance; }
    bool importInProgress() const { return m_importInProgress; }
    /** 平面相对初始姿态的旋转（枢轴为平面中心；左键拖动按屏幕左右/上下累积） */
    QQuaternion planeOrientation() const { return m_planeOrientation; }
    /** 视图立方体悬停面：0 +Z 上 1 -Z 下 2 左(-X) 3 右(+X) 4 +Y 后 5 -Y 前；-1 无 */
    int hoverViewCubeFace() const { return m_hoverViewCubeFace; }

    /** 轨道相机注视点（热床中心 + Ctrl+右键平移累积） */
    QVector3D orbitTarget() const;

    bool previewMode() const { return m_previewMode; }
    void setPreviewMode(bool on);

    qreal progress() const { return m_trajProgress; }
    void setProgress(qreal p);

    int displayLayer() const { return m_trajDisplayLayer; }
    void setDisplayLayer(int layer);

    int layerCount() const { return m_trajLayerZs.size(); }
    int totalSegments() const { return m_trajSegments.size(); }
    int sourceLineCount() const { return m_trajSourceLineCount; }
    QString gcodeText() const { return m_trajGcodeText; }
    QString pathSummary() const { return m_trajSummary; }
    bool pathLoaded() const { return !m_trajSegments.isEmpty(); }
    int playbackLine() const { return m_trajPlaybackLine; }
    QString playbackGcodeWindowText() const { return m_playbackGcodeWindowText; }
    int playbackGcodeWindowStartLine() const { return m_playbackGcodeWindowStartLine; }
    int playbackGcodeWindowEndLine() const { return m_playbackGcodeWindowEndLine; }
    int playbackGcodeWindowSize() const { return m_playbackGcodeWindowSize; }
    void setPlaybackGcodeWindowSize(int lines);
    qreal trajectoryDurationSec() const { return m_trajTotalTimeSec; }
    QVariantList trajectoryFeatureStats() const { return m_trajFeatureStats; }
    qreal playbackFeedMmMin() const { return qreal(m_trajPlaybackFeedMmMin); }
    qreal trajTipXMm() const { return qreal(m_trajTipX) * 1000.0; }
    qreal trajTipYMm() const { return qreal(m_trajTipY) * 1000.0; }
    qreal trajTipZMm() const { return qreal(m_trajTipZ) * 1000.0; }
    qreal displayLayerHeightMm() const;

    bool gcodeImportInProgress() const { return m_gcodeImportInProgress; }
    qreal gcodeImportProgress() const { return m_gcodeImportProgress; }

    bool previewShowTravel() const { return m_previewShowTravel; }
    void setPreviewShowTravel(bool on);
    int interactionTool() const { return m_interactionTool; }
    void setInteractionTool(int tool);
    qreal measuredDistanceMm() const { return m_measuredDistanceMm; }
    bool measurementReady() const { return !m_measureTraces.isEmpty(); }
    int measurementTraceCount() const { return m_measureTraces.size(); }

    bool undoAvailable() const { return !m_undoStack.isEmpty(); }
    bool redoAvailable() const { return !m_redoStack.isEmpty(); }

signals:
    void modelImportFinished(bool ok, const QString &message);
    void importInProgressChanged();
    void meshModelsChanged();
    void smartSelectEnabledChanged();
    /** 右键轻点命中模型时请求弹出菜单（QML 侧负责 popup；x/y 为视口内本地坐标，用于定位） */
    void contextMenuRequested(int modelIndex, qreal x, qreal y);
    void previewModeChanged();
    void progressChanged();
    void displayLayerChanged();
    void pathDataChanged();
    void gcodeLoadFinished(bool ok, const QString &message);
    void playbackLineChanged();
    void playbackGcodeWindowChanged();
    void playbackGcodeWindowSizeChanged();
    void playbackFeedChanged();
    void playbackTipChanged();
    void displayLayerZInfoChanged();
    void gcodeImportProgressChanged();
    void previewShowTravelChanged();
    void interactionToolChanged();
    void measurementChanged();
    /** 单模型异步导出结束（含右键保存、另存为、覆盖保存） */
    void meshSingleExportFinished(bool ok, int meshIndex, const QString &message);
    /** 批量导出到文件夹结束 */
    void meshBulkExportFinished(bool ok, const QString &message);
    void undoAvailableChanged();
    void redoAvailableChanged();

protected:
    Renderer *createRenderer() const override;
    void componentComplete() override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;

private:
    friend class ViewportRenderer;
    void recomputeTrajectoryPlaybackLine();
    void rebuildTrajectoryCumulativeTime();
    void rebuildTrajectoryFeatureStats();
    void requestPlaybackGcodeWindowUpdate(bool force = false);
    void launchPlaybackGcodeWindowBuild(int startLine, int endLine, int focusLine,
                                        const QStringList &rows);
    int visibleSegmentCountForProgress(qreal progress) const;
    void fitTrajectoryDistance();
    void snapCameraToModelFace(int faceIndex);
    void snapPlaneOrientationToAxisAligned(bool animate = true);
    int pickViewCubeFaceAt(const QPointF &itemPos) const;
    void faceAnimStep();
    // --- view-cube face snap animation ---
    QTimer *m_faceAnimTimer = nullptr;
    QElapsedTimer m_faceAnimElapsed;
    bool m_faceAnimActive = false;
    QQuaternion m_faceAnimStart;
    QQuaternion m_faceAnimTarget;
    int m_faceAnimDurationMs = 400; // milliseconds
    /** 视图立方体点击：轨道 yaw/pitch 动画（与平面四元数动画互斥） */
    bool m_orbitViewSnapActive = false;
    float m_orbitSnapYawStart = 0.f;
    float m_orbitSnapPitchStart = 0.f;
    float m_orbitSnapYawEnd = 0.f;
    float m_orbitSnapPitchEnd = 0.f;
    /** 拾取鼠标点击位置的模型索引，返回-1表示未命中任何模型 */
    int pickMeshAt(const QPointF &itemPos) const;
    bool rayFromItemPos(const QPointF &itemPos, QVector3D &rayOrigin, QVector3D &rayDir) const;
    bool intersectBedPlaneWorld(const QPointF &itemPos, QVector3D &hitWorld) const;

    QMatrix4x4 meshChunkWorldMatrix(int meshIndex) const;
    bool gatherMeshWorldTriangles(int meshIndex, QVector<QVector3D> &outTriangleVertices) const;
    static bool writeTriangleMeshStlBinary(const QString &path, const QVector<QVector3D> &triVerts, QString *errMsg);
    static bool writeTriangleMeshStlAscii(const QString &path, const QString &solidName,
                                          const QVector<QVector3D> &triVerts, QString *errMsg);
    static bool writeTriangleMeshObj(const QString &path, const QVector<QVector3D> &triVerts, QString *errMsg);
    static QString sanitizedExportBaseName(const QString &fileOrMeshName);

    struct MeshExportCpuJob {
        int meshIndex = -1;
        QString outputPath;
        int format = 0;
        QString solidName;
        QMatrix4x4 worldM;
        QVector<float> localVerts;
    };
    bool tryBuildMeshExportJob(int meshIndex, const QUrl &fileUrl, int format, MeshExportCpuJob &out,
                               QString *errMsg) const;
    static bool transformJobToFile(const MeshExportCpuJob &job, QString *errMsg);
    void dispatchSingleMeshExport(MeshExportCpuJob job);
    void dispatchBulkMeshExport(QVector<MeshExportCpuJob> jobs);

    struct ImportedMeshChunk {
        QString name;
        QString filePath;
        int firstVertex = 0;
        int vertexCount = 0;
        bool active = false;
        bool sceneVisible = true;
        // 模型在场景中的位置偏移（用于拖动）
        QVector3D positionOffset = QVector3D(0, 0, 0);
        // 模型相对初始姿态的旋转（四元数）
        QQuaternion rotation = QQuaternion();
        /** 本地镜像：各轴 ±1，在 geomCenterLocal 处缩放（等价于关于过该点且平行坐标面的平面对称） */
        QVector3D mirrorScale{1.0f, 1.0f, 1.0f};
        /** 顶点缓冲中该 chunk 的包围盒中心（本地坐标，导入时计算） */
        QVector3D geomCenterLocal{0.0f, 0.0f, 0.0f};
        /** 相对几何中心的均匀缩放（1=原始大小） */
        float uniformScale = 1.0f;
    };

    void ensureOneActiveModel();
    static void fillGeomCenterForChunk(ImportedMeshChunk &chunk, const QVector<float> &vertices);

    struct UndoWorkspaceState {
        std::shared_ptr<QVector<float>> importedMeshBuf;
        int importedMeshVertexCount = 0;
        QVector<ImportedMeshChunk> meshChunks;
        float yawDeg = kDefaultYawDeg;
        float pitchDeg = kDefaultPitchDeg;
        float distance = kDefaultDistance;
        QVector3D lookAtUpWorld{0.0f, 0.0f, 1.0f};
        QQuaternion planeOrientation;
        QVector3D viewPanWorld;
        bool previewMode = false;
        bool smartSelectEnabled = true;
        int interactionTool = 0;
        bool previewShowTravel = false;
        QVector<GCodeParser::Segment> trajSegments;
        QVector<float> trajLayerZs;
        QStringList trajSourceLines;
        QString trajGcodeText;
        QString trajSummary;
        int trajSourceLineCount = 0;
        qreal trajProgress = 0.0;
        int trajDisplayLayer = 0;
        int trajPlaybackLine = 1;
        QVector<MeasureTrace> measureTraces;
        bool measureHasFirstPoint = false;
        QVector3D measurePointA;
        QVector3D measurePointB;
        qreal measuredDistanceMm = 0.0;
    };

    bool canPushUndoSnapshot() const;
    void pushUndoSnapshot();
    UndoWorkspaceState captureUndoWorkspaceState() const;
    void restoreUndoWorkspaceState(const UndoWorkspaceState &s);
    void clearMeshSceneNoUndo();
    void deleteModelAtNoUndo(int index);

    /** 与渲染线程共享同一块顶点数据，避免大模型在主线程被多次深拷贝导致卡死 */
    std::shared_ptr<QVector<float>> m_importedMeshBuf;
    int m_importedMeshVertexCount = 0;
    quint64 m_meshDataVersion = 0;
    QVector<ImportedMeshChunk> m_meshChunks;
    bool m_meshExportAsyncBusy = false;
    bool m_importInProgress = false;
    /** 智能选择：导入时自动激活第一个模型 */
    bool m_smartSelectEnabled = true;
    /** 模型旋转模式：控制球旋转相关状态 */
    bool m_rotatingModel = false;
    int m_rotateModelIndex = -1;
    QQuaternion m_rotateStartQuaternion;
    QPointF m_rotateLastPos;

    bool m_dragging = false;
    bool m_panning = false;
    bool m_draggingMesh = false;
    int m_dragMeshIndex = -1;
    int m_pressMeshIndexForMenu = -1;
    QPointF m_lastPos;
    QPointF m_pressPos;
    QVector3D m_pressMeshPos;
    QVector3D m_pressBedHitWorld;
    int m_pressViewCubeFace = -1;
    int m_hoverViewCubeFace = -1;
    // 默认视角：约 15° 俯视，能看到平面高光
    // 默认视角：从正面约 30° 俯视，Z 轴朝上（Z-up）
    static constexpr float kDefaultYawDeg = 180.0f;
    static constexpr float kDefaultPitchDeg = 30.0f;
    static constexpr float kDefaultDistance = 1.35f;
    float m_yawDeg = kDefaultYawDeg;
    float m_pitchDeg = kDefaultPitchDeg;
    float m_distance = kDefaultDistance;
    QVector3D m_lookAtUpWorld{0.0f, 0.0f, 1.0f};
    // 默认不旋转场景坐标系（保持 Z 朝上）
    QQuaternion m_planeOrientation;
    /** Ctrl+右键拖动平移：世界空间偏移（相对默认热床中心） */
    QVector3D m_viewPanWorld;

    bool m_previewMode = false;
    QVector<GCodeParser::Segment> m_trajSegments;
    QVector<float> m_trajLayerZs;
    QStringList m_trajSourceLines;
    QString m_trajGcodeText;
    QString m_trajSummary;
    int m_trajSourceLineCount = 0;
    qreal m_trajProgress = 0.0;
    int m_trajDisplayLayer = 0;
    int m_trajPlaybackLine = 1;
    float m_trajPlaybackFeedMmMin = 0.f;
    QVector<double> m_trajCumTimeSec;
    double m_trajTotalTimeSec = 0.0;
    QVariantList m_trajFeatureStats;
    quint64 m_trajPathVersion = 0;
    float m_trajTipX = 0.f;
    float m_trajTipY = 0.f;
    float m_trajTipZ = 0.f;
    QString m_playbackGcodeWindowText;
    int m_playbackGcodeWindowStartLine = 1;
    int m_playbackGcodeWindowEndLine = 0;
    int m_playbackGcodeWindowFocusLine = 0;
    int m_playbackGcodeWindowSize = 200;
    quint64 m_playbackGcodeWindowRequestToken = 0;
    bool m_playbackGcodeWindowBuildInFlight = false;
    bool m_playbackGcodeWindowPending = false;
    int m_playbackGcodeWindowPendingStartLine = 1;
    int m_playbackGcodeWindowPendingEndLine = 0;
    int m_playbackGcodeWindowPendingFocusLine = 0;

    bool m_gcodeImportInProgress = false;
    qreal m_gcodeImportProgress = 0.0;
    bool m_previewShowTravel = false;
    int m_interactionTool = 0;
    bool m_rotateDragActive = false;
    bool m_measureHasFirstPoint = false;
    QVector3D m_measurePointA;
    QVector3D m_measurePointB;
    qreal m_measuredDistanceMm = 0.0;
    QVector<MeasureTrace> m_measureTraces;

    QVector<UndoWorkspaceState> m_undoStack;
    QVector<UndoWorkspaceState> m_redoStack;
    bool m_undoRedoApplying = false;
    /** 恢复栈后一帧内禁止入栈，避免 QML 绑定回写 preview 等触发误快照 */
    bool m_undoPostRestoreSuppress = false;
    static constexpr int kMaxUndoDepth = 25;
};
