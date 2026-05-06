#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQuickStyle>
#include <QIcon>
#include <QCoreApplication>

#include "OpenGLViewport.h"
#include "WindowHelper.h"

static void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);

    static QMutex mutex;
    QMutexLocker locker(&mutex);

    static QFile logFile;
    static bool opened = false;
    if (!opened) {
        const QString path = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("YOUTIAN3D.log");
        logFile.setFileName(path);
        opened = logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }

    QString level;
    switch (type) {
    case QtDebugMsg: level = "DEBUG"; break;
    case QtInfoMsg: level = "INFO"; break;
    case QtWarningMsg: level = "WARN"; break;
    case QtCriticalMsg: level = "CRIT"; break;
    case QtFatalMsg: level = "FATAL"; break;
    }

    const QString line = QString("[%1] [%2] %3\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(level, msg);

    if (logFile.isOpen()) {
        QTextStream ts(&logFile);
        ts << line;
        ts.flush();
    }

    fprintf(stderr, "%s", line.toLocal8Bit().constData());
    fflush(stderr);

    if (type == QtFatalMsg) {
        abort();
    }
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QQuickStyle::setStyle("Basic");

    app.setWindowIcon(QIcon(QStringLiteral("D:/XSZ/YT/ytkj/img/logo.ico")));

    qInstallMessageHandler(fileMessageHandler);
    qInfo() << "=== YOUTIAN3D started ===";

    qputenv("QSG_RHI_BACKEND", "opengl");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    qmlRegisterType<OpenGLViewport>("Youtian3D", 1, 0, "OpenGLViewport");
    qmlRegisterType<WindowHelper>("Youtian3D", 1, 0, "WindowHelper");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("Youtian3D", "Main");

    return app.exec();
}
