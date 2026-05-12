#include "WindowHelper.h"

#include <QDebug>
#include <QFileDialog>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUrl>

WindowHelper::WindowHelper(QObject *parent)
    : QObject(parent)
{
}

void WindowHelper::startSystemMove(QObject *windowObject) const
{
    if (auto *w = qobject_cast<QQuickWindow *>(windowObject))
        w->startSystemMove();
}

QUrl WindowHelper::urlFromLocalFile(const QString &localPath) const
{
    return QUrl::fromLocalFile(localPath);
}

QString WindowHelper::standardDocumentsPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

void WindowHelper::writeDebugLog(const QString &message) const
{
    qInfo() << "[MeshSave]" << message;
}

QVariantMap WindowHelper::openMeshSaveNativeDialog(QObject *windowObject, const QString &title,
                                                   const QString &initialFullPath) const
{
    QVariantMap out;
    out.insert(QStringLiteral("accepted"), false);
    out.insert(QStringLiteral("filePath"), QString());
    out.insert(QStringLiteral("format"), -1);

    if (auto *qw = qobject_cast<QQuickWindow *>(windowObject)) {
        qw->raise();
        qw->requestActivate();
    }

    QString selectedFilter;
    const QString filters = QStringLiteral("STL \u4e8c\u8fdb\u5236 (*.stl);;STL \u6587\u672c (*.stl);;Wavefront OBJ (*.obj);;\u6240\u6709\u6587\u4ef6 (*.*)");
    const QString path = QFileDialog::getSaveFileName(nullptr, title, initialFullPath, filters, &selectedFilter);
    if (path.isEmpty()) {
        qInfo() << "[MeshSave] native QFileDialog cancelled";
        return out;
    }

    int fmt = 0;
    if (selectedFilter.contains(QStringLiteral("OBJ"), Qt::CaseInsensitive))
        fmt = 2;
    else if (selectedFilter.contains(QStringLiteral("\u6587\u672c"))
             || selectedFilter.contains(QStringLiteral("ASCII"), Qt::CaseInsensitive)
             || selectedFilter.contains(QStringLiteral("TEXT"), Qt::CaseInsensitive))
        fmt = 1;
    else if (selectedFilter.contains(QStringLiteral("\u4e8c\u8fdb\u5236"))
             || selectedFilter.contains(QStringLiteral("BINARY"), Qt::CaseInsensitive))
        fmt = 0;

    if (path.endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive))
        fmt = 2;
    else if (path.endsWith(QStringLiteral(".stl"), Qt::CaseInsensitive) && fmt != 1)
        fmt = 0;

    out.insert(QStringLiteral("accepted"), true);
    out.insert(QStringLiteral("filePath"), path);
    out.insert(QStringLiteral("format"), fmt);
    qInfo() << "[MeshSave] native QFileDialog accepted path=" << path << "format=" << fmt << "filter=" << selectedFilter;
    return out;
}
