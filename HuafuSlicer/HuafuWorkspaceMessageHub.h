#pragma once

#include <QPointer>
#include <QQuickItem>

#include "OpenGLViewport.h"

namespace Slic3r::GUI {
class GuiWorkspaceHub;
}

/**
 * 将 OpenGLViewport 上需由 QML 统一处理的信号转发到单一入口，并封装撤销/恢复调用，
 * 便于 Main.qml 只连接本类型而不再直接绑定 scene3d 的多处 Connections。
 */
class HuafuWorkspaceMessageHub : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(OpenGLViewport *viewport READ viewport WRITE setViewport NOTIFY viewportChanged)
    Q_PROPERTY(bool undoAvailable READ undoAvailable NOTIFY undoAvailableChanged)
    Q_PROPERTY(bool redoAvailable READ redoAvailable NOTIFY redoAvailableChanged)

public:
    explicit HuafuWorkspaceMessageHub(QQuickItem *parent = nullptr);

    OpenGLViewport *viewport() const { return m_viewport; }
    void setViewport(OpenGLViewport *vp);

    /** 与 `slicerWorkspace`（GuiWorkspaceHub）配对后，用当前 viewport 从 Plater::model() 全量同步网格。 */
    void setGuiWorkspaceHub(Slic3r::GUI::GuiWorkspaceHub *hub);

    bool undoAvailable() const;
    bool redoAvailable() const;

    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();

    /** 从 libslic3r::Model 刷新视口网格与 meshModels（主线程；绑定 viewport 与 hub 后由 C++/QML 调用）。 */
    Q_INVOKABLE void applySlicerModelToViewport();

signals:
    void viewportChanged();
    void meshSingleExportFinished(bool ok, int meshIndex, const QString &message);
    void meshBulkExportFinished(bool ok, const QString &message);
    void modelImportFinished(bool ok, const QString &message);
    void contextMenuRequested(int modelIndex, qreal x, qreal y);
    void undoAvailableChanged();
    void redoAvailableChanged();

protected:
    void componentComplete() override;

private:
    void disconnectViewport();
    void bindHubFromContext();

    QPointer<OpenGLViewport> m_viewport;
    Slic3r::GUI::GuiWorkspaceHub *m_slicer_hub{ nullptr };
};
