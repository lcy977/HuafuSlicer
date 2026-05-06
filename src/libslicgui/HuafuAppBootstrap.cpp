#include "HuafuAppBootstrap.hpp"

#include <QQmlContext>

#include <QQmlApplicationEngine>

#include "HuafuApplicationController.hpp"

void huafuAttachSlicerAppToQmlEngine(QQmlApplicationEngine& engine)
{
    static HuafuApplicationController slicerApp;
    slicerApp.initialize();
    engine.rootContext()->setContextProperty(QStringLiteral("slicerApp"), &slicerApp);
}
