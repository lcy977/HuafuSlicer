#include "MeshToScene.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>

namespace {

void appendInterleavedFromTriangleMesh(const Slic3r::TriangleMesh& mesh, float scale, QVector<float>& out)
{
    const auto& its = mesh.its;
    const size_t                        nf  = its.indices.size();
    out.reserve(out.size() + int(nf * 3 * 6));

    for (size_t fi = 0; fi < nf; ++fi) {
        const Slic3r::Vec3i32& tri = its.indices[fi];
        Slic3r::Vec3f            p0 = its.vertices[tri(0)].cast<float>() * scale;
        Slic3r::Vec3f            p1 = its.vertices[tri(1)].cast<float>() * scale;
        Slic3r::Vec3f            p2 = its.vertices[tri(2)].cast<float>() * scale;

        Slic3r::Vec3f e1   = p1 - p0;
        Slic3r::Vec3f e2   = p2 - p0;
        Slic3r::Vec3f n    = e1.cross(e2);
        const float   len2 = n.squaredNorm();
        if (len2 > 1e-20f)
            n /= std::sqrt(len2);
        else
            n = Slic3r::Vec3f(0, 0, 1);

        auto pushV = [&](const Slic3r::Vec3f& p) {
            out.push_back(p.x());
            out.push_back(p.y());
            out.push_back(p.z());
            out.push_back(n.x());
            out.push_back(n.y());
            out.push_back(n.z());
        };
        pushV(p0);
        pushV(p1);
        pushV(p2);
    }
}

} // namespace

bool buildViewportMeshData(
    const Slic3r::Model&                model,
    std::shared_ptr<QVector<float>>&   outInterleaved,
    std::vector<ViewportMeshChunk>&     outChunks,
    QString*                            outError)
{
    auto buf = std::make_shared<QVector<float>>();
    buf->reserve(4096);
    outChunks.clear();

    constexpr float mm_to_m = 0.001f;

    try {
        int runningVerts = 0;

        for (Slic3r::ModelObject* obj : model.objects) {
            if (!obj)
                continue;
            Slic3r::TriangleMesh tm = obj->mesh();
            if (tm.empty())
                continue;

            const int triVerts = int(tm.facets_count() * 3);
            if (triVerts <= 0)
                continue;

            const int first = runningVerts;
            appendInterleavedFromTriangleMesh(tm, mm_to_m, *buf);
            runningVerts += triVerts;

            ViewportMeshChunk ch;
            ch.name = QString::fromStdString(obj->name.empty() ? "object" : obj->name);
            ch.filePath = QString::fromStdString(obj->input_file);
            if (ch.filePath.isEmpty())
                ch.filePath = ch.name;
            ch.firstVertex = first;
            ch.vertexCount = triVerts;
            outChunks.push_back(std::move(ch));
        }

        if (buf->isEmpty() || (buf->size() % 6) != 0) {
            if (outError)
                *outError = QStringLiteral("无可显示的三角面或顶点数据无效");
            return false;
        }

        outInterleaved = std::move(buf);
        return true;
    } catch (const std::exception& e) {
        if (outError)
            *outError = QString::fromUtf8(e.what());
        return false;
    } catch (...) {
        if (outError)
            *outError = QStringLiteral("网格转换失败（未知错误）");
        return false;
    }
}
