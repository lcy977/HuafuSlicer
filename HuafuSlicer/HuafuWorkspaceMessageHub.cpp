#include "HuafuWorkspaceMessageHub.h"
#include "OpenGLViewport.h"

HuafuWorkspaceMessageHub::HuafuWorkspaceMessageHub(QQuickItem *parent)
    : QQuickItem(parent)
{
    setWidth(0);
    setHeight(0);
}

void HuafuWorkspaceMessageHub::disconnectViewport()
{
    if (m_viewport)
        QObject::disconnect(m_viewport, nullptr, this, nullptr);
}

void HuafuWorkspaceMessageHub::setViewport(OpenGLViewport *vp)
{
    if (m_viewport.data() == vp)
        return;

    disconnectViewport();
    m_viewport = vp;

    if (m_viewport) {
        QObject::connect(m_viewport, &OpenGLViewport::meshSingleExportFinished,
                         this, &HuafuWorkspaceMessageHub::meshSingleExportFinished);
        QObject::connect(m_viewport, &OpenGLViewport::meshBulkExportFinished,
                         this, &HuafuWorkspaceMessageHub::meshBulkExportFinished);
        QObject::connect(m_viewport, &OpenGLViewport::modelImportFinished,
                         this, &HuafuWorkspaceMessageHub::modelImportFinished);
        QObject::connect(m_viewport, &OpenGLViewport::contextMenuRequested,
                         this, &HuafuWorkspaceMessageHub::contextMenuRequested);
        QObject::connect(m_viewport, &OpenGLViewport::undoAvailableChanged,
                         this, &HuafuWorkspaceMessageHub::undoAvailableChanged);
        QObject::connect(m_viewport, &OpenGLViewport::redoAvailableChanged,
                         this, &HuafuWorkspaceMessageHub::redoAvailableChanged);
    }

    emit viewportChanged();
    emit undoAvailableChanged();
    emit redoAvailableChanged();
}

bool HuafuWorkspaceMessageHub::undoAvailable() const
{
    return m_viewport && m_viewport->undoAvailable();
}

bool HuafuWorkspaceMessageHub::redoAvailable() const
{
    return m_viewport && m_viewport->redoAvailable();
}

bool HuafuWorkspaceMessageHub::undo()
{
    return m_viewport && m_viewport->undo();
}

bool HuafuWorkspaceMessageHub::redo()
{
    return m_viewport && m_viewport->redo();
}
