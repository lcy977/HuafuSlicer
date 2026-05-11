#include "Plater.hpp"

#include "GUI.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QString>

#include <cassert>
#include <cmath>
#include <exception>
#include <regex>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

/// 与 Orca Plater 导入分支类似的文件组合分类（用于对话框与 3MF 优先顺序）。
enum class ImportBatchKind {
    NoFile,
    Single3MF,
    SingleOther,
    Multiple3MF,
    MultipleOther,
    Multiple3MFOther,
};

enum class LoadType : unsigned char {
    Unknown,
    OpenProject,
    LoadGeometry,
    LoadConfig
};

LoadType determine_load_type_for_path(const std::string& filename)
{
    if (boost::algorithm::iends_with(filename, ".3mf"))
        return LoadType::OpenProject;
    return LoadType::LoadGeometry;
}

Vec2d bed_center_from_print_config(const DynamicPrintConfig& cfg)
{
    const ConfigOptionPoints* bed_shape = cfg.opt<ConfigOptionPoints>("bed_shape");
    if (!bed_shape || bed_shape->values.empty())
        return Vec2d(0., 0.);
    double sx = 0., sy = 0.;
    for (const auto& p : bed_shape->values) {
        sx += unscale<double>(p(0));
        sy += unscale<double>(p(1));
    }
    const size_t n = bed_shape->values.size();
    return Vec2d(sx / double(n), sy / double(n));
}

unsigned int filament_slots_from_config(const DynamicPrintConfig& cfg)
{
    const ConfigOptionStrings* cols = cfg.opt<ConfigOptionStrings>("filament_colour");
    if (!cols || cols->values.empty())
        return 1;
    return unsigned(std::max<size_t>(1, cols->values.size()));
}

void post_process_loaded_model(Model& model, DynamicPrintConfig* print_config, bool imperial_units, bool is_project_file)
{
    const float pref_deg = print_config && print_config->option("preferred_orientation") != nullptr
        ? print_config->opt_float("preferred_orientation")
        : 0.f;

    for (ModelObject* obj : model.objects) {
        if (obj->name.empty())
            obj->name = fs::path(obj->input_file).filename().string();
        if (pref_deg != 0.f)
            obj->rotate(Geometry::deg2rad(pref_deg), Axis::Z);
    }

    if (!is_project_file) {
        model.removed_objects_with_zero_volume();
        if (imperial_units)
            model.convert_from_imperial_units(false);
        else if (model.looks_like_saved_in_meters()) {
            model.convert_from_meters(true);
        }
        else if (model.looks_like_imperial_units()) {
            model.convert_from_imperial_units(true);
        }
    }

    if (!is_project_file && model.looks_like_multipart_object())
        model.convert_multipart_object(filament_slots_from_config(*print_config));

    for (ModelObject* mo : model.objects) {
        if (!is_project_file)
            mo->center_around_origin(false);
        if (!mo->instances.empty())
            mo->ensure_on_bed(is_project_file);
    }

    if (!is_project_file && print_config)
        model.center_instances_around_point(bed_center_from_print_config(*print_config));
}

std::vector<size_t> append_model_objects(Model& target, Model&& source)
{
    std::vector<size_t> idxs;
    idxs.reserve(source.objects.size());
    for (ModelObject* mo : source.objects) {
        const size_t idx = target.objects.size();
        target.add_object(*mo);
        idxs.push_back(idx);
    }
    source.clear_objects();
    return idxs;
}

} // namespace

struct Plater::priv {
    Plater* q{ nullptr };
    Model model;
    DynamicPrintConfig* print_config{ nullptr };

    explicit priv(Plater* owner)
        : q(owner)
    {
        print_config = new DynamicPrintConfig();
        print_config->apply(FullPrintConfig::defaults());
    }

    ~priv()
    {
        delete print_config;
        print_config = nullptr;
    }

    std::vector<size_t> load_files(const std::vector<fs::path>& input_files, LoadStrategy strategy, bool ask_multi);
};

Plater::Plater(QObject* parent)
    : QObject(parent)
    , p(std::make_unique<priv>(this))
{
    m_project_name = QStringLiteral("Untitled");
}

Plater::~Plater() = default;

void Plater::set_app_config(AppConfig* config) { m_app_config = config; }

void Plater::take_snapshot(const std::string& /*name*/, UndoRedo::SnapshotType /*type*/) {}

