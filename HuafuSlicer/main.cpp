#include <QFont>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QtQml>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QLibraryInfo>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQuickStyle>
#include <QIcon>
#include <QCoreApplication>
#include <QQuickImageProvider>
#include <QTimer>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSvgRenderer>
#include <QResource>
#include <atomic>

#include "OpenGLViewport.h"
#include "HuafuWorkspaceMessageHub.h"
#include "WindowHelper.h"

#include "GUI_App.hpp"
#include "GuiWorkspaceHub.hpp"
#include "Plater.hpp"
#include "libslic3r/Thread.hpp"

namespace {

    void logQtImageFormatsOnce()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        QStringList fmts;
        for (const QByteArray& f : QImageReader::supportedImageFormats())
            fmts.append(QString::fromLatin1(f));
        qInfo() << "[HuafuSlicer] QImageReader supported formats:" << fmts.join(QLatin1Char(','));
    }

    void logResourceProbe()
    {
        const QString png = QStringLiteral(":/huafuslicer/resources/import_model.png");
        const QString svg = QStringLiteral(":/huafuslicer/resources/ic_import_gcode.svg");
        const bool pngExists = QFile::exists(png);
        const bool svgExists = QFile::exists(svg);
        qInfo() << "[HuafuSlicer] probe QFile::exists png =" << pngExists << png;
        qInfo() << "[HuafuSlicer] probe QFile::exists svg =" << svgExists << svg;
        if (pngExists)
            qInfo() << "[HuafuSlicer] png size bytes (QFile)" << QFile(png).size();
        QResource rp(png);
        qInfo() << "[HuafuSlicer] QResource registered for png path?" << rp.isValid()
            << "size" << (rp.isValid() ? rp.size() : qint64(-1));
    }

    class HuafuSlicerIconImageProvider final : public QQuickImageProvider
    {
    public:
        HuafuSlicerIconImageProvider()
            : QQuickImageProvider(QQuickImageProvider::Image)
        {
        }

        QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override
        {
            static std::atomic<int> s_logBudget{ 40 };

            QString key = id;
            const int slash = key.indexOf(QLatin1Char('/'));
            if (slash > 0)
                key = key.left(slash);

            const bool doLog = (s_logBudget.fetch_sub(1) > 0);
            if (doLog) {
                qInfo() << "[HuafuSlicer] requestImage rawId=" << id << "key=" << key
                    << "requestedSize=" << requestedSize;
            }

            QImage base;
            if (key == QLatin1String("import_model")) {
                const QString path = QStringLiteral(":/huafuslicer/resources/import_model.png");
                base = QImage(path);
                if (base.isNull() && doLog)
                    qWarning() << "[HuafuSlicer] QImage load FAILED for" << path;
                else if (doLog)
                    qInfo() << "[HuafuSlicer] PNG ok size" << base.size();
            }
            else if (key == QLatin1String("import_gcode")) {
                const QString path = QStringLiteral(":/huafuslicer/resources/ic_import_gcode.svg");
                QSvgRenderer renderer(path);
                if (doLog)
                    qInfo() << "[HuafuSlicer] QSvgRenderer valid=" << renderer.isValid() << "defaultSize=" << renderer.defaultSize();
                if (renderer.isValid()) {
                    QSize def = renderer.defaultSize();
                    if (!def.isValid() || def.isEmpty())
                        def = QSize(48, 48);
                    def = def.scaled(128, 128, Qt::KeepAspectRatio);
                    base = QImage(def, QImage::Format_ARGB32_Premultiplied);
                    base.fill(Qt::transparent);
                    QPainter p(&base);
                    renderer.render(&p);
                    p.end();
                }
                else if (doLog) {
                    qWarning() << "[HuafuSlicer] QSvgRenderer FAILED for" << path;
                }
            }
            else if (doLog) {
                qWarning() << "[HuafuSlicer] unknown key" << key;
            }

            if (base.isNull()) {
                if (size)
                    *size = QSize();
                if (doLog)
                    qWarning() << "[HuafuSlicer] returning NULL image for key" << key;
                return {};
            }

            QImage out = base;
            if (requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0) {
                const QSize target = requestedSize.boundedTo(QSize(256, 256));
                out = base.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                if (doLog)
                    qInfo() << "[HuafuSlicer] scaled to" << out.size() << "from requested" << requestedSize;
            }

            if (size)
                *size = out.size();
            if (doLog)
                qInfo() << "[HuafuSlicer] return size" << out.size();
            return out;
        }
    };

    static void trySetWindowIcon(QApplication& app)
    {
        const QStringList candidates = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/logo.ico"),
            QStringLiteral("D:/XSZ/YT/ytkj/img/logo.ico"),
        };
        for (const QString& p : candidates) {
            if (QFile::exists(p)) {
                app.setWindowIcon(QIcon(p));
                return;
            }
        }
    }

} // namespace

