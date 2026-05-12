#ifndef slic3r_GUI_App_hpp_
#define slic3r_GUI_App_hpp_

#include <string>

#include <QObject>

#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
class PresetBundle;
}

/// Qt 侧应用外壳：对齐 Orca `GUI_App` 的配置与预设生命周期（不依赖 wx/slic3r GUI 源码）。
/// 顺序：`GUI_App` 构造 → `init_app_config()` → `load_preset_bundle()` → 使用 → 析构或 `shutdown_save()`。
class GUI_App : public QObject {
    Q_OBJECT
public:
    explicit GUI_App(QObject* parent = nullptr);
    ~GUI_App() override;

    /// 引导 `data_dir` / `resources_dir`，创建 `AppConfig` 并在文件存在时 `load()`。
    void init_app_config();

    /// 将 `resources/profiles` 同步到 `data_dir/system`，创建 `PresetBundle`、`setup_directories`、`load_presets`。
    bool load_preset_bundle(std::string* error_message = nullptr);

    /// 保存 `AppConfig`（若 dirty），释放预设与配置对象。
    void shutdown_save();

    Slic3r::AppConfig* app_config() const { return m_app_config; }
    Slic3r::PresetBundle* preset_bundle() const { return m_preset_bundle; }

private:
    Slic3r::AppConfig* m_app_config{ nullptr };
    Slic3r::PresetBundle* m_preset_bundle{ nullptr };
};

#endif
