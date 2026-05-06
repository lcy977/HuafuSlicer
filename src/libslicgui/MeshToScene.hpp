#pragma once

#include <memory>
#include <vector>

#include <QVector>
#include <QString>

namespace Slic3r {
class Model;
}

struct ViewportMeshChunk
{
    QString  name;
    QString  filePath;
    int      firstVertex{0};
    int      vertexCount{0};
};

/**
 * 将 Slic3r::Model 转为与 OpenGLViewport 一致的交错顶点缓冲（每顶点 6 float：pos+normal，单位与 MeshLoader 一致为米）。
 */
bool buildViewportMeshData(
    const Slic3r::Model&                model,
    std::shared_ptr<QVector<float>>&   outInterleaved,
    std::vector<ViewportMeshChunk>&     outChunks,
    QString*                            outError = nullptr);
