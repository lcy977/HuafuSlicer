#ifndef libslicgui_Qt_GUI_App_hpp_
#define libslicgui_Qt_GUI_App_hpp_

#include <string>

#include <QObject>

namespace Slic3r {
class AppConfig;
class PresetBundle;
namespace GUI {
class GuiWorkspaceHub;
namespace Qt {

/// Qt 专用门面：职责对齐 Orca `Slic3r::GUI::GUI_App`（wx `wxApp` 子类）中的应用初始化与预设生命周期，实现为 QObject，避免与 wx 版同名类在同一 TU 中共存时混淆。
/// wx 对应：`src/slic3r/GUI/GUI_App.hpp`。
/// 顺序：`GUI_App` 构造 → `init_app_config()` → `load_preset_bundle()` → 使用 → 析构或 `shutdown_save()`。
class GUI_App : public QObject {
    Q_OBJECT
public:
    explicit GUI_App(QObject* parent = nullptr);
    ~GUI_App() override;

    void init_app_config();

    bool load_preset_bundle(std::string* error_message = nullptr);

    void shutdown_save();

    Slic3r::AppConfig* app_config() const { return m_app_config; }
    Slic3r::PresetBundle* preset_bundle() const { return m_preset_bundle; }

    /// Qt / QML 工作区入口：持有 Plater 与 libslic3r::Model，导入与工程信号由此转发。
    Slic3r::GUI::GuiWorkspaceHub* workspaceHub() const { return m_workspace_hub; }

    /// 供 `QQmlContext::setContextProperty` 使用（避免 `GuiWorkspaceHub*` 匹配已删除的 `QVariant(T*)`）。
    QObject* workspaceHubObject() const;

private:
    Slic3r::AppConfig* m_app_config{ nullptr };
    Slic3r::PresetBundle* m_preset_bundle{ nullptr };
    Slic3r::GUI::GuiWorkspaceHub* m_workspace_hub{ nullptr };
};

} // namespace Qt
} // namespace GUI
} // namespace Slic3r

#endif
