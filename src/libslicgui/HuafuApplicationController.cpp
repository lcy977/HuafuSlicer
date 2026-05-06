#include "HuafuApplicationController.hpp"

#include "Plater.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QUrl>

#include "libslic3r/Config.hpp"
#include "libslic3r/Platform.hpp"
#include "libslic3r/Utils.hpp"

HuafuApplicationController::HuafuApplicationController(QObject* parent)
    : QObject(parent)
{
    m_plater = new Plater(this);
    connect(m_plater, &Plater::modelChanged, this, &HuafuApplicationController::sceneMeshesNeedRefresh);
}

QObject* HuafuApplicationController::plater() const
{
    return m_plater;
}

void HuafuApplicationController::setLastError(const QString& e)
{
    if (m_lastError == e)
        return;
    m_lastError = e;
    emit lastErrorChanged();
}

bool HuafuApplicationController::initialize()
{
    setLastError({});

    Slic3r::detect_platform();

    const QString appDir = QCoreApplication::applicationDirPath();
    QDir          dir(appDir);
    const QString res    = dir.absoluteFilePath(QStringLiteral("resources"));

    m_resourcesDir = res;
    emit resourcesDirectoryChanged();

    try {
        Slic3r::set_resources_dir(res.toStdString());
        // 与 CLI 常见布局对齐：资源下的子目录
        Slic3r::set_var_dir((QDir(res).absoluteFilePath(QStringLiteral("images"))).toStdString());
        Slic3r::set_local_dir((QDir(res).absoluteFilePath(QStringLiteral("i18n"))).toStdString());
        Slic3r::set_sys_shapes_dir((QDir(res).absoluteFilePath(QStringLiteral("shapes"))).toStdString());
        Slic3r::set_custom_gcodes_dir((QDir(res).absoluteFilePath(QStringLiteral("custom_gcodes"))).toStdString());
    } catch (...) {
        setLastError(QStringLiteral("设置资源路径失败"));
        return false;
    }

    // 数据目录默认可执行目录旁 user 目录（无则仅有默认配置）
    const QString dataDir = dir.absoluteFilePath(QStringLiteral("HuafuSlicerData"));
    QDir().mkpath(dataDir);
    try {
        Slic3r::set_data_dir(dataDir.toStdString());
    } catch (...) {
        // 非致命
    }

    if (!m_initialized) {
        m_initialized = true;
        emit initializedChanged();
    }
    return true;
}

bool HuafuApplicationController::loadPrintProfile(const QUrl& fileUrl)
{
    setLastError({});
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty()) {
        setLastError(QStringLiteral("无效的配置文件路径"));
        return false;
    }
    try {
        m_plater->printConfig().load_from_ini(path.toStdString(), Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        emit sceneMeshesNeedRefresh();
        return true;
    } catch (const std::exception& e) {
        setLastError(QString::fromUtf8(e.what()));
        return false;
    }
}

bool HuafuApplicationController::loadPrintProfileJson(const QUrl& fileUrl)
{
    setLastError({});
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty()) {
        setLastError(QStringLiteral("无效的配置文件路径"));
        return false;
    }
    try {
        std::map<std::string, std::string> keyvals;
        std::string                         reason;
        Slic3r::ConfigSubstitutionContext   subst(Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        const int r = m_plater->printConfig().load_from_json(path.toStdString(), subst, false, keyvals, reason);
        if (r != 0) {
            setLastError(QString::fromStdString(reason.empty() ? "load_from_json failed" : reason));
            return false;
        }
        emit sceneMeshesNeedRefresh();
        return true;
    } catch (const std::exception& e) {
        setLastError(QString::fromUtf8(e.what()));
        return false;
    }
}

int HuafuApplicationController::importModelFiles(const QVariantList& localFileUrls)
{
    setLastError({});
    QStringList paths;
    paths.reserve(localFileUrls.size());
    for (const QVariant& v : localFileUrls) {
        const QUrl u = v.toUrl();
        QString    p = u.isLocalFile() ? u.toLocalFile() : v.toString();
        if (!p.isEmpty())
            paths.push_back(p);
    }
    if (paths.isEmpty()) {
        setLastError(QStringLiteral("没有有效的文件路径"));
        return 0;
    }

    const int n = m_plater->loadFiles(paths);
    if (n <= 0)
        setLastError(QStringLiteral("未能导入任何模型"));
    return n;
}

bool HuafuApplicationController::sliceToGcode(const QString& outputPathTemplate)
{
    setLastError({});
    QString err;
    const bool ok = m_plater->sliceToGcode(outputPathTemplate, nullptr, &err);
    if (!ok)
        setLastError(err);
    return ok;
}

void HuafuApplicationController::clearProject()
{
    m_plater->clear();
}

bool HuafuApplicationController::sliceToGcodeFile(const QUrl& outputFileUrl)
{
    return sliceToGcode(outputFileUrl.toLocalFile());
}

bool HuafuApplicationController::sliceToGcodeAsync(const QString& outputPathTemplate)
{
    setLastError({});
    QString err;
    const bool ok = m_plater->requestSliceAsync(outputPathTemplate, &err);
    if (!ok)
        setLastError(err);
    return ok;
}