void Plater::suppress_snapshots() { ++m_snapshot_suppress_depth; }

void Plater::allow_snapshots()
{
    assert(m_snapshot_suppress_depth > 0);
    --m_snapshot_suppress_depth;
}

Plater::TakeSnapshot::TakeSnapshot(Plater* plater, const std::string& snapshot_name)
    : m_plater(plater)
{
    if (!m_plater)
        return;
    m_plater->take_snapshot(snapshot_name);
    m_plater->suppress_snapshots();
}

Plater::TakeSnapshot::TakeSnapshot(Plater* plater, const std::string& snapshot_name, UndoRedo::SnapshotType snapshot_type)
    : m_plater(plater)
{
    if (!m_plater)
        return;
    m_plater->take_snapshot(snapshot_name, snapshot_type);
    m_plater->suppress_snapshots();
}

Plater::TakeSnapshot::~TakeSnapshot()
{
    if (m_plater)
        m_plater->allow_snapshots();
}

void Plater::set_project_name(const QString& name) { m_project_name = name.isEmpty() ? QStringLiteral("Untitled") : name; }

const Model& Plater::model() const { return p->model; }

Model& Plater::model() { return p->model; }

int Plater::get_3mf_file_count(const std::vector<fs::path>& paths)
{
    int count = 0;
    for (const auto& path : paths) {
        if (boost::algorithm::iends_with(path.filename().string(), ".3mf"))
            ++count;
    }
    return count;
}

bool Plater::open_3mf_file(const fs::path& file_path)
{
    const std::string filename = file_path.filename().string();
    if (!boost::algorithm::iends_with(filename, ".3mf"))
        return false;

    const bool not_empty_plate = !model().objects.empty();
    bool load_setting_ask_when_relevant = true;
    if (m_app_config)
        load_setting_ask_when_relevant =
            m_app_config->get(SETTING_PROJECT_LOAD_BEHAVIOUR) == OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT;

    LoadType load_type = determine_load_type_for_path(filename);
    if (load_setting_ask_when_relevant && not_empty_plate) {
        // 简化：非空场景下仍按完整工程打开；后续可在此弹出 QMessageBox 询问仅几何 / 仅配置。
        load_type = LoadType::OpenProject;
    }

    switch (load_type) {
    case LoadType::OpenProject:
        load_project(fs::path(file_path).make_preferred().string(), "<loadall>");
        break;
    case LoadType::LoadGeometry: {
        TakeSnapshot snap(this, "Import Object");
        load_files({ file_path }, LoadStrategy::LoadModel);
        break;
    }
    case LoadType::LoadConfig:
        load_files({ file_path }, LoadStrategy::LoadConfig);
        break;
    case LoadType::Unknown:
    default:
        return false;
    }
    return true;
}

void Plater::load_project(const std::string& filename2, const std::string& originfile)
{
    if (m_loading_project)
        return;

    if (filename2.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "load_project: empty filename";
        return;
    }

    m_loading_project = true;

    model().calib_pa_pattern.reset(nullptr);
    model().plates_custom_gcodes.clear();

    TakeSnapshot snapshot(this, "Load Project", UndoRedo::SnapshotType::ProjectSeparator);

    std::vector<fs::path> paths;
    paths.emplace_back(filename2);
    if (originfile != "-" && originfile != "<loadall>" && originfile != "<silence>" && !originfile.empty())
        paths.emplace_back(originfile);

    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig;
    if (originfile == "<silence>")
        strategy = strategy | LoadStrategy::Silence;

    load_files(paths, strategy);

    if (originfile.empty() || originfile == "-")
        set_project_name(QStringLiteral("Untitled"));
    else if (originfile != "<loadall>" && originfile != "<silence>")
        set_project_name(QString::fromUtf8(fs::path(originfile).stem().string().c_str()));
    else
        set_project_name(QString::fromUtf8(fs::path(filename2).stem().string().c_str()));

    emit project_metadata_changed();
    m_loading_project = false;
}

