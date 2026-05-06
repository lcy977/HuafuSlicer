#include "Plater.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Platform.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"

#include <QDebug>

#include <exception>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::string qPathToUtf8(const QString& s)
{
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), size_t(b.size()));
}

Slic3r::Model load_model_from_path(
    const std::string&                                  pathUtf8,
    Slic3r::DynamicPrintConfig*                         printConfig,
    Slic3r::ConfigSubstitutionContext&                  subst,
    bool                                                mergeOnlyGeometry)
{
    using namespace Slic3r;
    LoadStrategy strategy = mergeOnlyGeometry
        ? (LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances)
        : (LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances);

    DynamicPrintConfig* cfgPtr = mergeOnlyGeometry ? nullptr : printConfig;
    return Model::read_from_file(
        pathUtf8,
        cfgPtr,
        &subst,
        strategy,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        -1,
        nullptr);
}

void merge_models(Slic3r::Model& dst, Slic3r::Model& src)
{
    using namespace Slic3r;
    // add_object 会克隆对象；src 析构时释放其原有实例
    const ModelObjectPtrs copy = src.objects;
    for (const ModelObject* o : copy) {
        if (o)
            dst.add_object(*o);
    }
}

} // namespace

struct Plater::Impl
{
    Slic3r::Slic3rDocument              document;
    std::once_flag                      platform_once;

    void ensure_platform()
    {
        std::call_once(platform_once, []() { Slic3r::detect_platform(); });
    }
};

Plater::Plater(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    m_impl->ensure_platform();
}

Plater::~Plater() = default;

Slic3r::Model& Plater::model() { return m_impl->document.model; }

const Slic3r::Model& Plater::model() const { return m_impl->document.model; }

Slic3r::DynamicPrintConfig& Plater::printConfig() { return m_impl->document.print_config; }

const Slic3r::DynamicPrintConfig& Plater::printConfig() const { return m_impl->document.print_config; }

Slic3r::Slic3rDocument& Plater::document() { return m_impl->document; }

const Slic3r::Slic3rDocument& Plater::document() const { return m_impl->document; }

void Plater::resetPrintConfigToFactoryDefaults()
{
    m_impl->document.print_config = Slic3r::DynamicPrintConfig::full_print_config();
}

void Plater::clear()
{
    m_impl->document.clear();
    emit modelChanged();
}

