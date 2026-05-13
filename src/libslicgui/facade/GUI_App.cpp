#include "GUI_App.hpp"
#include "AppEnvironment.hpp"
#include "GuiWorkspaceHub.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/log/trivial.hpp>

#include <exception>
#include <memory>

namespace Slic3r::GUI::Qt {

GUI_App::GUI_App(QObject* parent)
    : QObject(parent)
{
    init_app_config();
    m_workspace_hub = new Slic3r::GUI::GuiWorkspaceHub(this);
    m_workspace_hub->attachApplication(this);
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
        bundle->load_presets(*m_app_config, Slic3r::EnableSystemSilent);

        delete m_preset_bundle;
        m_preset_bundle = bundle.release();
        if (m_workspace_hub)
            m_workspace_hub->syncBindingsFromApp();
        return true;
    } catch (const std::exception& ex) {
        if (error_message)
            *error_message = ex.what();
        BOOST_LOG_TRIVIAL(error) << "GUI_App::load_preset_bundle: " << ex.what();
        return false;
    }
}

QObject* GUI_App::workspaceHubObject() const
{
    return static_cast<QObject*>(m_workspace_hub);
}

void GUI_App::shutdown_save()
{
    if (m_app_config && m_app_config->dirty()) {
        try {
            m_app_config->save();
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "GUI_App::shutdown_save: AppConfig::save — " << ex.what();
        }
    }

    delete m_preset_bundle;
    m_preset_bundle = nullptr;

    delete m_app_config;
    m_app_config = nullptr;
}

} // namespace Slic3r::GUI::Qt