void Plater::add_file()
{
    QStringList file_paths = QFileDialog::getOpenFileNames(
        nullptr,
        QObject::tr("打开模型"),
        QString(),
        QObject::tr("模型文件 (*.stl *.obj *.amf *.3mf *.step *.stp *.svg *.drc);;所有文件 (*)"));

    if (file_paths.isEmpty())
        return;

    std::vector<fs::path> paths;
    paths.reserve(file_paths.size());
    for (const QString& f : file_paths)
        paths.emplace_back(into_path(f));

    std::string snapshot_label = "Import Objects";
    if (!paths.empty()) {
        snapshot_label += ": ";
        snapshot_label += paths.front().filename().string();
        for (size_t i = 1; i < paths.size(); ++i) {
            snapshot_label += ", ";
            snapshot_label += paths[i].filename().string();
        }
    }

    const int amf_files_count = get_3mf_file_count(paths);

    ImportBatchKind loadfiles_type = ImportBatchKind::NoFile;
    if (paths.size() > 1 && amf_files_count < int(paths.size()))
        loadfiles_type = ImportBatchKind::Multiple3MFOther;
    if (paths.size() > 1 && amf_files_count == int(paths.size()))
        loadfiles_type = ImportBatchKind::Multiple3MF;
    if (paths.size() > 1 && amf_files_count == 0)
        loadfiles_type = ImportBatchKind::MultipleOther;
    if (paths.size() == 1 && amf_files_count == 1)
        loadfiles_type = ImportBatchKind::Single3MF;
    if (paths.size() == 1 && amf_files_count == 0)
        loadfiles_type = ImportBatchKind::SingleOther;

    std::vector<fs::path> first_file;
    std::vector<fs::path> tmf_file;
    std::vector<fs::path> other_file;

    switch (loadfiles_type) {
    case ImportBatchKind::Single3MF:
        open_3mf_file(paths[0]);
        break;

    case ImportBatchKind::SingleOther: {
        TakeSnapshot snapshot(this, snapshot_label);
        if (!load_files(paths, LoadStrategy::LoadModel, false).empty()) {
            if (project_name() == QStringLiteral("Untitled"))
                set_project_name(QString::fromUtf8(paths[0].stem().string().c_str()));
            emit project_metadata_changed();
        }
        break;
    }
    case ImportBatchKind::Multiple3MF:
        first_file = { paths[0] };
        for (size_t i = 1; i < paths.size(); ++i)
            other_file.push_back(paths[i]);
        open_3mf_file(first_file[0]);
        if (!load_files(other_file, LoadStrategy::LoadModel).empty())
            emit project_metadata_changed();
        break;

    case ImportBatchKind::MultipleOther: {
        TakeSnapshot snapshot(this, snapshot_label);
        if (!load_files(paths, LoadStrategy::LoadModel, true).empty()) {
            if (project_name() == QStringLiteral("Untitled"))
                set_project_name(QString::fromUtf8(paths[0].stem().string().c_str()));
            emit project_metadata_changed();
        }
        break;
    }
    case ImportBatchKind::Multiple3MFOther:
        for (const fs::path& path : paths) {
            if (boost::algorithm::iends_with(path.filename().string(), ".3mf")) {
                if (first_file.empty())
                    first_file.push_back(path);
                else
                    tmf_file.push_back(path);
            }
            else {
                other_file.push_back(path);
            }
        }
        if (!first_file.empty())
            open_3mf_file(first_file[0]);
        load_files(tmf_file, LoadStrategy::LoadModel);
        if (!load_files(other_file, LoadStrategy::LoadModel, false).empty())
            emit project_metadata_changed();
        break;
    default:
        break;
    }
}

