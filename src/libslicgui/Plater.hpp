#pragma once

#include "Slic3rDocument.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <vector>

#include "libslic3r/Format/bbs_3mf.hpp" // LoadStrategy

class QThread;

/**
 * Qt 侧 Plater：负责与 libslic3r 对接的
 *  - 模型导入（stl/obj/3mf 等，走 Model::read_from_file）
 *  - 文档存储（Slic3r::Slic3rDocument：Model + DynamicPrintConfig）
 *  - 切片与 G-code 导出（Slic3r::Print::apply / process / export_gcode）
 *
 * 不依赖 wxWidgets；可在 QML 或 Qt Widgets 中持有单例或业务层对象使用。
 */
class Plater : public QObject
{
    Q_OBJECT

public:
    explicit Plater(QObject* parent = nullptr);
    ~Plater() override;

    /// 从单文件加载；成功则替换当前 document 中的 model；3mf 可能同时更新 print_config。
    bool loadFile(const QString& filePath);

    /// 顺序加载多个文件；返回成功加载的文件数。
    int loadFiles(const QStringList& filePaths, Slic3r::LoadStrategy strategy = Slic3r::LoadStrategy::LoadModel);

    void clear();

    Slic3r::Model& model();
    const Slic3r::Model& model() const;

    Slic3r::DynamicPrintConfig& printConfig();
    const Slic3r::DynamicPrintConfig& printConfig() const;

    Slic3r::Slic3rDocument& document();
    const Slic3r::Slic3rDocument& document() const;

    void resetPrintConfigToFactoryDefaults();

    /// 同步：切片并写出 G-code。outputPathTemplate 与 Slic3r::Print::export_gcode 规则一致（可为带占位符的模板路径）。
    bool sliceToGcode(
        const QString& outputPathTemplate,
        QString*      outGeneratedPath = nullptr,
        QString*      outError         = nullptr);

    /// 在后台线程中执行 sliceToGcode；通过 signal 返回结果。若已有任务在进行会失败。
    bool requestSliceAsync(const QString& outputPathTemplate, QString* outError = nullptr);

    bool isSlicing() const { return m_slicing.load(std::memory_order_acquire); }

signals:
    void modelChanged();
    void importFinished(const QString& path, int objectCount);
    void importFailed(const QString& path, const QString& error);
    void sliceProgress(int percent, const QString& message);
    void sliceFinished(const QString& gcodePath);
    void sliceFailed(const QString& error);

private:
    struct Impl;
    std::unique_ptr<Impl>   m_impl;
    std::atomic<bool>       m_slicing{false};
};
