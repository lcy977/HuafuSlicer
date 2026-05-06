#pragma once

#include <QtGui/QHoverEvent>
#include <QtGui/QQuaternion>
#include <QtGui/QVector3D>
#include <QtQuick/QQuickFramebufferObject>
#include <QPointF>
#include <memory>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVector>

#include "GCodeParser.h"

class ViewportRenderer;

class OpenGLViewport : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_DISABLE_COPY(OpenGLViewport)
    Q_PROPERTY(bool importInProgress READ importInProgress NOTIFY importInProgressChanged)
    Q_PROPERTY(QVariantList meshModels READ meshModels NOTIFY meshModelsChanged)
    /** 是否所有模型均在「选中」列勾选（列表为空时为 false） */
    Q_PROPERTY(bool allModelsSelected READ allModelsSelected WRITE setAllModelsSelected NOTIFY meshModelsChanged)
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
    /** 按 G-code F 与段长累计的仿真总时长（秒）；无轨迹时为 0 */
    Q_PROPERTY(qreal trajectoryDurationSec READ trajectoryDurationSec NOTIFY pathDataChanged)
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

public:
    explicit OpenGLViewport(QQuickItem *parent = nullptr);

    Q_INVOKABLE void importModel(const QUrl &fileUrl);
    Q_INVOKABLE void resetView();
    /** 多选：勾选/取消「选中」，可多个同时选中（用于视图红色高亮等） */
    Q_INVOKABLE void setModelSelected(int index, bool selected);
    /** 单选：同一时间只有一个「激活」模型 */
    Q_INVOKABLE void setModelActive(int index);
    /** 是否在 OpenGL 视图中绘制该模型（false 为隐藏） */
    Q_INVOKABLE void setModelSceneVisible(int index, bool visible);
    /** 「选中」列：全部勾选 / 全部取消（仅影响多选高亮，不删模型） */
    Q_INVOKABLE void selectAllModels();
    Q_INVOKABLE void deselectAllModels();
    /** 删除指定索引的模型 */
    Q_INVOKABLE void deleteModelAt(int index);
    /** 清空所有已导入模型与网格数据 */
    Q_INVOKABLE void clearAllModels();
    /** 启动模型旋转模式：在指定模型几何中心显示控制球 */
    Q_INVOKABLE void startRotateModel(int index);
    /** 停止模型旋转模式 */
    Q_INVOKABLE void stopRotateModel();
    /** 获取指定屏幕坐标下的模型索引，-1表示未命中 */
    Q_INVOKABLE int getModelAt(int x, int y);
    Q_INVOKABLE void loadGcode(const QUrl &fileUrl);
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

    bool allModelsSelected() const;
    void setAllModelsSelected(bool all);

    bool smartSelectEnabled() const { return m_smartSelectEnabled; }
    void setSmartSelectEnabled(bool enabled);

    float yawDegrees() const { return m_yawDeg; }
    float pitchDegrees() const { return m_pitchDeg; }
    float distance() const { return m_distance; }
    bool importInProgress() const { return m_importInProgress; }
    /** 平面相对初始姿态的旋转（枢轴为平面中心；左键拖动按屏幕左右/上下累积） */
    QQuaternion planeOrientation() const { return m_planeOrientation; }
    /** 视图立方体悬停面：0 前(+Z) 1 后 2 顶 3 底 4 右 5 左；-1 无 */
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
    qreal trajectoryDurationSec() const { return m_trajTotalTimeSec; }
    qreal playbackFeedMmMin() const { return qreal(m_trajPlaybackFeedMmMin); }
    qreal trajTipXMm() const { return qreal(m_trajTipX) * 1000.0; }
    qreal trajTipYMm() const { return qreal(m_trajTipY) * 1000.0; }
    qreal trajTipZMm() const { return qreal(m_trajTipZ) * 1000.0; }
    qreal displayLayerHeightMm() const;

    bool gcodeImportInProgress() const { return m_gcodeImportInProgress; }
    qreal gcodeImportProgress() const { return m_gcodeImportProgress; }

    bool previewShowTravel() const { return m_previewShowTravel; }
    void setPreviewShowTravel(bool on);

signals:
    void modelImportFinished(bool ok, const QString &message);
    void importInProgressChanged();
    void meshModelsChanged();
    void smartSelectEnabledChanged();
    /** 右键轻点命中模型时请求弹出菜单（QML 侧负责 popup） */
    void contextMenuRequested(int modelIndex);
    void previewModeChanged();
    void progressChanged();
    void displayLayerChanged();
    void pathDataChanged();
    void gcodeLoadFinished(bool ok, const QString &message);
    void playbackLineChanged();
    void playbackFeedChanged();
    void playbackTipChanged();
    void displayLayerZInfoChanged();
    void gcodeImportProgressChanged();
    void previewShowTravelChanged();

protected:
    Renderer *createRenderer() const override;

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
    int visibleSegmentCountForProgress(qreal progress) const;
    void fitTrajectoryDistance();
    void snapCameraToModelFace(int faceIndex);
    int pickViewCubeFaceAt(const QPointF &itemPos) const;
    /** 拾取鼠标点击位置的模型索引，返回-1表示未命中任何模型 */
    int pickMeshAt(const QPointF &itemPos) const;
    bool rayFromItemPos(const QPointF &itemPos, QVector3D &rayOrigin, QVector3D &rayDir) const;
    bool intersectBedPlaneWorld(const QPointF &itemPos, QVector3D &hitWorld) const;

    struct ImportedMeshChunk {
        QString name;
        QString filePath;
        int firstVertex = 0;
        int vertexCount = 0;
        bool selected = false;
        bool active = false;
        bool sceneVisible = true;
        // 模型在场景中的位置偏移（用于拖动）
        QVector3D positionOffset = QVector3D(0, 0, 0);
        // 模型相对初始姿态的旋转（四元数）
        QQuaternion rotation = QQuaternion();
    };

    void ensureOneActiveModel();

    /** 与渲染线程共享同一块顶点数据，避免大模型在主线程被多次深拷贝导致卡死 */
    std::shared_ptr<QVector<float>> m_importedMeshBuf;
    int m_importedMeshVertexCount = 0;
    quint64 m_meshDataVersion = 0;
    QVector<ImportedMeshChunk> m_meshChunks;
    /** 与 m_meshChunks[].active 同步，供渲染线程在 synchronize 时可靠读取（-1 表示无） */
    int m_activeModelIndex = -1;
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
    // 默认不旋转场景坐标系（保持 Z 朝上）
    QQuaternion m_planeOrientation;
    /** Ctrl+右键拖动平移：世界空间偏移（相对默认热床中心） */
    QVector3D m_viewPanWorld;

    bool m_previewMode = false;
    QVector<GCodeParser::Segment> m_trajSegments;
    QVector<float> m_trajLayerZs;
    QString m_trajGcodeText;
    QString m_trajSummary;
    int m_trajSourceLineCount = 0;
    qreal m_trajProgress = 0.0;
    int m_trajDisplayLayer = 0;
    int m_trajPlaybackLine = 1;
    float m_trajPlaybackFeedMmMin = 0.f;
    QVector<double> m_trajCumTimeSec;
    double m_trajTotalTimeSec = 0.0;
    quint64 m_trajPathVersion = 0;
    float m_trajTipX = 0.f;
    float m_trajTipY = 0.f;
    float m_trajTipZ = 0.f;

    bool m_gcodeImportInProgress = false;
    qreal m_gcodeImportProgress = 0.0;
    bool m_previewShowTravel = false;
};