bool Plater::load_files(const QStringList& filenames)
{
    const std::regex pattern_drop(
        R"(.*[.](stp|step|stl|oltp|obj|amf|3mf|svg|zip|drc))",
        std::regex::icase);
    const std::regex pattern_gcode_drop(".*[.](gcode|g)", std::regex::icase);

    std::vector<fs::path> normal_paths;
    std::vector<fs::path> gcode_paths;

    for (const QString& filename : filenames) {
        const fs::path path(into_path(filename));
        const std::string s = path.string();
        if (std::regex_match(s, pattern_drop))
            normal_paths.push_back(path);
        else if (std::regex_match(s, pattern_gcode_drop))
            gcode_paths.push_back(path);
    }

    if (normal_paths.empty() && gcode_paths.empty()) {
        QMessageBox::warning(nullptr, QObject::tr("导入"), QObject::tr("没有受支持的模型文件。"));
        return false;
    }

    if (normal_paths.empty()) {
        if (gcode_paths.size() > 1) {
            QMessageBox::information(nullptr, QObject::tr("G-code"), QObject::tr("一次只能打开一个 G-code 文件。"));
            return false;
        }
        QMessageBox::information(nullptr, QObject::tr("G-code"),
            QObject::tr("当前 Qt 演示管线尚未接入 G-code 预览；请在后续版本中连接查看器。"));
        return false;
    }

    if (!gcode_paths.empty()) {
        QMessageBox::warning(nullptr, QObject::tr("导入"), QObject::tr("不能同时将模型与 G-code 一起加载。"));
        return false;
    }

    std::sort(normal_paths.begin(), normal_paths.end(),
        [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });

    std::string snapshot_label = normal_paths.size() == 1 ? "Load File: " : "Load Files: ";
    snapshot_label += normal_paths.front().filename().string();
    for (size_t i = 1; i < normal_paths.size(); ++i) {
        snapshot_label += ", ";
        snapshot_label += normal_paths[i].filename().string();
    }

    auto loadfiles_type = ImportBatchKind::NoFile;
    const int amf_files_count = get_3mf_file_count(normal_paths);
    if (normal_paths.size() > 1 && amf_files_count < int(normal_paths.size()))
        loadfiles_type = ImportBatchKind::Multiple3MFOther;
    if (normal_paths.size() > 1 && amf_files_count == int(normal_paths.size()))
        loadfiles_type = ImportBatchKind::Multiple3MF;
    if (normal_paths.size() > 1 && amf_files_count == 0)
        loadfiles_type = ImportBatchKind::MultipleOther;
    if (normal_paths.size() == 1 && amf_files_count == 1)
        loadfiles_type = ImportBatchKind::Single3MF;
    if (normal_paths.size() == 1 && amf_files_count == 0)
        loadfiles_type = ImportBatchKind::SingleOther;

    std::vector<fs::path> first_file;
    std::vector<fs::path> tmf_file;
    std::vector<fs::path> other_file;

    bool res = true;

    switch (loadfiles_type) {
    case ImportBatchKind::Single3MF:
        open_3mf_file(normal_paths[0]);
        break;

    case ImportBatchKind::SingleOther: {
        TakeSnapshot snapshot(this, snapshot_label);
        if (load_files(normal_paths, LoadStrategy::LoadModel, false).empty())
            res = false;
        break;
    }
    case ImportBatchKind::Multiple3MF:
        first_file = { normal_paths[0] };
        for (size_t i = 1; i < normal_paths.size(); ++i)
            other_file.push_back(normal_paths[i]);
        open_3mf_file(first_file[0]);
        if (load_files(other_file, LoadStrategy::LoadModel).empty())
            res = false;
        break;

    case ImportBatchKind::MultipleOther: {
        TakeSnapshot snapshot(this, snapshot_label);
        if (load_files(normal_paths, LoadStrategy::LoadModel, true).empty())
            res = false;
        break;
    }

    case ImportBatchKind::Multiple3MFOther:
        for (const fs::path& path : normal_paths) {
            if (boost::algorithm::iends_with(path.filename().string(), ".3mf")) {
                if (first_file.empty())
                    first_file.push_back(path);
                else
                    tmf_file.push_back(path);
            }
            else {
                other_file.push_back(path);
            }
        }
        if (!first_file.empty())
            open_3mf_file(first_file[0]);
        if (!tmf_file.empty() && load_files(tmf_file, LoadStrategy::LoadModel).empty())
            res = false;
        if (!other_file.empty() && load_files(other_file, LoadStrategy::LoadModel, false).empty())
            res = false;
        break;
    default:
        break;
    }

    return res;
}

std::vector<size_t> Plater::load_files(const std::vector<fs::path>& input_files, LoadStrategy strategy, bool ask_multi)
{
    return p->load_files(input_files, strategy, ask_multi);
}

std::vector<size_t> Plater::load_files(const std::vector<std::string>& input_files, LoadStrategy strategy, bool ask_multi)
{
    std::vector<fs::path> paths;
    paths.reserve(input_files.size());
    for (const std::string& path : input_files)
        paths.emplace_back(path);
    return p->load_files(paths, strategy, ask_multi);
}

