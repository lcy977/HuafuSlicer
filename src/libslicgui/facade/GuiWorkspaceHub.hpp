#ifndef libslicgui_GuiWorkspaceHub_hpp_
#define libslicgui_GuiWorkspaceHub_hpp_

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace Slic3r::GUI::Qt {
class GUI_App;
}

namespace Slic3r::GUI {

class Plater;

/// QML 门面：把导入/工程名等暴露给 View；数据在 `Plater`（libslic3r::Model）。
/// wx 对应：无单一类，职责来自 `Plater` + `MainFrame` 中与模型列表相关的绑定。
class GuiWorkspaceHub final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectNameChanged)
    Q_PROPERTY(int modelObjectCount READ modelObjectCount NOTIFY modelObjectCountChanged)
    Q_PROPERTY(bool importBusy READ importBusy NOTIFY importBusyChanged)

public:
    explicit GuiWorkspaceHub(QObject* parent = nullptr);
    ~GuiWorkspaceHub() override;

    void attachApplication(Slic3r::GUI::Qt::GUI_App* app);

    QString projectName() const;
    int modelObjectCount() const;
    bool importBusy() const { return m_import_busy; }

    Plater* plater() { return m_plater; }
    const Plater* plater() const { return m_plater; }

    Q_INVOKABLE void importModelUrls(const QVariantList& urls);

    Q_INVOKABLE void importModelPaths(const QStringList& localPaths);

    Q_INVOKABLE void openImportModelDialog();

    /** 按索引删除 Model 中的对象（与视口列表行一一对应），并发 slicerModelChanged。 */
    Q_INVOKABLE void removeModelObject(int index);
    Q_INVOKABLE void clearAllModelObjects();
    /** 删除多个对象；indices 可为任意顺序，内部按降序删除。 */
    Q_INVOKABLE void removeModelObjectsByIndices(const QVariantList& indices);

    void syncBindingsFromApp();

    /** 由 Qt 应用在加载 QML 后注入：在 Model 变更时查找 OpenGLViewport 并同步网格（避免 main 侧 findChild 失败）。 */
    void setViewportMeshSync(std::function<void()> fn);

    /** 仅执行已注册的视口同步（不改变 model 信号）。用于 QML 完成布局后再补一次同步。 */
    Q_INVOKABLE void runViewportMeshSyncNow() const;

signals:
    void projectNameChanged();
    void modelObjectCountChanged();
    void importBusyChanged();

    void modelImportFinished(bool ok, const QString& message);

    void slicerModelChanged();

private:
    void setImportBusy(bool busy);

    void notifySlicerModelChanged();

    Slic3r::GUI::Qt::GUI_App* m_app{ nullptr };
    Plater* m_plater{ nullptr };
    bool m_import_busy{ false };
    std::function<void()> m_viewport_mesh_sync;
};

} // namespace Slic3r::GUI

#endif
