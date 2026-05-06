#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class Plater;

/**
 * Qt / QML 可用的应用门面：初始化 libslic3r 资源路径、加载打印配置、驱动 Plater（导入 / 切片），
 * 与 Quick 界面通过上下文属性或 qmlRegisterType 绑定。
 */
class HuafuApplicationController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool initialized READ isInitialized NOTIFY initializedChanged)
    Q_PROPERTY(QString resourcesDirectory READ resourcesDirectory NOTIFY resourcesDirectoryChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QObject* plater READ plater CONSTANT)

public:
    explicit HuafuApplicationController(QObject* parent = nullptr);

    bool isInitialized() const { return m_initialized; }
    QString resourcesDirectory() const { return m_resourcesDir; }
    QString lastError() const { return m_lastError; }
    QObject* plater() const;

    /// 供 C++ 侧访问（完整类型在 .cpp 中）
    Plater* platerImpl() const { return m_plater; }

    /// 检测平台、设置 resources 目录（可执行文件目录下的 resources/）、初始化日志侧需要的路径。
    Q_INVOKABLE bool initialize();

    /// 从 .ini / Prusa 风格打印配置加载到当前 Plater 的 printConfig（合并键）。
    Q_INVOKABLE bool loadPrintProfile(const QUrl& fileUrl);

    /// 从 JSON（Orca/Bambu 工程或配置片段）加载；失败时回落为错误信息。
    Q_INVOKABLE bool loadPrintProfileJson(const QUrl& fileUrl);

    /// 导入一个或多个本地文件 URL（file:///）；内部使用 libslic3r::Model::read_from_file。
    Q_INVOKABLE int importModelFiles(const QVariantList& localFileUrls);

    /// 导出 G-code；outputPathTemplate 规则同 Slic3r::Print::export_gcode。
    Q_INVOKABLE bool sliceToGcode(const QString& outputPathTemplate);

    /// 从保存对话框返回的 file URL 导出（兼容 Windows 路径）。
    Q_INVOKABLE bool sliceToGcodeFile(const QUrl& outputFileUrl);

    /// 异步切片（后台线程）；结果通过 Plater::sliceFinished / sliceFailed 转发。
    Q_INVOKABLE bool sliceToGcodeAsync(const QString& outputPathTemplate);

    /// 清空 libslic3r 模型与默认配置重置（界面通过 sceneMeshesNeedRefresh 刷新视图）。
    Q_INVOKABLE void clearProject();

signals:
    void initializedChanged();
    void resourcesDirectoryChanged();
    void lastErrorChanged();
    /// 模型或配置变更后，界面应调用 OpenGLViewport::syncSceneFromApplication 刷新网格。
    void sceneMeshesNeedRefresh();

private:
    void setLastError(const QString& e);

    bool     m_initialized{false};
    QString  m_resourcesDir;
    QString  m_lastError;
    Plater*  m_plater{nullptr};
};
