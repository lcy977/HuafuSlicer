#ifndef libslicgui_Plater_hpp_
#define libslicgui_Plater_hpp_
// wx 对应：`src/slic3r/GUI/Plater.*`（底板与导入管线）；本类为 Qt 门面，仅桥接 libslic3r::Model。

#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {

class AppConfig;
class PresetBundle;

namespace UndoRedo {
enum class SnapshotType : unsigned char {
    Invalid = 0,
    Normal,
    ProjectSeparator,
};
}

namespace GUI {

/// Qt 侧入口：持有 libslic3r::Model，负责文件对话框与导入管线（对齐 Slic3r/Orca 的 Plater 职责，UI 全部使用 Qt）。
class Plater : public QObject {
    Q_OBJECT
public:
    explicit Plater(QObject* parent = nullptr);
    ~Plater() override;

    void set_app_config(AppConfig* config);
    AppConfig* app_config() const { return m_app_config; }

    void set_preset_bundle(PresetBundle* bundle);
    PresetBundle* preset_bundle() const { return m_preset_bundle; }

    /// 通过 QFileDialog 选择模型文件并导入。
    void add_file();

    std::vector<size_t> load_files(const std::vector<boost::filesystem::path>& input_files,
        LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances,
        bool ask_multi = false);
    std::vector<size_t> load_files(const std::vector<std::string>& input_files,
        LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances,
        bool ask_multi = false);
    bool load_files(const QStringList& filenames);

    bool open_3mf_file(const boost::filesystem::path& file_path);
    static int get_3mf_file_count(const std::vector<boost::filesystem::path>& paths);

    const Model& model() const;
    Model& model();

    /// 加载工程（当前实现：等同按路径调用 load_files；后续可接预设保存确认、最近文件等）。
    void load_project(const std::string& filename = "", const std::string& originfile = "-");

    QString project_name() const { return m_project_name; }
    void set_project_name(const QString& name);

    /// 预留与 Orca 一致的快照 RAII；Undo/Redo 栈接入前为空操作。
    class TakeSnapshot {
    public:
        TakeSnapshot(Plater* plater, const std::string& snapshot_name);
        TakeSnapshot(Plater* plater, const std::string& snapshot_name, UndoRedo::SnapshotType snapshot_type);
        ~TakeSnapshot();

    private:
        Plater* m_plater;
    };

signals:
    /// 导入完成后发出（索引为 model().objects 下标）。
    void objects_imported(const QVector<quint64>& object_indices);
    void project_metadata_changed();

private:
    friend class TakeSnapshot;

    void take_snapshot(const std::string& name, UndoRedo::SnapshotType type = UndoRedo::SnapshotType::Normal);
    void suppress_snapshots();
    void allow_snapshots();

    struct priv;
    std::unique_ptr<priv> p;

    QString m_project_name;
    std::string m_3mf_path;
    AppConfig* m_app_config{ nullptr };
    PresetBundle* m_preset_bundle{ nullptr };

    bool m_loading_project{ false };

    int m_snapshot_suppress_depth{ 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif
