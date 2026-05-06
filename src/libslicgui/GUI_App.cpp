#include "GUI_App.hpp"
#include"libslic3r/Utils.hpp"
#include"libslic3r/Model.hpp"
#include"libslic3r/PresetBundle.hpp"
#include <QCoreApplication>
#include <QDir>
GUI_App::GUI_App(QObject *parent):QObject(parent){
    init_app_config();
}
GUI_App::~GUI_App(){
    //destructor
}
void GUI_App::init_app_config(){
    //init app config
    if (Slic3r::data_dir().empty()) {
        QString exePath = QCoreApplication::applicationFilePath();

        // 获取所在目录
        QDir exeDir = QFileInfo(exePath).dir();
        auto _app_folder = boost::filesystem::path(exeDir.absolutePath().toUtf8().data()).parent_path();
        boost::filesystem::path app_data_dir_path = _app_folder / "data_dir";
        if(!boost::filesystem::exists(app_data_dir_path)){
            Slic3r::set_data_dir(app_data_dir_path.string());
        }
        else{
            boost::filesystem::path data_dir_path;
            if (!boost::filesystem::exists(data_dir_path)){
                boost::filesystem::create_directory(data_dir_path);
            }
        }
    }

    if(!app_config){
        app_config = new AppConfig();
    }
}