#include "AppEnvironment.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <cstdlib>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

fs::path application_executable_directory()
{
    const QString exe = QCoreApplication::applicationFilePath();
    return fs::path(QFileInfo(exe).absoluteDir().absolutePath().toUtf8().data());
}

} // namespace

void bootstrap_data_dir()
{
    if (!data_dir().empty())
        return;

    const fs::path exe_dir = application_executable_directory();
    const fs::path portable = exe_dir / "data_dir";
    if (fs::exists(portable) && fs::is_directory(portable)) {
        set_data_dir(fs::path(portable).make_preferred().string());
        BOOST_LOG_TRIVIAL(info) << "libslicgui: using portable data_dir " << data_dir();
        return;
    }

    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) {
        set_data_dir(fs::path(exe_dir / "data_dir").make_preferred().string());
        BOOST_LOG_TRIVIAL(warning) << "libslicgui: GenericDataLocation empty; fallback data_dir " << data_dir();
        return;
    }

    const fs::path user_data = fs::path(base.toUtf8().constData()) / "HuafuSlicer";
    set_data_dir(fs::path(user_data).make_preferred().string());
    BOOST_LOG_TRIVIAL(info) << "libslicgui: using user data_dir " << data_dir();
}

void bootstrap_resources_dir()
{
    if (!resources_dir().empty())
        return;

    if (const char* env = std::getenv("HUAFU_RESOURCES"); env != nullptr && *env != '\0') {
        set_resources_dir(fs::path(env).make_preferred().string());
        BOOST_LOG_TRIVIAL(info) << "libslicgui: resources_dir from HUAFU_RESOURCES " << resources_dir();
        return;
    }

    const fs::path exe_dir = application_executable_directory();
    const std::initializer_list<fs::path> candidates = {
        exe_dir / "resources",
        exe_dir.parent_path() / "resources",
        exe_dir.parent_path().parent_path() / "resources",
    };

    for (const fs::path& r : candidates) {
        try {
            const fs::path profiles = r / "profiles";
            if (fs::exists(profiles) && fs::is_directory(profiles)) {
                set_resources_dir(fs::path(r).make_preferred().string());
                BOOST_LOG_TRIVIAL(info) << "libslicgui: resources_dir " << resources_dir();
                return;
            }
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "libslicgui: resources candidate failed: " << ex.what();
        }
    }

    const fs::path fallback = exe_dir / "resources";
    if (fs::exists(fallback)) {
        set_resources_dir(fs::path(fallback).make_preferred().string());
        BOOST_LOG_TRIVIAL(warning) << "libslicgui: resources_dir set without profiles subdirectory: " << resources_dir();
    } else {
        BOOST_LOG_TRIVIAL(warning) << "libslicgui: could not locate resources; set HUAFU_RESOURCES or ship resources next to the executable";
    }
}

bool sync_resource_profiles_to_system(std::string* error_summary)
{
    const std::string& rsrc_root = resources_dir();
    if (rsrc_root.empty()) {
        if (error_summary)
            error_summary->clear();
        return true;
    }

    const fs::path src = (fs::path(rsrc_root) / "profiles").make_preferred();
    if (!fs::exists(src) || !fs::is_directory(src)) {
        BOOST_LOG_TRIVIAL(info) << "libslicgui: no resources/profiles at " << src.string() << ", skip sync";
        if (error_summary)
            error_summary->clear();
        return true;
    }

    const fs::path dst = (fs::path(data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    boost::system::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        if (error_summary)
            *error_summary = ec.message();
        return false;
    }

    std::string local_errors;
    for (fs::directory_iterator it(src); it != fs::directory_iterator(); ++it) {
        const fs::path& p = it->path();
        if (!fs::is_regular_file(p))
            continue;
        if (!is_json_file(p.string()))
            continue;

        const fs::path out = dst / p.filename();
        bool do_copy = true;
        if (fs::exists(out)) {
            std::time_t t_src = fs::last_write_time(p, ec);
            std::time_t t_dst = fs::last_write_time(out, ec);
            do_copy = (t_src > t_dst);
        }
        if (!do_copy)
            continue;

        std::string err;
        const CopyFileResult cr = copy_file(p.string(), out.string(), err, false);
        if (cr != SUCCESS) {
            BOOST_LOG_TRIVIAL(error) << "libslicgui: copy " << p << " -> " << out << " : " << err;
            if (!local_errors.empty())
                local_errors += "\n";
            local_errors += err;
        }
    }

    if (error_summary)
        *error_summary = local_errors;
    return local_errors.empty();
}

} // namespace GUI
} // namespace Slic3r