bool Plater::loadFile(const QString& filePath)
{
    m_impl->ensure_platform();
    Slic3r::ConfigSubstitutionContext subst(Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
    try {
        const std::string u8 = qPathToUtf8(filePath);
        Slic3r::Model     m  = load_model_from_path(u8, &m_impl->document.print_config, subst, /*mergeOnlyGeometry=*/false);
        m_impl->document.model = std::move(m);
        const int nobj = int(m_impl->document.model.objects.size());
        emit modelChanged();
        emit importFinished(filePath, nobj);
        return true;
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        emit importFailed(filePath, err);
        return false;
    } catch (...) {
        emit importFailed(filePath, QStringLiteral("Unknown error while loading file."));
        return false;
    }
}

int Plater::loadFiles(const QStringList& filePaths, Slic3r::LoadStrategy strategy)
{
    Q_UNUSED(strategy);
    m_impl->ensure_platform();
    int okCount = 0;
    bool first  = true;

    for (const QString& fp : filePaths) {
        Slic3r::ConfigSubstitutionContext subst(Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        try {
            const std::string u8 = qPathToUtf8(fp);
            Slic3r::Model     m  = load_model_from_path(
                u8,
                &m_impl->document.print_config,
                subst,
                /*mergeOnlyGeometry=*/!first);

            if (first) {
                m_impl->document.model = std::move(m);
                first = false;
            } else {
                merge_models(m_impl->document.model, m);
            }
            ++okCount;
            emit importFinished(fp, int(m_impl->document.model.objects.size()));
        } catch (const std::exception& e) {
            emit importFailed(fp, QString::fromUtf8(e.what()));
        } catch (...) {
            emit importFailed(fp, QStringLiteral("Unknown error while loading file."));
        }
    }

    if (okCount > 0)
        emit modelChanged();
    return okCount;
}

bool Plater::sliceToGcode(const QString& outputPathTemplate, QString* outGeneratedPath, QString* outError)
{
    m_impl->ensure_platform();

    Slic3r::Print print;
    print.set_status_callback([this](const Slic3r::PrintBase::SlicingStatus& st) {
        emit sliceProgress(st.percent, QString::fromUtf8(st.text.c_str()));
    });

    try {
        Slic3r::DynamicPrintConfig cfg = m_impl->document.print_config;
        print.apply(m_impl->document.model, std::move(cfg));

        Slic3r::StringObjectException verr = print.validate();
        if (!verr.string.empty()) {
            const QString msg = QString::fromUtf8(verr.string.c_str());
            if (outError)
                *outError = msg;
            emit sliceFailed(msg);
            return false;
        }

        print.process();

        Slic3r::GCodeProcessorResult gresult;
        const std::string            pathTpl = qPathToUtf8(outputPathTemplate);
        const std::string            outPath = print.export_gcode(pathTpl, &gresult);
        if (outPath.empty()) {
            const QString msg = QStringLiteral("export_gcode returned empty path.");
            if (outError)
                *outError = msg;
            emit sliceFailed(msg);
            return false;
        }

        const QString qout = QString::fromUtf8(outPath.c_str());
        if (outGeneratedPath)
            *outGeneratedPath = qout;
        emit sliceFinished(qout);
        return true;
    } catch (const std::exception& e) {
        const QString msg = QString::fromUtf8(e.what());
        if (outError)
            *outError = msg;
        emit sliceFailed(msg);
        return false;
    } catch (...) {
        const QString msg = QStringLiteral("Slicing failed with unknown error.");
        if (outError)
            *outError = msg;
        emit sliceFailed(msg);
        return false;
    }
}

bool Plater::requestSliceAsync(const QString& outputPathTemplate, QString* outError)
{
    if (m_slicing.exchange(true, std::memory_order_acq_rel)) {
        const QString msg = QStringLiteral("Another slicing task is running.");
        if (outError)
            *outError = msg;
        emit sliceFailed(msg);
        return false;
    }

    m_impl->ensure_platform();

    // 在线程内使用副本，避免与 UI 线程同时改 document
    Slic3r::Model                 modelCopy = m_impl->document.model;
    Slic3r::DynamicPrintConfig    cfgCopy   = m_impl->document.print_config;
    const std::string             pathTpl   = qPathToUtf8(outputPathTemplate);

    std::thread([this, model = std::move(modelCopy), cfg = std::move(cfgCopy), pathTpl]() mutable {
        try {
            Slic3r::Print print;
            print.set_status_callback([this](const Slic3r::PrintBase::SlicingStatus& st) {
                emit sliceProgress(st.percent, QString::fromUtf8(st.text.c_str()));
            });

            print.apply(model, std::move(cfg));
            Slic3r::StringObjectException verr = print.validate();
            if (!verr.string.empty()) {
                emit sliceFailed(QString::fromUtf8(verr.string.c_str()));
                m_slicing.store(false, std::memory_order_release);
                return;
            }

            print.process();
            Slic3r::GCodeProcessorResult gresult;
            const std::string            outPath = print.export_gcode(pathTpl, &gresult);
            if (outPath.empty())
                emit sliceFailed(QStringLiteral("export_gcode returned empty path."));
            else
                emit sliceFinished(QString::fromUtf8(outPath.c_str()));
        } catch (const std::exception& e) {
            emit sliceFailed(QString::fromUtf8(e.what()));
        } catch (...) {
            emit sliceFailed(QStringLiteral("Slicing failed with unknown error."));
        }
        m_slicing.store(false, std::memory_order_release);
    }).detach();

    return true;
}