static void fileMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    Q_UNUSED(context);

    static QMutex mutex;
    QMutexLocker locker(&mutex);

    static QFile logFile;
    static bool opened = false;
    if (!opened) {
        const QString path = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("HuafuSlicer.log");
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

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    // libslic3r::AppConfig::save() 要求 is_main_thread_active()；wx 入口外若未调用会误判为「工作线程」并抛出 CriticalException。
    Slic3r::save_main_thread_id();

    {
        QFont uiFont = app.font();
        uiFont.setFamilies(
            { QStringLiteral("PingFang SC"), QStringLiteral("PingFang TC"), QStringLiteral("PingFang HK"),
             QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
             QStringLiteral("Segoe UI") });
        uiFont.setPixelSize(14);
        uiFont.setStyleHint(QFont::SansSerif);
        uiFont.setStyleStrategy(QFont::PreferAntialias);
        QGuiApplication::setFont(uiFont);
    }

    QQuickStyle::setStyle("Basic");

    trySetWindowIcon(app);

    qInstallMessageHandler(fileMessageHandler);
    qInfo() << "=== HuafuSlicer started ===";

    qputenv("QSG_RHI_BACKEND", "opengl");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    qmlRegisterType<OpenGLViewport>("HuafuSlicer", 1, 0, "OpenGLViewport");
    qmlRegisterType<HuafuWorkspaceMessageHub>("HuafuSlicer", 1, 0, "HuafuWorkspaceMessageHub");
    qmlRegisterType<WindowHelper>("HuafuSlicer", 1, 0, "WindowHelper");

    Slic3r::GUI::Qt::GUI_App guiApp(&app);
    {
        std::string presetErr;
        if (!guiApp.load_preset_bundle(&presetErr)) {
            qWarning() << "[HuafuSlicer] load_preset_bundle failed:" << QString::fromStdString(presetErr);
        }
    }

    logQtImageFormatsOnce();
    logResourceProbe();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("slicerWorkspace"), guiApp.workspaceHubObject());
    const QString appDir = QCoreApplication::applicationDirPath();
    engine.addImportPath(appDir);
    engine.addImportPath(appDir + QStringLiteral("/qml"));
    engine.addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));
    QCoreApplication::addLibraryPath(QLibraryInfo::path(QLibraryInfo::PluginsPath));
    qInfo() << "[HuafuSlicer] QmlImportsPath" << QLibraryInfo::path(QLibraryInfo::QmlImportsPath);
    qInfo() << "[HuafuSlicer] PluginsPath" << QLibraryInfo::path(QLibraryInfo::PluginsPath);

    QObject::connect(
        &engine,
        &QQmlEngine::warnings,
        [](const QList<QQmlError>& warnings) {
            for (const QQmlError& w : warnings)
                qWarning() << "[QML]" << w.toString();
        });

    engine.addImageProvider(QStringLiteral("huafuslicer"), new HuafuSlicerIconImageProvider());
    qInfo() << "[HuafuSlicer] addImageProvider huafuslicer -> HuafuSlicerIconImageProvider";
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() {
            qCritical() << "[HuafuSlicer] objectCreationFailed (see stderr / HuafuSlicer.log for [QML] lines)";
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("HuafuSlicer"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        qWarning() << "[HuafuSlicer] loadFromModule produced no root; trying qrc Main.qml";
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/HuafuSlicer/Main.qml")));
    }
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "[HuafuSlicer] QML load failed; check HuafuSlicer.log for [QML] warnings";
        return -1;
    }

    return app.exec();
}
    //if (qEnvironmentVariableIntValue("HUAFSLICER_SELFTEST_MESH_SAVE") != 0) {
    //    QTimer::singleShot(1500, &app, [&engine]() {
    //        const QList<QObject*> roots = engine.rootObjects();
    //        if (roots.isEmpty()) {
    //            qWarning() << "[MeshSave] SELFTEST no root QML object";
    //            return;
    //        }
    //        const bool ok = QMetaObject::invokeMethod(roots.first(), "selfTestOpenNativeSaveDialog", Qt::QueuedConnection);
    //        if (!ok)
    //            qWarning() << "[MeshSave] SELFTEST invokeMethod selfTestOpenNativeSaveDialog failed";
    //        });
    //}

    //return app.exec();
//}
