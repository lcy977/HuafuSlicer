#pragma once

#include <QPointer>
#include <QQuickItem>

#include "OpenGLViewport.h"

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

    bool undoAvailable() const;
    bool redoAvailable() const;

    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();

signals:
    void viewportChanged();
    void meshSingleExportFinished(bool ok, int meshIndex, const QString &message);
    void meshBulkExportFinished(bool ok, const QString &message);
    void modelImportFinished(bool ok, const QString &message);
    void contextMenuRequested(int modelIndex, qreal x, qreal y);
    void undoAvailableChanged();
    void redoAvailableChanged();

private:
    void disconnectViewport();

    QPointer<OpenGLViewport> m_viewport;
};
