#include "GuiWorkspaceHub.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"

#include "libslic3r/Model.hpp"

#include <QUrl>

#include <algorithm>

namespace fs = boost::filesystem;

namespace Slic3r::GUI {

GuiWorkspaceHub::GuiWorkspaceHub(QObject* parent)
    : QObject(parent)
    , m_plater(new Plater(this))
{
    connect(m_plater, &Plater::objects_imported, this, [this](const QVector<quint64>& idx) {
        notifySlicerModelChanged();
        emit modelImportFinished(!idx.isEmpty(),
            idx.isEmpty() ? tr("未能导入模型（已取消或格式不受支持）。") : QString());
    });
    connect(m_plater, &Plater::project_metadata_changed, this, [this]() {
        emit projectNameChanged();
    });
}

GuiWorkspaceHub::~GuiWorkspaceHub() = default;

void GuiWorkspaceHub::attachApplication(Slic3r::GUI::Qt::GUI_App* app)
{
    m_app = app;
    syncBindingsFromApp();
}

void GuiWorkspaceHub::syncBindingsFromApp()
{
    if (!m_plater)
        return;
    if (m_app) {
        m_plater->set_app_config(m_app->app_config());
        m_plater->set_preset_bundle(m_app->preset_bundle());
    } else {
        m_plater->set_app_config(nullptr);
        m_plater->set_preset_bundle(nullptr);
    }
}

QString GuiWorkspaceHub::projectName() const
{
    return m_plater ? m_plater->project_name() : QString();
}

int GuiWorkspaceHub::modelObjectCount() const
{
    return m_plater ? int(m_plater->model().objects.size()) : 0;
}

void GuiWorkspaceHub::setImportBusy(bool busy)
{
    if (m_import_busy == busy)
        return;
    m_import_busy = busy;
    emit importBusyChanged();
}

void GuiWorkspaceHub::setViewportMeshSync(std::function<void()> fn)
{
    m_viewport_mesh_sync = std::move(fn);
}

void GuiWorkspaceHub::runViewportMeshSyncNow() const
{
    if (m_viewport_mesh_sync)
        m_viewport_mesh_sync();
}

void GuiWorkspaceHub::notifySlicerModelChanged()
{
    if (m_viewport_mesh_sync)
        m_viewport_mesh_sync();
    emit modelObjectCountChanged();
    emit slicerModelChanged();
}

void GuiWorkspaceHub::importModelUrls(const QVariantList& urls)
{
    QStringList paths;
    paths.reserve(int(urls.size()));
    for (const QVariant& v : urls) {
        if (v.canConvert<QUrl>()) {
            const QUrl u = v.toUrl();
            const QString loc = u.isLocalFile() ? u.toLocalFile() : QString();
            if (!loc.isEmpty())
                paths.append(loc);
        } else {
            const QString s = v.toString();
            if (s.isEmpty())
                continue;
            const QUrl u(s);
            const QString loc = u.isLocalFile() ? u.toLocalFile() : s;
            if (!loc.isEmpty())
                paths.append(loc);
        }
    }
    importModelPaths(paths);
}

void GuiWorkspaceHub::importModelPaths(const QStringList& localPaths)
{
    if (!m_plater) {
        emit modelImportFinished(false, tr("内部错误：Plater 未初始化。"));
        return;
    }
    if (localPaths.isEmpty()) {
        emit modelImportFinished(false, tr("没有有效的文件路径。"));
        return;
    }

    setImportBusy(true);

    std::vector<fs::path> vec;
    vec.reserve(size_t(localPaths.size()));
    for (const QString& p : localPaths) {
        const QString t = p.trimmed();
        if (!t.isEmpty())
            vec.push_back(into_path(t));
    }

    if (vec.empty()) {
        setImportBusy(false);
        emit modelImportFinished(false, tr("没有有效的文件路径。"));
        return;
    }

    m_plater->load_files(vec,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances,
        false);
    setImportBusy(false);
}

void GuiWorkspaceHub::openImportModelDialog()
{
    if (m_plater)
        m_plater->add_file();
}

void GuiWorkspaceHub::removeModelObject(int index)
{
    if (!m_plater || index < 0)
        return;
    Model& m = m_plater->model();
    if (size_t(index) >= m.objects.size())
        return;
    m.delete_object(size_t(index));
    notifySlicerModelChanged();
}

void GuiWorkspaceHub::clearAllModelObjects()
{
    if (!m_plater)
        return;
    m_plater->model().clear_objects();
    notifySlicerModelChanged();
}

void GuiWorkspaceHub::removeModelObjectsByIndices(const QVariantList& indices)
{
    if (!m_plater || indices.isEmpty())
        return;
    QVector<int> idxs;
    idxs.reserve(indices.size());
    for (const QVariant& v : indices) {
        bool ok = false;
        const int i = v.toInt(&ok);
        if (ok && i >= 0)
            idxs.append(i);
    }
    if (idxs.isEmpty())
        return;
    std::sort(idxs.begin(), idxs.end(), std::greater<int>());
    idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());

    Model& m = m_plater->model();
    for (int i : std::as_const(idxs)) {
        if (size_t(i) < m.objects.size())
            m.delete_object(size_t(i));
    }
    notifySlicerModelChanged();
}

} // namespace Slic3r::GUI
