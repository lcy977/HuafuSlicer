#include "HuafuWorkspaceMessageHub.h"
#include "OpenGLViewport.h"

#include "GuiWorkspaceHub.hpp"
#include "Plater.hpp"

#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>

void HuafuWorkspaceMessageHub::setGuiWorkspaceHub(Slic3r::GUI::GuiWorkspaceHub *hub)
{
    m_slicer_hub = hub;
    if (m_slicer_hub) {
        // 与 GuiWorkspaceHub::notifySlicerModelChanged 内联：导入后必须能调到本 hub 上的视口同步。
        m_slicer_hub->setViewportMeshSync([this]() { applySlicerModelToViewport(); });
    }
    if (m_viewport && m_slicer_hub)
        applySlicerModelToViewport();
}

void HuafuWorkspaceMessageHub::componentComplete()
{
    QQuickItem::componentComplete();
    // 等 QML 树挂上引擎上下文后再读 `slicerWorkspace`（不依赖 main.cpp findChild）。
    QTimer::singleShot(0, this, [this]() { bindHubFromContext(); });
}

void HuafuWorkspaceMessageHub::bindHubFromContext()
{
    QQmlContext *ctx = QQmlEngine::contextForObject(this);
    if (!ctx)
        return;
    QObject *hubObj = ctx->contextProperty(QStringLiteral("slicerWorkspace")).value<QObject *>();
    auto *hub = qobject_cast<Slic3r::GUI::GuiWorkspaceHub *>(hubObj);
    if (!hub)
        return;
    setGuiWorkspaceHub(hub);
}

void HuafuWorkspaceMessageHub::applySlicerModelToViewport()
{
    if (!m_viewport || !m_slicer_hub || !m_slicer_hub->plater())
        return;
    m_viewport->syncMeshesFromSlicerModel(&m_slicer_hub->plater()->model(), true);
}

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

    if (m_viewport && m_slicer_hub)
        applySlicerModelToViewport();
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
