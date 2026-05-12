#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

class WindowHelper : public QObject
{
    Q_OBJECT

public:
    explicit WindowHelper(QObject *parent = nullptr);

    /// 使用系统拖动（推荐），避免无边框窗口反复 setPosition 导致的闪烁 / 抖动。
    Q_INVOKABLE void startSystemMove(QObject *windowObject) const;
    /** 供 QML FileDialog.selectedFile 等使用，生成本地文件 URL */
    Q_INVOKABLE QUrl urlFromLocalFile(const QString &localPath) const;
    /** 用户文档目录（避免 QML import QtCore 在部署环境缺失） */
    Q_INVOKABLE QString standardDocumentsPath() const;
    /** 写入 Qt 日志（经 qInstallMessageHandler 进入 HuafuSlicer.log）；QML console.log 默认不进该文件 */
    Q_INVOKABLE void writeDebugLog(const QString &message) const;

    /**
     * 阻塞式原生「另存为」对话框（QFileDialog）。无边框 QML FileDialog 在 Windows 上常不弹出，故走此路径。
     * 返回 QVariantMap：accepted(bool)、filePath(string)、format(int 0 二进制 STL /1 ASCII STL /2 OBJ)，取消则 accepted=false。
     */
    Q_INVOKABLE QVariantMap openMeshSaveNativeDialog(QObject *windowObject, const QString &title,
                                                     const QString &initialFullPath) const;
};
