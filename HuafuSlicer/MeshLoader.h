#pragma once

#include <QString>
#include <QVector>

/** OpenGL 热床正方形边长（世界坐标 1 单位 = 1 m） */
inline constexpr float kBedPlaneExtentMeters = 1.0f;

/**
 * STL（二进制/ASCII）与 Wavefront OBJ 三角网格加载，输出交错顶点：px,py,pz,nx,ny,nz（每顶点 6 个 float）。
 * 顶点保持文件内几何关系，仅按启发式做 mm→m 等统一单位换算；不做居中或强制摆放到热床中心。
 * OBJ：Y-up 转为场景 Z-up（XY 热床、Z 竖直）。
 */
class MeshLoader {
public:
    static bool loadTriangleMesh(const QString &path, QVector<float> &outInterleaved, QString *errorMessage = nullptr);

private:
    static bool loadStl(const QString &path, QVector<float> &out, QString *errorMessage);
    static bool loadObj(const QString &path, QVector<float> &out, QString *errorMessage);
};
