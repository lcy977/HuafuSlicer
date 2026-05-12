#include "GUI_App.hpp"
#include "AppEnvironment.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/log/trivial.hpp>

#include <exception>
#include <memory>

GUI_App::GUI_App(QObject* parent)
    : QObject(parent)
{
    init_app_config();
}

GUI_App::~GUI_App()
{
    shutdown_save();
}

void GUI_App::init_app_config()
{
    Slic3r::GUI::bootstrap_data_dir();
    Slic3r::GUI::bootstrap_resources_dir();

    if (!m_app_config)
        m_app_config = new Slic3r::AppConfig();

    if (m_app_config->exists()) {
        const std::string err = m_app_config->load();
        if (!err.empty())
            BOOST_LOG_TRIVIAL(error) << "GUI_App: AppConfig::load — " << err;
    }
}

bool GUI_App::load_preset_bundle(std::string* error_message)
{
    try {
        std::string sync_err;
        if (!Slic3r::GUI::sync_resource_profiles_to_system(&sync_err)) {
            if (error_message)
                *error_message = sync_err;
            return false;
        }
        if (!sync_err.empty())
            BOOST_LOG_TRIVIAL(warning) << "GUI_App: profile sync note: " << sync_err;

        std::unique_ptr<Slic3r::PresetBundle> bundle(new Slic3r::PresetBundle());
        bundle->setup_directories();
        // 非 scoped enum，枚举符在 Slic3r 命名空间（勿写 ForwardCompatibilitySubstitutionRule::，部分 MSVC 会报 C2653）
        bundle->load_presets(*m_app_config, Slic3r::EnableSystemSilent);

        delete m_preset_bundle;
        m_preset_bundle = bundle.release();
        return true;
    } catch (const std::exception& ex) {
        if (error_message)
            *error_message = ex.what();
        BOOST_LOG_TRIVIAL(error) << "GUI_App::load_preset_bundle: " << ex.what();
        return false;
    }
}

void GUI_App::shutdown_save()
{
    if (m_app_config && m_app_config->dirty())
        m_app_config->save();

    delete m_preset_bundle;
    m_preset_bundle = nullptr;

    delete m_app_config;
    m_app_config = nullptr;
}