std::vector<size_t> Plater::priv::load_files(const std::vector<fs::path>& input_files, LoadStrategy strategy, bool ask_multi)
{
    (void)ask_multi;

    std::vector<size_t> obj_idxs;
    if (input_files.empty())
        return obj_idxs;

    q->m_3mf_path = input_files[0].string();

    const bool load_model = (strategy & LoadStrategy::LoadModel);
    const bool load_config = (strategy & LoadStrategy::LoadConfig);
    const bool imperial_units = (strategy & LoadStrategy::ImperialUnits);

    QProgressDialog progress(QObject::tr("正在加载…"), QString(), 0, 100, nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    progress.show();
    QApplication::processEvents();

    const size_t n_files = input_files.size();
    for (size_t fi = 0; fi < n_files; ++fi) {
#ifdef _WIN32
        fs::path path = input_files[fi];
        path.make_preferred();
#else
        const fs::path& path = input_files[fi];
#endif

        const int base_percent = int(100.0 * double(fi) / double(std::max<size_t>(1, n_files)));
        progress.setValue(base_percent);
        QApplication::processEvents();
        if (progress.wasCanceled())
            break;

        Model loaded;

        try {
            const bool type_3mf = boost::algorithm::iends_with(path.string(), ".3mf");
            const bool type_any_amf = !type_3mf && boost::algorithm::iends_with(path.string(), ".amf");

            if (type_3mf && load_model) {
                DynamicPrintConfig cfg_from_file;
                ConfigSubstitutionContext subs{ ForwardCompatibilitySubstitutionRule::Enable };
                En3mfType en_type = En3mfType::From_BBS;
                PlateDataPtrs plate_data;
                std::vector<Preset*> project_presets;
                Semver file_version;

                loaded = Model::read_from_archive(
                    path.string(),
                    load_config ? &cfg_from_file : nullptr,
                    load_config ? &subs : nullptr,
                    en_type,
                    strategy,
                    &plate_data,
                    &project_presets,
                    &file_version,
                    [&progress, base_percent, fi, n_files](int stage, int cur, int tot, bool& cancel) {
                        const float ratio = tot > 0 ? float(cur) / float(tot) : 0.f;
                        const float inner = (float(stage) + ratio) / 14.f;
                        const int v = base_percent + int(inner * (100 / std::max<size_t>(1, n_files)));
                        progress.setValue(std::min(99, v));
                        QApplication::processEvents();
                        cancel = progress.wasCanceled();
                    });

                for (Preset* pp : project_presets)
                    delete pp;
                project_presets.clear();
                release_PlateData_list(plate_data);

                if (load_config && !cfg_from_file.empty()) {
                    Preset::normalize(cfg_from_file);
                    print_config->apply(cfg_from_file);
                }

                const bool is_project_file = load_config;
                post_process_loaded_model(loaded, print_config, imperial_units, is_project_file);
            }
            else if (!type_3mf && load_model) {
                PlateDataPtrs plate_data;
                std::vector<Preset*> project_presets;
                bool is_xxx = false;
                Semver file_version;

                loaded = Model::read_from_file(
                    path.string(),
                    nullptr,
                    nullptr,
                    strategy,
                    &plate_data,
                    &project_presets,
                    &is_xxx,
                    &file_version,
                    nullptr,
                    nullptr,
                    nullptr,
                    0,
                    nullptr);

                for (Preset* pp : project_presets)
                    delete pp;
                project_presets.clear();
                release_PlateData_list(plate_data);

                bool effective_imperial = imperial_units;
                if (type_any_amf && is_xxx)
                    effective_imperial = true;

                post_process_loaded_model(loaded, print_config, effective_imperial, /*is_project_file*/ false);
            }

            if (load_model) {
                std::vector<size_t> ids = append_model_objects(q->model(), std::move(loaded));
                obj_idxs.insert(obj_idxs.end(), ids.begin(), ids.end());
            }
        }
        catch (const ConfigurationError& e) {
            QMessageBox::critical(nullptr, QObject::tr("配置错误"),
                QString::fromUtf8(e.what()));
        }
        catch (const std::exception& e) {
            QMessageBox::critical(nullptr, QObject::tr("加载失败"), QString::fromUtf8(e.what()));
        }
    }

    progress.setValue(100);
    QApplication::processEvents();

    QVector<quint64> qidx;
    qidx.reserve(int(obj_idxs.size()));
    for (size_t i : obj_idxs)
        qidx.push_back(quint64(i));
    emit q->objects_imported(qidx);

    return obj_idxs;
}

} // namespace GUI
} // namespace Slic3r
