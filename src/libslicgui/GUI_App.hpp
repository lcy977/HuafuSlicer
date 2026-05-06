#include<QWidget>
#include<QObject>
#include"libslic3r/AppConfig.hpp"

using namespace Slic3r;

class GUI_App :public QObject {
    Q_OBJECT
public:
    GUI_App(QObject* parent = nullptr);
    ~GUI_App();
public:
    void init_app_config();


private:
    Slic3r::AppConfig* app_config=nullptr;
};