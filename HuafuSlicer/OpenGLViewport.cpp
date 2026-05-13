#include "OpenGLViewport.h"
#include "MeshLoader.h"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "admesh/stl.h"

#include <QtQuick/QQuickWindow>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#include <QtGui/QMatrix4x4>
#include <QtGui/QVector4D>
#include <QtGui/QMouseEvent>
#include <QtGui/QVector3D>
#include <QtGui/QCursor>
#include <QtGui/QWheelEvent>
#include <QtGui/QHoverEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <QtOpenGL/QOpenGLFramebufferObject>
#include <QtOpenGL/QOpenGLPaintDevice>
#include <QtOpenGL/QOpenGLShaderProgram>
#include <QtOpenGL/QOpenGLTexture>
#include <QMetaObject>
#include <QSharedPointer>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QFontMetricsF>
#include <QElapsedTimer>
#include <QPointer>

#if defined(__has_include)
#  if __has_include(<ft2build.h>)
#    include <ft2build.h>
#    include FT_FREETYPE_H
#    define HAVE_FREETYPE 1
#  else
#    define HAVE_FREETYPE 0
#  endif
#else
#  define HAVE_FREETYPE 0
#endif
#include <QFutureWatcher>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <QUrl>
#include <QColor>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDataStream>
#include <QTextStream>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QStringConverter>
#include <cstring>
#include <QtEndian>
#include <QTimer>
#include <QVariantMap>
#include <memory>
#include <tuple>
#include <array>
#include <limits>
#include <algorithm>
#include <qopenglbuffer.h>
#include <qopenglvertexarrayobject.h>

// 可打印热床：XY ∈ [0, kBedPlaneExtentMeters]，Z 向上；世界原点 (0,0,0) 为可打印区左下角
static constexpr float kBedMargin = 0.04f; // 底板外扩
static constexpr float kBedVisMin = -kBedMargin;
static constexpr float kBedVisMax = kBedPlaneExtentMeters + kBedMargin;
static constexpr float kBedCenterXY = kBedPlaneExtentMeters * 0.5f;

namespace {
constexpr int kGizmoMargin = 10;
constexpr int kGizmoSize = 130;

static void appendChunkMirrorTransform(QMatrix4x4 &chunkModel, const QVector3D &geomCenterLocal,
                                       const QVector3D &mirrorScale)
{
    if (qFuzzyCompare(mirrorScale.x(), 1.0f) && qFuzzyCompare(mirrorScale.y(), 1.0f)
        && qFuzzyCompare(mirrorScale.z(), 1.0f))
        return;
    chunkModel.translate(geomCenterLocal);
    chunkModel.scale(mirrorScale);
    chunkModel.translate(-geomCenterLocal);
}

static QMatrix4x4 viewCubeProj(int gw, int gh)
{
    QMatrix4x4 P;
    const float aspect = gh > 0 ? float(gw) / float(gh) : 1.0f;
    P.perspective(45.0f, aspect, 0.05f, 100.0f);
    return P;
}

static QVector<QQuaternion> computeAxisAlignedOrientations()
{
    QVector<QQuaternion> list;
    const float degs[4] = {0.0f, 90.0f, 180.0f, 270.0f};
    for (int ix = 0; ix < 4; ++ix) {
        for (int iy = 0; iy < 4; ++iy) {
            for (int iz = 0; iz < 4; ++iz) {
                QQuaternion qx = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), degs[ix]);
                QQuaternion qy = QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), degs[iy]);
                QQuaternion qz = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), degs[iz]);
                QQuaternion q = qx * qy * qz;
                // deduplicate (q and -q represent same rotation)
                bool dup = false;
                for (const QQuaternion &exist : list) {
                    const float dot = q.scalar() * exist.scalar() + q.x() * exist.x() + q.y() * exist.y() + q.z() * exist.z();
                    if (qAbs(dot) > 0.999f) { dup = true; break; }
                }
                if (!dup)
                    list.append(q.normalized());
            }
        }
    }
    return list;
}

/** 角度（度）按最短弧插值，用于 yaw 动画 */
static float lerpAngleDeg(float a0, float a1, float t)
{
    float d = a1 - a0;
    while (d > 180.0f)
        d -= 360.0f;
    while (d < -180.0f)
        d += 360.0f;
    return a0 + d * t;
}

/** forward = normalize(target - eye)，与 preferred 尽量共面；近平行时换辅助轴 */
static QVector3D stableLookAtUp(const QVector3D &forwardFromEyeToTarget, const QVector3D &preferredWorldUp)
{
    QVector3D f = forwardFromEyeToTarget;
    if (f.lengthSquared() < 1e-20f)
        f = QVector3D(0.0f, 0.0f, -1.0f);
    else
        f.normalize();

    QVector3D u = preferredWorldUp;
    if (u.lengthSquared() < 1e-20f)
        u = QVector3D(0.0f, 0.0f, 1.0f);
    else
        u.normalize();

    if (qAbs(QVector3D::dotProduct(f, u)) > 0.96f) {
        u = QVector3D(0.0f, 1.0f, 0.0f);
        if (qAbs(QVector3D::dotProduct(f, u)) > 0.96f)
            u = QVector3D(1.0f, 0.0f, 0.0f);
    }
    return u;
}

/** 去掉沿 unitNormal 的分量，使平移留在热床切平面内（床面旋转后避免法向分量导致屏上方向「反了」） */
static QVector3D rejectAlongUnitNormal(const QVector3D &v, const QVector3D &unitNormal)
{
    if (unitNormal.lengthSquared() < 1e-20f)
        return v;
    const QVector3D n = unitNormal.normalized();
    return v - n * QVector3D::dotProduct(v, n);
}

// 与主场景轨道相机一致：世界 Z 向上；视线方向由 yaw/pitch 决定（与主视图 offset 同向），
// 使左下角立方体上的 XYZ 与热床/原点坐标轴在屏幕上的投影平行。
static QMatrix4x4 viewCubeView(float yawDeg, float pitchDeg, const QVector3D &preferredLookAtUpWorld)
{
    const float yaw = qDegreesToRadians(yawDeg);
    const float pitch = qDegreesToRadians(pitchDeg);
    const float cp = qCos(pitch);
    const float sp = qSin(pitch);
    QVector3D dir(qSin(yaw) * cp, qCos(yaw) * cp, sp);
    if (dir.lengthSquared() < 1e-12f)
        dir = QVector3D(0.0f, -1.0f, 0.0f);
    else
        dir.normalize();

    QMatrix4x4 V;
    const QVector3D eye = dir * 2.85f;
    const QVector3D forward = QVector3D(0.0f, 0.0f, 0.0f) - eye;
    const QVector3D up = stableLookAtUp(forward, preferredLookAtUpWorld);
    V.lookAt(eye, QVector3D(0.0f, 0.0f, 0.0f), up);
    return V;
}

static QMatrix4x4 viewCubeModel(const QQuaternion &planeOrientation)
{
    QMatrix4x4 M;
    M.rotate(planeOrientation);
    return M;
}

static float vcomp(const QVector3D &v, int i)
{
    return i == 0 ? v.x() : (i == 1 ? v.y() : v.z());
}

static bool rayUnitBoxFace(const QVector3D &ro, const QVector3D &rd, QVector3D &hit, int &faceOut)
{
    const float lo = -0.5f;
    const float hi = 0.5f;
    float t0 = 0.0f;
    float t1 = 1.0e30f;
    for (int a = 0; a < 3; ++a) {
        const float o = vcomp(ro, a);
        const float d = vcomp(rd, a);
        if (qFuzzyIsNull(d)) {
            if (o < lo || o > hi)
                return false;
            continue;
        }
        const float invD = 1.0f / d;
        float tNear = (lo - o) * invD;
        float tFar = (hi - o) * invD;
        if (tNear > tFar)
            qSwap(tNear, tFar);
        t0 = qMax(t0, tNear);
        t1 = qMin(t1, tFar);
        if (t0 > t1)
            return false;
    }
    float tHit = t0;
    if (tHit < 0.0f) {
        tHit = t1;
        if (tHit < 0.0f)
            return false;
    }
    hit = ro + rd * tHit;
    const float eps = 0.04f;
    // 与几何 / 纹理一致：0 +Z 上 1 -Z 下 2 -X 左 3 +X 右 4 +Y 后 5 -Y 前
    if (hit.z() >= hi - eps)
        faceOut = 0;
    else if (hit.z() <= lo + eps)
        faceOut = 1;
    else if (hit.x() <= lo + eps)
        faceOut = 2;
    else if (hit.x() >= hi - eps)
        faceOut = 3;
    else if (hit.y() >= hi - eps)
        faceOut = 4;
    else if (hit.y() <= lo + eps)
        faceOut = 5;
    else
        return false;
    return true;
}

static int pickViewCubeFaceImpl(const QPointF &itemPos, qreal itemW, qreal itemH, int fbW, int fbH,
                                const QQuaternion &planeOrientation, float yawDeg, float pitchDeg,
                                const QVector3D &lookAtUpWorld)
{
    if (fbW < 1 || fbH < 1 || itemW <= 0.0 || itemH <= 0.0)
        return -1;

    const double mx = itemPos.x() * double(fbW) / double(itemW);
    const double myTop = itemPos.y() * double(fbH) / double(itemH);
    const int myGl = fbH - 1 - int(qBound(0, int(qRound(myTop)), fbH - 1));

    const int x0 = kGizmoMargin;
    // 与 glViewport 一致：FBO 经 QQuickFramebufferObject::mirrorVertically(true) 显示，y 需从「底」起算
    const int y0 = kGizmoMargin;
    const int gw = kGizmoSize;
    const int gh = kGizmoSize;

    if (mx < double(x0) || mx >= double(x0 + gw) || myGl < y0 || myGl >= y0 + gh)
        return -1;

    const float px = float(mx - double(x0));
    const float py = float(myGl - y0);
    const float ndcX = (px + 0.5f) / float(gw) * 2.0f - 1.0f;
    const float ndcY = (py + 0.5f) / float(gh) * 2.0f - 1.0f;

    const QMatrix4x4 P = viewCubeProj(gw, gh);
    const QMatrix4x4 V = viewCubeView(yawDeg, pitchDeg, lookAtUpWorld);
    const QMatrix4x4 M = viewCubeModel(planeOrientation);
    const QMatrix4x4 inv = (P * V * M).inverted();

    auto unproject = [&](float ndcZ, QVector3D &out) {
        QVector4D clip(ndcX, ndcY, ndcZ, 1.0f);
        QVector4D wv = inv * clip;
        if (qFuzzyIsNull(wv.w()))
            return false;
        out = QVector3D(wv.x() / wv.w(), wv.y() / wv.w(), wv.z() / wv.w());
        return true;
    };

    QVector3D wNear, wFar;
    if (!unproject(-1.0f, wNear) || !unproject(1.0f, wFar))
        return -1;
    const QVector3D rd = (wFar - wNear).normalized();
    if (rd.lengthSquared() < 1e-10f)
        return -1;

    QVector3D hit;
    int face = -1;
    if (!rayUnitBoxFace(wNear, rd, hit, face))
        return -1;
    return face;
}

struct TrajPlaybackSample {
    int fullSegCount = 0;
    int partialIndex = -1;
    float partialAlpha = 0.f;
};

static TrajPlaybackSample sampleTrajectoryAtProgress(const QVector<GCodeParser::Segment> &segs,
                                                     qreal progress,
                                                     const QVector<double> &cumTimeSec)
{
    TrajPlaybackSample s;
    const int n = segs.size();
    if (n <= 0)
        return s;
    if (cumTimeSec.size() != n + 1 || cumTimeSec.constLast() <= 1e-12) {
        s.fullSegCount = int(qFloor(qBound(0.0, double(progress), 1.0) * qreal(n) + 1e-9));
        s.fullSegCount = qBound(0, s.fullSegCount, n);
        if (s.fullSegCount < n) {
            s.partialIndex = s.fullSegCount;
            const double u = qBound(0.0, double(progress), 1.0) * qreal(n) - qreal(s.fullSegCount);
            s.partialAlpha = float(qBound(0.0, u, 1.0));
        }
        return s;
    }
    const double total = cumTimeSec.constLast();
    const double T = qBound(0.0, double(progress), 1.0) * total;
    int k = 0;
    while (k < n && cumTimeSec[k + 1] <= T + 1e-15)
        ++k;
    s.fullSegCount = k;
    if (k < n) {
        s.partialIndex = k;
        const double d0 = cumTimeSec[k];
        const double d1 = cumTimeSec[k + 1];
        const double segDur = qMax(1e-9, d1 - d0);
        s.partialAlpha = float(qBound(0.0, (T - d0) / segDur, 1.0));
    }
    return s;
}

static int trajectoryVisibleEnd(const QVector<GCodeParser::Segment> &segs,
                                qreal progress,
                                const QVector<double> &cumTimeSec)
{
    return sampleTrajectoryAtProgress(segs, progress, cumTimeSec).fullSegCount;
}

static void buildTrajectoryInterleaved(const QVector<GCodeParser::Segment> &segs,
                                       qreal progress,
                                       int displayLayer,
                                       const QVector<float> &layerZs,
                                       const QVector<double> &cumTimeSec,
                                       bool showTravelMoves,
                                       QVector<float> &out)
{
    out.clear();
    if (segs.isEmpty())
        return;

    const int n = segs.size();
    const TrajPlaybackSample sm = sampleTrajectoryAtProgress(segs, progress, cumTimeSec);
    const int visEnd = qBound(0, sm.fullSegCount, n);

    float zCut = 1.0e6f;
    if (!layerZs.isEmpty()) {
        const int li = qBound(0, displayLayer, layerZs.size() - 1);
        zCut = layerZs[li] + 1e-4f;
    }

    out.reserve(size_t(visEnd + 2) * 12);
    for (int i = 0; i < visEnd; ++i) {
        const GCodeParser::Segment &s = segs[i];
        if (!showTravelMoves && s.kind == GCodeParser::MoveKind::Travel)
            continue;
        const float mz = qMax(s.az, s.bz);
        if (mz > zCut)
            continue;
        const QColor c = GCodeParser::colorForKind(s.kind);
        const float r = c.redF(), g = c.greenF(), b = c.blueF();
        out << s.ax << s.ay << s.az << r << g << b;
        out << s.bx << s.by << s.bz << r << g << b;
    }

    if (sm.partialIndex >= 0 && sm.partialIndex < n && sm.partialAlpha > 1e-7f) {
        const GCodeParser::Segment &s = segs[sm.partialIndex];
        if (!showTravelMoves && s.kind == GCodeParser::MoveKind::Travel)
            return;
        const float mz = qMax(s.az, s.bz);
        if (mz > zCut)
            return;
        const float t = sm.partialAlpha;
        const float px = s.ax + (s.bx - s.ax) * t;
        const float py = s.ay + (s.by - s.ay) * t;
        const float pz = s.az + (s.bz - s.az) * t;
        const float dx = px - s.ax, dy = py - s.ay, dz = pz - s.az;
        if (dx * dx + dy * dy + dz * dz < 1e-16f)
            return;
        const QColor c = GCodeParser::colorForKind(s.kind);
        const float r = c.redF(), g = c.greenF(), b = c.blueF();
        out << s.ax << s.ay << s.az << r << g << b;
        out << px << py << pz << r << g << b;
    }
}

// 轨迹预览用：z=0 为刀尖（圆锥顶点），沿 +Z 为圆锥笔头 + 圆柱笔身；顶点色：锥尖红色、杆身白色
static void appendPreviewNozzleMesh(QVector<float> &out)
{
    constexpr float kRadius = 0.0045f; // 约 4.5 mm，锥底与柱身同径
    constexpr float kConeH = 0.016f;        // 16 mm 锥段
    constexpr float kBarrelH = 0.024f;      // 24 mm 柱段
    constexpr int kSeg = 20;
    const float twoPi = float(2.0 * M_PI);
    const float zJoin = kConeH;
    const float zTop = kConeH + kBarrelH;
    const QVector3D kTipRed(0.92f, 0.14f, 0.12f);
    const QVector3D kRodWhite(0.98f, 0.98f, 0.99f);

    auto pushV = [&out](float x, float y, float z, const QVector3D &nm, const QVector3D &col) {
        out << x << y << z << nm.x() << nm.y() << nm.z() << col.x() << col.y() << col.z();
    };
    auto pushTri = [&pushV](const QVector3D &a, const QVector3D &b, const QVector3D &c, const QVector3D &ca,
                            const QVector3D &cb, const QVector3D &cc) {
        const QVector3D n = QVector3D::crossProduct(b - a, c - a).normalized();
        pushV(a.x(), a.y(), a.z(), n, ca);
        pushV(b.x(), b.y(), b.z(), n, cb);
        pushV(c.x(), c.y(), c.z(), n, cc);
    };

    const QVector3D apex(0.f, 0.f, 0.f);
    out.reserve(out.size() + size_t(kSeg) * 36 * 4);

    for (int i = 0; i < kSeg; ++i) {
        const float a0 = twoPi * float(i) / float(kSeg);
        const float a1 = twoPi * float(i + 1) / float(kSeg);
        const float c0 = qCos(a0), s0 = qSin(a0);
        const float c1 = qCos(a1), s1 = qSin(a1);
        const QVector3D p0(kRadius * c0, kRadius * s0, zJoin);
        const QVector3D p1(kRadius * c1, kRadius * s1, zJoin);
        pushTri(apex, p1, p0, kTipRed, kTipRed, kTipRed);
    }

    for (int i = 0; i < kSeg; ++i) {
        const float a0 = twoPi * float(i) / float(kSeg);
        const float a1 = twoPi * float(i + 1) / float(kSeg);
        const float c0 = qCos(a0), s0 = qSin(a0);
        const float c1 = qCos(a1), s1 = qSin(a1);
        const QVector3D b0(kRadius * c0, kRadius * s0, zJoin);
        const QVector3D b1(kRadius * c1, kRadius * s1, zJoin);
        const QVector3D t0(kRadius * c0, kRadius * s0, zTop);
        const QVector3D t1(kRadius * c1, kRadius * s1, zTop);
        pushTri(b0, b1, t0, kRodWhite, kRodWhite, kRodWhite);
        pushTri(b1, t1, t0, kRodWhite, kRodWhite, kRodWhite);
    }

    const QVector3D ctop(0.f, 0.f, zTop);
    for (int i = 0; i < kSeg; ++i) {
        const float a0 = twoPi * float(i) / float(kSeg);
        const float a1 = twoPi * float(i + 1) / float(kSeg);
        const float c0 = qCos(a0), s0 = qSin(a0);
        const float c1 = qCos(a1), s1 = qSin(a1);
        const QVector3D r0(kRadius * c0, kRadius * s0, zTop);
        const QVector3D r1(kRadius * c1, kRadius * s1, zTop);
        pushTri(ctop, r0, r1, kRodWhite, kRodWhite, kRodWhite);
    }
}
} // namespace

void OpenGLViewport::snapPlaneOrientationToAxisAligned(bool animate)
{
    m_orbitViewSnapActive = false;

    const QVector<QQuaternion> candidates = computeAxisAlignedOrientations();
    if (candidates.isEmpty())
        return;
    const QQuaternion cur = m_planeOrientation.normalized();
    float bestDot = -1.0f;
    QQuaternion bestQ = cur;
    for (const QQuaternion &cand : candidates) {
        const float dot = qAbs(cur.scalar() * cand.scalar() + cur.x() * cand.x() + cur.y() * cand.y() + cur.z() * cand.z());
        if (dot > bestDot) {
            bestDot = dot;
            bestQ = cand;
        }
    }
    if (animate) {
        m_faceAnimStart = m_planeOrientation;
        m_faceAnimTarget = bestQ;
        m_faceAnimElapsed.restart();
        m_faceAnimActive = true;
        if (m_faceAnimTimer && !m_faceAnimTimer->isActive()) m_faceAnimTimer->start();
    } else {
        m_planeOrientation = bestQ;
        update();
        if (auto *w = window()) w->update();
    }
}

struct MeshDrawChunk {
    int firstVertex = 0;
    int vertexCount = 0;
    bool active = false;
    bool sceneVisible = true;
    QVector3D positionOffset = QVector3D(0, 0, 0);
    QQuaternion rotation = QQuaternion();
    QVector3D mirrorScale{1.0f, 1.0f, 1.0f};
    QVector3D geomCenterLocal{0.0f, 0.0f, 0.0f};
    float uniformScale = 1.0f;
};

class ViewportRenderer final : public QQuickFramebufferObject::Renderer
{
public:
    ViewportRenderer() {
        // 注意：构造函数未必在 OpenGL context 激活时运行，
        // 所以不要在这里编译 shader / 创建 VAO/VBO。
        qDebug() << "[ViewportRenderer] Constructed";
    }

    ~ViewportRenderer() {
        qDebug() << "[ViewportRenderer] Destructor called";
        if (vaoPlane.isCreated()) vaoPlane.destroy();
        if (vaoGrid.isCreated()) vaoGrid.destroy();
        if (vaoAxis.isCreated()) vaoAxis.destroy();
        if (vaoAxisLabel.isCreated()) vaoAxisLabel.destroy();
        if (vaoViewCube.isCreated()) vaoViewCube.destroy();
        if (vaoViewCubeAxis.isCreated()) vaoViewCubeAxis.destroy();
        if (vaoViewCubeEdges.isCreated()) vaoViewCubeEdges.destroy();
        if (vaoMesh.isCreated()) vaoMesh.destroy();
        if (vaoBg.isCreated()) vaoBg.destroy();
        if (vaoTraj.isCreated()) vaoTraj.destroy();
        if (vboPlane.isCreated()) vboPlane.destroy();
        if (vboGrid.isCreated()) vboGrid.destroy();
        if (vboAxis.isCreated()) vboAxis.destroy();
        if (vboAxisLabel.isCreated()) vboAxisLabel.destroy();
        if (vboViewCube.isCreated()) vboViewCube.destroy();
        if (vboViewCubeAxis.isCreated()) vboViewCubeAxis.destroy();
        if (vboViewCubeEdges.isCreated()) vboViewCubeEdges.destroy();
        if (vboMesh.isCreated()) vboMesh.destroy();
        if (vboBg.isCreated()) vboBg.destroy();
        if (vboTraj.isCreated()) vboTraj.destroy();
        if (vaoNozzle.isCreated()) vaoNozzle.destroy();
        if (vboNozzle.isCreated()) vboNozzle.destroy();
        delete bgTexture;
        bgTexture = nullptr;
        if (viewCubeAtlas) {
            delete viewCubeAtlas;
            viewCubeAtlas = nullptr;
        }
    }

    void ensureInitialized() {
        if (initialized)
            return;

        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            qWarning() << "[ViewportRenderer] ERROR: No OpenGL context in ensureInitialized()";
            return;
        }

        qDebug() << "=== [ViewportRenderer] Initializing in render() ===";
        qDebug() << "[ViewportRenderer] isOpenGLES:" << ctx->isOpenGLES();
        qDebug() << "[ViewportRenderer] surface format version:"
                 << ctx->format().majorVersion() << "." << ctx->format().minorVersion()
                 << "profile:" << ctx->format().profile();

        // 背景：全屏 procedural 星系云（上半淡信息云，下半素色渐变）
        const QByteArray bgVertexShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec2 aPos;
            out vec2 vUv;
            void main() {
                vUv = aPos * 0.5 + 0.5;
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec2 aPos;
            out vec2 vUv;
            void main() {
                vUv = aPos * 0.5 + 0.5;
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
        )");

        const QByteArray bgFragmentShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec2 vUv;
            out vec4 outColor;
            uniform sampler2D uBgTex;
            void main() {
                // QQuickFramebufferObject::mirrorVertically(true) → 采样做 y 翻转
                vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
                outColor = texture(uBgTex, uv);
            }
        )") : QByteArray(R"(
            #version 330 core
            in vec2 vUv;
            out vec4 outColor;
            uniform sampler2D uBgTex;
            void main() {
                vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
                outColor = texture(uBgTex, uv);
            }
        )");

        // 平面：金属底材 + 网格 + Blinn-Phong 高光（掠角更明显）
        const QByteArray planeVertexShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec3 position;
            uniform mat4 model;
            uniform mat4 mvp;
            out vec3 vPos;
            out vec3 vWorldPos;
            out vec3 vWorldNormal;
            void main() {
                vPos = position;
                vec4 wp = model * vec4(position, 1.0);
                vWorldPos = wp.xyz;
                vWorldNormal = normalize((model * vec4(0.0, 0.0, 1.0, 0.0)).xyz);
                gl_Position = mvp * vec4(position, 1.0);
                // 启用多边形偏移，避免z-fighting
                gl_Position.z -= 0.0001;
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 position;
            uniform mat4 model;
            uniform mat4 mvp;
            out vec3 vPos;
            out vec3 vWorldPos;
            out vec3 vWorldNormal;
            void main() {
                vPos = position;
                vec4 wp = model * vec4(position, 1.0);
                vWorldPos = wp.xyz;
                vWorldNormal = normalize((model * vec4(0.0, 0.0, 1.0, 0.0)).xyz);
                gl_Position = mvp * vec4(position, 1.0);
                // 启用多边形偏移，避免z-fighting
                gl_Position.z -= 0.0001;
            }
        )");

        const QByteArray planeFragmentShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec3 vPos;
            in vec3 vWorldPos;
            in vec3 vWorldNormal;
            out vec4 outColor;

            uniform vec3 uCameraPos;
            uniform vec3 uLightPos0;
            uniform vec3 uLightPos1;
            uniform vec3 uLightPos2;
            uniform vec3 uLightCol0;
            uniform vec3 uLightCol1;
            uniform vec3 uLightCol2;

            // 抗锯齿网格线：缩放/远看时避免“虚线/断续”
            float gridLineAA(float coord, float step) {
                float x = coord / step;
                float fx = abs(fract(x) - 0.5);
                // 线在格子边界处：fx -> 0.5；把它换成到边界的距离（0=在线上）
                float distToLine = 0.5 - fx;
                // fwidth 提供屏幕空间覆盖宽度，保证远处线条连续
                float w = max(fwidth(x) * 1.25, 0.001);
                return 1.0 - smoothstep(0.0, w, distToLine);
            }

            // 2D SDF：圆角矩形（中心坐标系）
            float sdRoundBox(vec2 p, vec2 b, float r) {
                vec2 q = abs(p) - b;
                return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
            }

            void main() {
                // 造型：圆角热床 + 四角安装孔
                const float bedMin = -0.04;
                const float bedMax = 1.04;
                const float extent = bedMax - bedMin;         // 平面边长（m）
                vec2 p = vec2(vPos.x, vPos.y) - vec2((bedMin + bedMax) * 0.5); // 以中心为原点
                float cornerR = 0.025;            // 圆角半径
                float halfInner = extent * 0.5 - 0.02; // 外边留 20mm
                float dPlate = sdRoundBox(p, vec2(halfInner - cornerR), cornerR);
                float aa = max(fwidth(dPlate) * 1.25, 0.001);
                float plateMask = 1.0 - smoothstep(0.0, aa, dPlate); // 1=板内，0=板外

                // 四角孔：放在网格正方形 [0,1] 外侧（位于扩展边缘区域）
                // 直径 < 15mm => 半径 < 7.5mm
                float holeR = 0.007;         // 14mm 直径
                float holeInset = 0.020;     // 距离外边缘 20mm
                vec2 h0 = vec2(bedMin + holeInset, bedMin + holeInset);
                vec2 h1 = vec2(bedMax - holeInset, bedMin + holeInset);
                vec2 h2 = vec2(bedMin + holeInset, bedMax - holeInset);
                vec2 h3 = vec2(bedMax - holeInset, bedMax - holeInset);
                vec2 vxy = vec2(vPos.x, vPos.y);
                float dH0 = length(vxy - h0) - holeR;
                float dH1 = length(vxy - h1) - holeR;
                float dH2 = length(vxy - h2) - holeR;
                float dH3 = length(vxy - h3) - holeR;
                float holeMask = smoothstep(0.0, max(fwidth(dH0) * 1.25, 0.001), min(min(dH0, dH1), min(dH2, dH3)));

                // 更接近“打印店刻度板”的中性灰底
                float n = sin(vPos.x * 0.12) * sin(vPos.y * 0.10);
                // 参考图：整体更亮的灰白刻度板
                vec3 base = vec3(0.70, 0.71, 0.72) + n * 0.006;

                // 需求：保留大网格，去掉小网格（仅 50 间距）
                // 同时：边界留白，不在外圈边缘区域绘制网格线（避免 x=0/y=0 等线在边界多出一截）
                float major = max(gridLineAA(vPos.x, 0.05), gridLineAA(vPos.y, 0.05));
                // 网格线仅绘制在 [0,1] 的正方形范围内，不允许进入负值或超出 1m
                const float gridMin = 0.0;
                const float gridMax = 1.0;
                float gridMask =
                    step(gridMin, vPos.x) * step(gridMin, vPos.y) *
                    step(vPos.x, gridMax) * step(vPos.y, gridMax);
                major *= gridMask;
                vec3 majorCol = vec3(1.00, 1.00, 1.00);
                // 网格线淡一点：降低混合强度
                vec3 albedo = mix(base, majorCol, major * 0.40);

                // 板边缘做一点深色描边
                float edge = 1.0 - smoothstep(0.0, aa * 6.0, abs(dPlate));
                albedo = mix(albedo, albedo * 0.90, edge * 0.35);

                vec3 N = normalize(vWorldNormal);
                vec3 V = normalize(uCameraPos - vWorldPos);
                float ndv = max(dot(N, V), 0.0);

                // 米制场景：距离约 0.3–1m，须用较大衰减系数，否则多盏灯叠加会过曝
                vec3 ambient = albedo * 0.22;
                vec3 diffuse = vec3(0.0);
                vec3 specCol = vec3(0.0);

                vec3 lightPos[3] = vec3[3](uLightPos0, uLightPos1, uLightPos2);
                vec3 lightCol[3] = vec3[3](uLightCol0, uLightCol1, uLightCol2);
                for (int i = 0; i < 3; ++i) {
                    vec3 L = lightPos[i] - vWorldPos;
                    float dist = max(length(L), 0.001);
                    L /= dist;
                    float att = 1.0 / (1.0 + 14.0 * dist * dist);
                    float ndl = max(dot(N, L), 0.0);
                    vec3 H = normalize(L + V);
                    float spec = pow(max(dot(N, H), 0.0), 128.0);
                    diffuse += albedo * (0.38 * ndl) * lightCol[i] * att;
                    specCol += vec3(1.0) * spec * 0.30 * lightCol[i] * att;
                }
                float fresnel = pow(1.0 - ndv, 4.0);
                vec3 rim = vec3(0.75, 0.82, 0.95) * fresnel * 0.09;

                vec3 lit = ambient + diffuse + specCol + rim;

                // 视角相关的双面效果：
                // - 从 +Z 方向看（正面）：高亮灰白、不透明
                // - 从下面看（背面）：黑色透明
                float facing = step(0.0, dot(N, V)); // 1=正面，0=背面
                vec3 frontCol = clamp(lit * 0.95 + vec3(0.05), 0.0, 1.0);

                // 背面：半透明黑；正面：不透明
                vec3 baseCol = mix(vec3(0.0), frontCol, facing);
                float baseAlpha = mix(0.35, 1.0, facing);

                // 网格线略提亮，避免再叠成纯白
                baseCol = mix(baseCol, vec3(1.0), major * 0.28);
                float outA = max(baseAlpha, major * 0.15);

                // 造型遮罩：板外透明；孔洞透明
                float shapeA = plateMask * holeMask;
                outColor = vec4(baseCol, outA * shapeA);
            }
        )") : QByteArray(R"(
            #version 330 core
            in vec3 vPos;
            in vec3 vWorldPos;
            in vec3 vWorldNormal;
            out vec4 outColor;

            uniform vec3 uCameraPos;
            uniform vec3 uLightPos0;
            uniform vec3 uLightPos1;
            uniform vec3 uLightPos2;
            uniform vec3 uLightCol0;
            uniform vec3 uLightCol1;
            uniform vec3 uLightCol2;

            // 抗锯齿网格线：缩放/远看时避免“虚线/断续”
            float gridLineAA(float coord, float step) {
                float x = coord / step;
                float fx = abs(fract(x) - 0.5);
                float distToLine = 0.5 - fx;
                float w = max(fwidth(x) * 1.25, 0.001);
                return 1.0 - smoothstep(0.0, w, distToLine);
            }

            // 2D SDF：圆角矩形（中心坐标系）
            float sdRoundBox(vec2 p, vec2 b, float r) {
                vec2 q = abs(p) - b;
                return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
            }

            void main() {
                // 造型：圆角热床 + 四角安装孔
                const float bedMin = -0.04;
                const float bedMax = 1.04;
                const float extent = bedMax - bedMin;         // 平面边长（m）
                vec2 p = vec2(vPos.x, vPos.y) - vec2((bedMin + bedMax) * 0.5); // 以中心为原点
                float cornerR = 0.025;            // 圆角半径
                float halfInner = extent * 0.5 - 0.02; // 外边留 20mm
                float dPlate = sdRoundBox(p, vec2(halfInner - cornerR), cornerR);
                float aa = max(fwidth(dPlate) * 1.25, 0.001);
                float plateMask = 1.0 - smoothstep(0.0, aa, dPlate); // 1=板内，0=板外

                // 四角孔：放在网格正方形 [0,1] 外侧（位于扩展边缘区域）
                // 直径 < 15mm => 半径 < 7.5mm
                float holeR = 0.007;         // 14mm 直径
                float holeInset = 0.020;     // 距离外边缘 20mm
                vec2 h0 = vec2(bedMin + holeInset, bedMin + holeInset);
                vec2 h1 = vec2(bedMax - holeInset, bedMin + holeInset);
                vec2 h2 = vec2(bedMin + holeInset, bedMax - holeInset);
                vec2 h3 = vec2(bedMax - holeInset, bedMax - holeInset);
                vec2 vxy = vec2(vPos.x, vPos.y);
                float dH0 = length(vxy - h0) - holeR;
                float dH1 = length(vxy - h1) - holeR;
                float dH2 = length(vxy - h2) - holeR;
                float dH3 = length(vxy - h3) - holeR;
                float holeMask = smoothstep(0.0, max(fwidth(dH0) * 1.25, 0.001), min(min(dH0, dH1), min(dH2, dH3)));

                // 更接近“打印店刻度板”的中性灰底
                float n = sin(vPos.x * 0.12) * sin(vPos.y * 0.10);
                // 参考图：整体更亮的灰白刻度板
                vec3 base = vec3(0.70, 0.71, 0.72) + n * 0.006;

                // 需求：保留大网格，去掉小网格（仅 50 间距）
                // 同时：边界留白，不在外圈边缘区域绘制网格线（避免 x=0/y=0 等线在边界多出一截）
                float major = max(gridLineAA(vPos.x, 0.05), gridLineAA(vPos.y, 0.05));
                // 网格线仅绘制在 [0,1] 的正方形范围内，不允许进入负值或超出 1m
                const float gridMin = 0.0;
                const float gridMax = 1.0;
                float gridMask =
                    step(gridMin, vPos.x) * step(gridMin, vPos.y) *
                    step(vPos.x, gridMax) * step(vPos.y, gridMax);
                major *= gridMask;
                vec3 majorCol = vec3(1.00, 1.00, 1.00);
                // 网格线淡一点：降低混合强度
                vec3 albedo = mix(base, majorCol, major * 0.40);

                // 板边缘做一点深色描边
                float edge = 1.0 - smoothstep(0.0, aa * 6.0, abs(dPlate));
                albedo = mix(albedo, albedo * 0.90, edge * 0.35);

                vec3 N = normalize(vWorldNormal);
                vec3 V = normalize(uCameraPos - vWorldPos);
                float ndv = max(dot(N, V), 0.0);

                // 米制场景：距离约 0.3–1m，须用较大衰减系数，否则多盏灯叠加会过曝
                vec3 ambient = albedo * 0.22;
                vec3 diffuse = vec3(0.0);
                vec3 specCol = vec3(0.0);

                vec3 lightPos[3] = vec3[3](uLightPos0, uLightPos1, uLightPos2);
                vec3 lightCol[3] = vec3[3](uLightCol0, uLightCol1, uLightCol2);
                for (int i = 0; i < 3; ++i) {
                    vec3 L = lightPos[i] - vWorldPos;
                    float dist = max(length(L), 0.001);
                    L /= dist;
                    float att = 1.0 / (1.0 + 14.0 * dist * dist);
                    float ndl = max(dot(N, L), 0.0);
                    vec3 H = normalize(L + V);
                    float spec = pow(max(dot(N, H), 0.0), 128.0);
                    diffuse += albedo * (0.38 * ndl) * lightCol[i] * att;
                    specCol += vec3(1.0) * spec * 0.30 * lightCol[i] * att;
                }
                float fresnel = pow(1.0 - ndv, 4.0);
                vec3 rim = vec3(0.75, 0.82, 0.95) * fresnel * 0.09;

                vec3 lit = ambient + diffuse + specCol + rim;

                // 视角相关的双面效果：
                // - 从 +Z 方向看（正面）：高亮灰白、不透明
                // - 从下面看（背面）：黑色透明
                float facing = step(0.0, dot(N, V)); // 1=正面，0=背面
                vec3 frontCol = clamp(lit * 0.95 + vec3(0.05), 0.0, 1.0);

                // 背面：半透明黑；正面：不透明
                vec3 baseCol = mix(vec3(0.0), frontCol, facing);
                float baseAlpha = mix(0.35, 1.0, facing);

                // 网格线略提亮，避免再叠成纯白
                baseCol = mix(baseCol, vec3(1.0), major * 0.28);
                float outA = max(baseAlpha, major * 0.15);

                // 造型遮罩：板外透明；孔洞透明
                float shapeA = plateMask * holeMask;
                outColor = vec4(baseCol, outA * shapeA);
            }
        )");

        // 导入网格：逐顶点法线 + Blinn-Phong
        const QByteArray meshVertexShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec3 position;
            layout(location = 1) in vec3 normal;
            layout(location = 2) in vec3 vertexColor;
            uniform mat4 model;
            uniform mat4 mvp;
            uniform mat3 uNormalMat;
            out vec3 vWorldPos;
            out vec3 vWorldNormal;
            out vec3 vVertexColor;
            void main() {
                vec4 wp = model * vec4(position, 1.0);
                vWorldPos = wp.xyz;
                vWorldNormal = normalize(uNormalMat * normal);
                vVertexColor = vertexColor;
                gl_Position = mvp * vec4(position, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 position;
            layout(location = 1) in vec3 normal;
            layout(location = 2) in vec3 vertexColor;
            uniform mat4 model;
            uniform mat4 mvp;
            uniform mat3 uNormalMat;
            out vec3 vWorldPos;
            out vec3 vWorldNormal;
            out vec3 vVertexColor;
            void main() {
                vec4 wp = model * vec4(position, 1.0);
                vWorldPos = wp.xyz;
                vWorldNormal = normalize(uNormalMat * normal);
                vVertexColor = vertexColor;
                gl_Position = mvp * vec4(position, 1.0);
            }
        )");

        const QByteArray meshFragmentShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec3 vWorldPos;
            in vec3 vWorldNormal;
            in vec3 vVertexColor;
            out vec4 outColor;
            uniform vec3 uCameraPos;
            uniform vec3 uLightPos0;
            uniform vec3 uLightPos1;
            uniform vec3 uLightPos2;
            uniform vec3 uLightCol0;
            uniform vec3 uLightCol1;
            uniform vec3 uLightCol2;
            uniform vec3 uMeshAlbedo;
            uniform float uDrawMode;
            uniform float uUseVertexColor;
            void main() {
                float nlen0 = length(vWorldNormal);
                vec3 N0 = (nlen0 > 1e-6) ? (vWorldNormal / nlen0) : vec3(0.0, 0.0, 1.0);
                vec3 Vdir0 = (uCameraPos - vWorldPos);
                float vlen0 = max(length(Vdir0), 1e-6);
                vec3 V0 = Vdir0 / vlen0;
                // 叠加：uDrawMode 1.0=选中红，2.0=激活绿（用 float 避免部分驱动 int uniform 异常）
                if (uDrawMode > 1.5) {
                    vec3 dqdx = dFdx(vWorldPos);
                    vec3 dqdy = dFdy(vWorldPos);
                    vec3 Nf = cross(dqdx, dqdy);
                    float nlenF = length(Nf);
                    vec3 Nface = (nlenF > 1e-6) ? (Nf / nlenF) : N0;
                    if (dot(Nface, V0) < 0.0)
                        Nface = -Nface;
                    float ndv = max(dot(Nface, V0), 0.0);
                    float edge = pow(1.0 - ndv, 1.55);
                    float rim = pow(1.0 - ndv, 3.2) * 0.55;
                    vec3 fluo = vec3(0.22, 1.0, 0.48) * (0.38 + 0.90 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                if (uDrawMode > 0.5) {
                    vec3 dqdx = dFdx(vWorldPos);
                    vec3 dqdy = dFdy(vWorldPos);
                    vec3 Nf = cross(dqdx, dqdy);
                    float nlenF = length(Nf);
                    vec3 Nface = (nlenF > 1e-6) ? (Nf / nlenF) : N0;
                    if (dot(Nface, V0) < 0.0)
                        Nface = -Nface;
                    float ndv = max(dot(Nface, V0), 0.0);
                    float edge = pow(1.0 - ndv, 1.55);
                    float rim = pow(1.0 - ndv, 3.2) * 0.55;
                    vec3 fluo = vec3(1.0, 0.22, 0.14) * (0.42 + 0.95 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                // 兜底：若 uniform 未成功设置（全 0），仍保持灰白材质；uUseVertexColor>0.5 时用顶点色（喷嘴）
                vec3 alb0 = (dot(uMeshAlbedo, uMeshAlbedo) > 1e-6) ? uMeshAlbedo : vec3(0.78, 0.79, 0.80);
                vec3 albedo = mix(alb0, clamp(vVertexColor, 0.0, 1.0), step(0.5, uUseVertexColor));
                vec3 N = N0;
                vec3 V = V0;
                vec3 ambient = albedo * 0.34;
                vec3 diffuse = vec3(0.0);
                vec3 specCol = vec3(0.0);
                vec3 lightPos[3] = vec3[3](uLightPos0, uLightPos1, uLightPos2);
                vec3 lightCol[3] = vec3[3](uLightCol0, uLightCol1, uLightCol2);
                for (int i = 0; i < 3; ++i) {
                    vec3 L = lightPos[i] - vWorldPos;
                    float dist = max(length(L), 0.001);
                    L /= dist;
                    float att = 1.0 / (1.0 + 14.0 * dist * dist);
                    float ndl = max(dot(N, L), 0.0);
                    vec3 H = normalize(L + V);
                    float spec = pow(max(dot(N, H), 0.0), 64.0);
                    diffuse += albedo * (0.55 * ndl) * lightCol[i] * att;
                    specCol += vec3(1.0) * spec * 0.22 * lightCol[i] * att;
                }
                float fresnel = pow(1.0 - max(dot(N, V), 0.0), 3.0);
                vec3 rim2 = vec3(0.75, 0.82, 0.95) * fresnel * 0.08;
                vec3 lit = ambient + diffuse + specCol + rim2;
                vec3 col = clamp(lit * 1.10 + vec3(0.02), 0.0, 1.0);
                outColor = vec4(col, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            in vec3 vWorldPos;
            in vec3 vWorldNormal;
            in vec3 vVertexColor;
            out vec4 outColor;
            uniform vec3 uCameraPos;
            uniform vec3 uLightPos0;
            uniform vec3 uLightPos1;
            uniform vec3 uLightPos2;
            uniform vec3 uLightCol0;
            uniform vec3 uLightCol1;
            uniform vec3 uLightCol2;
            uniform vec3 uMeshAlbedo;
            uniform float uDrawMode;
            uniform float uUseVertexColor;
            void main() {
                float nlen0 = length(vWorldNormal);
                vec3 N0 = (nlen0 > 1e-6) ? (vWorldNormal / nlen0) : vec3(0.0, 0.0, 1.0);
                vec3 Vdir0 = (uCameraPos - vWorldPos);
                float vlen0 = max(length(Vdir0), 1e-6);
                vec3 V0 = Vdir0 / vlen0;
                if (uDrawMode > 1.5) {
                    vec3 dqdx = dFdx(vWorldPos);
                    vec3 dqdy = dFdy(vWorldPos);
                    vec3 Nf = cross(dqdx, dqdy);
                    float nlenF = length(Nf);
                    vec3 Nface = (nlenF > 1e-6) ? (Nf / nlenF) : N0;
                    if (dot(Nface, V0) < 0.0)
                        Nface = -Nface;
                    float ndv = max(dot(Nface, V0), 0.0);
                    float edge = pow(1.0 - ndv, 1.55);
                    float rim = pow(1.0 - ndv, 3.2) * 0.55;
                    vec3 fluo = vec3(0.22, 1.0, 0.48) * (0.38 + 0.90 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                if (uDrawMode > 0.5) {
                    vec3 dqdx = dFdx(vWorldPos);
                    vec3 dqdy = dFdy(vWorldPos);
                    vec3 Nf = cross(dqdx, dqdy);
                    float nlenF = length(Nf);
                    vec3 Nface = (nlenF > 1e-6) ? (Nf / nlenF) : N0;
                    if (dot(Nface, V0) < 0.0)
                        Nface = -Nface;
                    float ndv = max(dot(Nface, V0), 0.0);
                    float edge = pow(1.0 - ndv, 1.55);
                    float rim = pow(1.0 - ndv, 3.2) * 0.55;
                    vec3 fluo = vec3(1.0, 0.22, 0.14) * (0.42 + 0.95 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                vec3 alb0 = (dot(uMeshAlbedo, uMeshAlbedo) > 1e-6) ? uMeshAlbedo : vec3(0.78, 0.79, 0.80);
                vec3 albedo = mix(alb0, clamp(vVertexColor, 0.0, 1.0), step(0.5, uUseVertexColor));
                vec3 N = N0;
                vec3 V = V0;
                vec3 ambient = albedo * 0.34;
                vec3 diffuse = vec3(0.0);
                vec3 specCol = vec3(0.0);
                vec3 lightPos[3] = vec3[3](uLightPos0, uLightPos1, uLightPos2);
                vec3 lightCol[3] = vec3[3](uLightCol0, uLightCol1, uLightCol2);
                for (int i = 0; i < 3; ++i) {
                    vec3 L = lightPos[i] - vWorldPos;
                    float dist = max(length(L), 0.001);
                    L /= dist;
                    float att = 1.0 / (1.0 + 14.0 * dist * dist);
                    float ndl = max(dot(N, L), 0.0);
                    vec3 H = normalize(L + V);
                    float spec = pow(max(dot(N, H), 0.0), 64.0);
                    diffuse += albedo * (0.55 * ndl) * lightCol[i] * att;
                    specCol += vec3(1.0) * spec * 0.22 * lightCol[i] * att;
                }
                float fresnel = pow(1.0 - max(dot(N, V), 0.0), 3.0);
                vec3 rim2 = vec3(0.75, 0.82, 0.95) * fresnel * 0.08;
                vec3 lit = ambient + diffuse + specCol + rim2;
                vec3 col = clamp(lit * 1.10 + vec3(0.02), 0.0, 1.0);
                outColor = vec4(col, 1.0);
            }
        )");

        // 坐标轴 shader（带颜色）
        const QByteArray axisVertexShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec3 position;
            layout(location = 1) in vec3 color;
            uniform mat4 mvp;
            out vec3 vColor;
            void main() {
                vColor = color;
                gl_Position = mvp * vec4(position, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 position;
            layout(location = 1) in vec3 color;
            uniform mat4 mvp;
            out vec3 vColor;
            void main() {
                vColor = color;
                gl_Position = mvp * vec4(position, 1.0);
            }
        )");

        const QByteArray axisFragmentShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec3 vColor;
            out vec4 outColor;
            void main() { outColor = vec4(vColor, 1.0); }
        )") : QByteArray(R"(
            #version 330 core
            in vec3 vColor;
            out vec4 outColor;
            void main() { outColor = vec4(vColor, 1.0); }
        )");

        // 轴标签：锚点随 model 移动，笔画偏移沿相机 right/up，始终朝向屏幕可读
        const QByteArray axisLabelVertexShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec3 anchor;
            layout(location = 1) in vec2 local;
            layout(location = 2) in vec3 color;
            uniform mat4 model;
            uniform mat4 view;
            uniform mat4 projection;
            uniform vec3 uCamRight;
            uniform vec3 uCamUp;
            out vec3 vColor;
            void main() {
                vColor = color;
                vec4 wa = model * vec4(anchor, 1.0);
                vec3 worldPos = wa.xyz + uCamRight * local.x + uCamUp * local.y;
                gl_Position = projection * view * vec4(worldPos, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 anchor;
            layout(location = 1) in vec2 local;
            layout(location = 2) in vec3 color;
            uniform mat4 model;
            uniform mat4 view;
            uniform mat4 projection;
            uniform vec3 uCamRight;
            uniform vec3 uCamUp;
            out vec3 vColor;
            void main() {
                vColor = color;
                vec4 wa = model * vec4(anchor, 1.0);
                vec3 worldPos = wa.xyz + uCamRight * local.x + uCamUp * local.y;
                gl_Position = projection * view * vec4(worldPos, 1.0);
            }
        )");

        planeProgram.removeAllShaders();
        meshProgram.removeAllShaders();
        axisProgram.removeAllShaders();
        axisLabelProgram.removeAllShaders();
        bgProgram.removeAllShaders();
        viewCubeProgram.removeAllShaders();

        // Logo shader: render texture quad on plane
        const QByteArray logoVertSrc = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec2 aUV;
            uniform mat4 uMVP;
            out vec2 vUV;
            void main() {
                vUV = aUV;
                gl_Position = uMVP * vec4(aPos, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec2 aUV;
            uniform mat4 uMVP;
            out vec2 vUV;
            void main() {
                vUV = aUV;
                gl_Position = uMVP * vec4(aPos, 1.0);
            }
        )");

        const QByteArray logoFragSrc = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec2 vUV;
            out vec4 outColor;
            uniform sampler2D uLogoTex;
            uniform float uAlpha;
            void main() {
                vec4 texColor = texture(uLogoTex, vUV);
                outColor = vec4(texColor.rgb, texColor.a * uAlpha);
            }
        )") : QByteArray(R"(
            #version 330 core
            in vec2 vUV;
            out vec4 outColor;
            uniform sampler2D uLogoTex;
            uniform float uAlpha;
            void main() {
                vec4 texColor = texture(uLogoTex, vUV);
                outColor = vec4(texColor.rgb, texColor.a * uAlpha);
            }
        )");

        if (!logoProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, logoVertSrc)) {
            qWarning() << "[Logo] Vertex shader FAILED:" << logoProgram.log();
        }
        if (!logoProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, logoFragSrc)) {
            qWarning() << "[Logo] Fragment shader FAILED:" << logoProgram.log();
        }
        logoProgram.bindAttributeLocation("aPos", 0);
        logoProgram.bindAttributeLocation("aUV", 1);
        if (!logoProgram.link()) {
            qWarning() << "[Logo] Program link FAILED:" << logoProgram.log();
        }

        setupLogoGeometry();

        // Background program compile/link
        bool bgVertOk = bgProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, bgVertexShaderSource);
        if (!bgVertOk) {
            qWarning() << "[ViewportRenderer] Background vertex shader FAILED:" << bgProgram.log();
            return;
        }
        bool bgFragOk = bgProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, bgFragmentShaderSource);
        if (!bgFragOk) {
            qWarning() << "[ViewportRenderer] Background fragment shader FAILED:" << bgProgram.log();
            return;
        }
        bgProgram.bindAttributeLocation("aPos", 0);
        bool bgLinkOk = bgProgram.link();
        if (!bgLinkOk) {
            qWarning() << "[ViewportRenderer] Background program link FAILED:" << bgProgram.log();
            return;
        }

        bool pVertOk = planeProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, planeVertexShaderSource);
        if (!pVertOk) {
            qWarning() << "[ViewportRenderer] Plane vertex shader FAILED:" << planeProgram.log();
            return;
        }

        bool pFragOk = planeProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, planeFragmentShaderSource);
        if (!pFragOk) {
            qWarning() << "[ViewportRenderer] Plane fragment shader FAILED:" << planeProgram.log();
            return;
        }

        planeProgram.bindAttributeLocation("position", 0);
        bool pLinkOk = planeProgram.link();
        if (!pLinkOk) {
            qWarning() << "[ViewportRenderer] Plane program link FAILED:" << planeProgram.log();
            return;
        }

        bool mVertOk = meshProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, meshVertexShaderSource);
        if (!mVertOk) {
            qWarning() << "[ViewportRenderer] Mesh vertex shader FAILED:" << meshProgram.log();
            return;
        }
        bool mFragOk = meshProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, meshFragmentShaderSource);
        if (!mFragOk) {
            qWarning() << "[ViewportRenderer] Mesh fragment shader FAILED:" << meshProgram.log();
            return;
        }
        meshProgram.bindAttributeLocation("position", 0);
        meshProgram.bindAttributeLocation("normal", 1);
        meshProgram.bindAttributeLocation("vertexColor", 2);
        bool mLinkOk = meshProgram.link();
        if (!mLinkOk) {
            qWarning() << "[ViewportRenderer] Mesh program link FAILED:" << meshProgram.log();
            return;
        }
        qInfo() << "[ViewportRenderer] Mesh program OK attrib pos=0 normal=1 vertexColor=2";

        // Axis program compile/link
        bool aVertOk = axisProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, axisVertexShaderSource);
        if (!aVertOk) {
            qWarning() << "[ViewportRenderer] Axis vertex shader FAILED:" << axisProgram.log();
            return;
        }
        bool aFragOk = axisProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, axisFragmentShaderSource);
        if (!aFragOk) {
            qWarning() << "[ViewportRenderer] Axis fragment shader FAILED:" << axisProgram.log();
            return;
        }
        axisProgram.bindAttributeLocation("position", 0);
        axisProgram.bindAttributeLocation("color", 1);
        bool aLinkOk = axisProgram.link();
        if (!aLinkOk) {
            qWarning() << "[ViewportRenderer] Axis program link FAILED:" << axisProgram.log();
            return;
        }

        bool alVertOk = axisLabelProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, axisLabelVertexShaderSource);
        if (!alVertOk) {
            qWarning() << "[ViewportRenderer] Axis label vertex shader FAILED:" << axisLabelProgram.log();
            return;
        }
        bool alFragOk = axisLabelProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, axisFragmentShaderSource);
        if (!alFragOk) {
            qWarning() << "[ViewportRenderer] Axis label fragment shader FAILED:" << axisLabelProgram.log();
            return;
        }
        axisLabelProgram.bindAttributeLocation("anchor", 0);
        axisLabelProgram.bindAttributeLocation("local", 1);
        axisLabelProgram.bindAttributeLocation("color", 2);
        bool alLinkOk = axisLabelProgram.link();
        if (!alLinkOk) {
            qWarning() << "[ViewportRenderer] Axis label program link FAILED:" << axisLabelProgram.log();
            return;
        }

        setupPlaneGeometry();
        setupAxisGeometry();
        setupAxisLabelGeometry();
        setupBackgroundGeometry();
        setupViewCubeResources(ctx);

        if (!bgTexture) {
            // 与 CMake qt_add_resources(huafu_assets) + HuafuSlicer.qrc 中 resources/viewport_bg.png 一致
            const QString path = QStringLiteral(":/huafuslicer/resources/viewport_bg.png");
            QImage img(path);
            if (img.isNull()) {
                qWarning() << "[Background] Failed to load image:" << path;
            } else {
                img = img.convertToFormat(QImage::Format_RGBA8888);
                bgTexture = new QOpenGLTexture(img);
                bgTexture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
                bgTexture->setWrapMode(QOpenGLTexture::ClampToEdge);
                qDebug() << "[Background] Loaded image:" << path << "size:" << img.size();
            }
        }

        // 背景按“静态图片”处理：不做时间动画
        timer.invalidate();

        {
            const QByteArray tv = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location=0) in vec3 pos;
            layout(location=1) in vec3 col;
            uniform mat4 mvp;
            out vec3 vC;
            void main(){ vC=col; gl_Position=mvp*vec4(pos,1.0); }
        )") : QByteArray(R"(
            #version 330 core
            layout(location=0) in vec3 pos;
            layout(location=1) in vec3 col;
            uniform mat4 mvp;
            out vec3 vC;
            void main(){ vC=col; gl_Position=mvp*vec4(pos,1.0); }
        )");
            const QByteArray tf = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec3 vC;
            out vec4 o;
            void main(){ o=vec4(vC,1.0); }
        )") : QByteArray(R"(
            #version 330 core
            in vec3 vC;
            out vec4 o;
            void main(){ o=vec4(vC,1.0); }
        )");
            if (!trajProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, tv)
                || !trajProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, tf)
                || !trajProgram.link()) {
                qWarning() << "[ViewportRenderer] Trajectory program FAILED:" << trajProgram.log();
            } else {
                vboTraj.create();
                vaoTraj.create();
                QOpenGLFunctions *gf = ctx->functions();
                vaoTraj.bind();
                vboTraj.bind();
                gf->glEnableVertexAttribArray(0);
                gf->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * int(sizeof(float)), nullptr);
                gf->glEnableVertexAttribArray(1);
                gf->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * int(sizeof(float)),
                                         reinterpret_cast<const void *>(quintptr(3 * sizeof(float))));
                vaoTraj.release();
                vboTraj.release();
            }
        }

        setupPreviewNozzleGeometry(ctx);

        initialized = true;
        qDebug() << "[ViewportRenderer] Init done. Plane:" << planeVertexCount << "Axis:" << axisVertexCount
                 << "AxisLabels:" << axisLabelVertexCount;
        qDebug() << "=== [ViewportRenderer] Initialization finished ===";
    }

    void setupBackgroundGeometry() {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLFunctions *f = ctx->functions();
        if (!f) {
            qWarning() << "[Background] ERROR: No QOpenGLFunctions!";
            return;
        }

        // Fullscreen quad (2 triangles): aPos(x,y)
        const float verts[] = {
            -1.0f, -1.0f,
            +1.0f, -1.0f,
            -1.0f, +1.0f,
            +1.0f, -1.0f,
            +1.0f, +1.0f,
            -1.0f, +1.0f
        };
        bgVertexCount = 6;

        vboBg.create();
        if (!vboBg.isCreated()) {
            qWarning() << "[Background] ERROR: VBO not created!";
            return;
        }
        vboBg.bind();
        vboBg.allocate(verts, int(sizeof(verts)));

        vaoBg.create();
        if (!vaoBg.isCreated()) {
            qWarning() << "[Background] ERROR: VAO not created!";
            vboBg.release();
            return;
        }
        vaoBg.bind();
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        vaoBg.release();
        vboBg.release();
    }

    void setupPlaneGeometry() {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLFunctions *f = ctx->functions();
        if (!f) {
            qWarning() << "[Plane] ERROR: No QOpenGLFunctions!";
            return;
        }
        
        qDebug() << "[Plane] Creating plane vertices...";
        
        // z=0 平面（XY）：可打印区从 (0,0)，底板略大于 1m 便于边缘
        QVector<float> vertices = {
            // tri 1
            kBedVisMin,  kBedVisMin,  0.0f,  0.4f, 0.4f, 0.4f,
            kBedVisMax,  kBedVisMin,  0.0f,  0.4f, 0.4f, 0.4f,
            kBedVisMin,  kBedVisMax,  0.0f,  0.4f, 0.4f, 0.4f,
            // tri 2
            kBedVisMax,  kBedVisMin,  0.0f,  0.4f, 0.4f, 0.4f,
            kBedVisMax,  kBedVisMax,  0.0f,  0.4f, 0.4f, 0.4f,
            kBedVisMin,  kBedVisMax,  0.0f,  0.4f, 0.4f, 0.4f,
        };

        planeVertexCount = 6;
        qDebug() << "[Plane] Vertex count:" << planeVertexCount;

        vboPlane.create();
        if (!vboPlane.isCreated()) {
            qWarning() << "[Plane] ERROR: VBO not created!";
            return;
        }
        
        vboPlane.bind();
        vboPlane.allocate(vertices.constData(), vertices.size() * sizeof(float));

        vaoPlane.create();
        if (!vaoPlane.isCreated()) {
            qWarning() << "[Plane] ERROR: VAO not created!";
            vboPlane.release();
            return;
        }
        
        vaoPlane.bind();
        
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        
        vaoPlane.release();
        vboPlane.release();
        
        qDebug() << "[Plane] SUCCESS - VAO/VBO created. Vertices:" << planeVertexCount;
    }

    void setupGridGeometry() {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLFunctions *f = ctx->functions();
        
        qDebug() << "[Grid] Creating grid vertices...";
        
        QVector<float> vertices;
        const float L = kBedPlaneExtentMeters;
        const float zMajor = 0.001f;
        const float zMinor = 0.0015f;
        const float zEdge = 0.002f;
        const float majorStep = 0.1f;  // 100mm
        const float minorStep = 0.05f; // 50mm

        // 主网格线（XY 平面，与热床一致）
        QVector3D gridColor(0.35f, 0.38f, 0.42f);
        for (float t = 0.0f; t <= L + 1e-5f; t += majorStep) {
            vertices << t << 0.0f << zMajor << gridColor.x() << gridColor.y() << gridColor.z();
            vertices << t << L << zMajor << gridColor.x() << gridColor.y() << gridColor.z();
            vertices << 0.0f << t << zMajor << gridColor.x() << gridColor.y() << gridColor.z();
            vertices << L << t << zMajor << gridColor.x() << gridColor.y() << gridColor.z();
        }

        // 细网格线（50mm，与 100mm 主线错开）
        QVector3D fineColor(0.25f, 0.28f, 0.32f);
        for (float t = minorStep; t <= L - minorStep + 1e-5f; t += minorStep) {
            const int stepIndex = int(qRound(t / minorStep));
            if (stepIndex % 2 == 0)
                continue;
            vertices << t << 0.0f << zMinor << fineColor.x() << fineColor.y() << fineColor.z();
            vertices << t << L << zMinor << fineColor.x() << fineColor.y() << fineColor.z();
            vertices << 0.0f << t << zMinor << fineColor.x() << fineColor.y() << fineColor.z();
            vertices << L << t << zMinor << fineColor.x() << fineColor.y() << fineColor.z();
        }

        // 原点处坐标轴边线（与左下角视图立方体轴一致：X 红、Y 绿、Z 蓝）
        const float zAxisLen = qMin(0.08f, L * 0.12f);
        QVector3D cx(0.90f, 0.20f, 0.20f);
        QVector3D cy(0.20f, 0.90f, 0.20f);
        QVector3D cz(0.20f, 0.45f, 0.95f);
        vertices << 0.0f << 0.0f << zEdge << cx.x() << cx.y() << cx.z();
        vertices << L << 0.0f << zEdge << cx.x() << cx.y() << cx.z();
        vertices << 0.0f << 0.0f << zEdge << cy.x() << cy.y() << cy.z();
        vertices << 0.0f << L << zEdge << cy.x() << cy.y() << cy.z();
        vertices << 0.0f << 0.0f << zEdge << cz.x() << cz.y() << cz.z();
        vertices << 0.0f << 0.0f << zEdge + zAxisLen << cz.x() << cz.y() << cz.z();

        gridVertexCount = vertices.size() / 6;
        qDebug() << "[Grid] Total vertices:" << gridVertexCount;

        vboGrid.create();
        vboGrid.bind();
        vboGrid.allocate(vertices.constData(), vertices.size() * sizeof(float));

        vaoGrid.create();
        vaoGrid.bind();
        
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        
        vaoGrid.release();
        vboGrid.release();
        
        qDebug() << "[Grid] SUCCESS - VAO/VBO created";
    }

    void setupAxisGeometry() {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLFunctions *f = ctx->functions();
        
        qDebug() << "[Axis] Creating axis vertices...";
        // 数据：position(3) + color(3)
        QVector<float> vertices;
        auto push = [&](float x, float y, float z, float r, float g, float b) {
            vertices << x << y << z << r << g << b;
        };

        const float L = 0.12f;
        const float a = 0.01f;   // arrow length
        const float w = 0.006f;  // arrow half width

        // 线段（GL_LINES）：每轴 2 个点
        // X 轴（红）
        push(0, 0, 0, 0.90f, 0.20f, 0.20f);
        push(L, 0, 0, 0.90f, 0.20f, 0.20f);
        // Y 轴（绿）
        push(0, 0, 0, 0.20f, 0.90f, 0.20f);
        push(0, L, 0, 0.20f, 0.90f, 0.20f);
        // Z 轴（蓝）
        push(0, 0, 0, 0.20f, 0.45f, 0.95f);
        push(0, 0, L, 0.20f, 0.45f, 0.95f);

        axisLineVertexCount = 6;

        // 箭头（三角形，扁平箭头）
        // X arrow in XY plane at (L,0,0)
        push(L, 0, 0, 0.90f, 0.20f, 0.20f);
        push(L - a, +w, 0, 0.90f, 0.20f, 0.20f);
        push(L - a, -w, 0, 0.90f, 0.20f, 0.20f);

        // Y arrow in XY plane at (0,L,0)
        push(0, L, 0, 0.20f, 0.90f, 0.20f);
        push(+w, L - a, 0, 0.20f, 0.90f, 0.20f);
        push(-w, L - a, 0, 0.20f, 0.90f, 0.20f);

        // Z arrow in XZ plane at (0,0,L)
        push(0, 0, L, 0.20f, 0.45f, 0.95f);
        push(+w, 0, L - a, 0.20f, 0.45f, 0.95f);
        push(-w, 0, L - a, 0.20f, 0.45f, 0.95f);

        axisVertexCount = vertices.size() / 6;

        vboAxis.create();
        vboAxis.bind();
        vboAxis.allocate(vertices.constData(), vertices.size() * sizeof(float));

        vaoAxis.create();
        vaoAxis.bind();
        
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        
        vaoAxis.release();
        vboAxis.release();
        
        qDebug() << "[Axis] SUCCESS - VAO/VBO created";
    }

    void setupAxisLabelGeometry() {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLFunctions *f = ctx->functions();

        qDebug() << "[AxisLabel] Creating billboard label vertices...";
        // anchor(3) + local(2) + color(3) = 8 floats / vertex
        QVector<float> vertices;
        auto pushV = [&](float ax, float ay, float az, float lx, float ly, float r, float g, float b) {
            vertices << ax << ay << az << lx << ly << r << g << b;
        };
        auto segXY = [&](float cx, float cy, float cz, float x0, float y0, float x1, float y1, float r, float g, float b) {
            pushV(cx, cy, cz, x0 - cx, y0 - cy, r, g, b);
            pushV(cx, cy, cz, x1 - cx, y1 - cy, r, g, b);
        };
        auto segXZ = [&](float cx, float cy, float cz, float x0, float z0, float x1, float z1, float r, float g, float b) {
            pushV(cx, cy, cz, x0 - cx, z0 - cz, r, g, b);
            pushV(cx, cy, cz, x1 - cx, z1 - cz, r, g, b);
        };

        const float L = 0.12f;
        const float gap = 0.016f;
        const float s = 0.011f;
        const float rx = 0.90f, gx = 0.20f, bx = 0.20f;
        const float ry = 0.20f, gy = 0.90f, by = 0.20f;
        const float rz = 0.20f, gz = 0.45f, bz = 0.95f;

        const float cX = L + gap + 0.5f * s;
        const float cY = L + gap + (0.45f * s - s) * 0.5f;
        const float cZ = L + gap + 0.5f * s;

        // "X" near +X（XY 平面内笔画 → local 用 dx,dy）
        segXY(cX, 0, 0, L + gap, -s * 0.5f, L + gap + s, s * 0.5f, rx, gx, bx);
        segXY(cX, 0, 0, L + gap, s * 0.5f, L + gap + s, -s * 0.5f, rx, gx, bx);

        // "Y" near +Y
        segXY(0, cY, 0, -s * 0.35f, L + gap + s * 0.45f, 0, L + gap, ry, gy, by);
        segXY(0, cY, 0, +s * 0.35f, L + gap + s * 0.45f, 0, L + gap, ry, gy, by);
        segXY(0, cY, 0, 0, L + gap, 0, L + gap - s, ry, gy, by);

        // "Z" near +Z（XZ 平面内笔画 → local 用 dx,dz 存在 local.x/local.y）
        segXZ(0, 0, cZ, -s * 0.5f, L + gap, +s * 0.5f, L + gap, rz, gz, bz);
        segXZ(0, 0, cZ, -s * 0.5f, L + gap + s, +s * 0.5f, L + gap + s, rz, gz, bz);
        segXZ(0, 0, cZ, +s * 0.5f, L + gap, -s * 0.5f, L + gap + s, rz, gz, bz);

        axisLabelVertexCount = vertices.size() / 8;

        vboAxisLabel.create();
        vboAxisLabel.bind();
        vboAxisLabel.allocate(vertices.constData(), vertices.size() * sizeof(float));

        vaoAxisLabel.create();
        vaoAxisLabel.bind();
        const GLsizei stride = 8 * sizeof(float);
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
        f->glEnableVertexAttribArray(2);
        f->glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void *)(5 * sizeof(float)));
        vaoAxisLabel.release();
        vboAxisLabel.release();

        qDebug() << "[AxisLabel] SUCCESS - vertices:" << axisLabelVertexCount;
    }

    void setupLogoGeometry() {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLFunctions *f = ctx->functions();
        if (!f) {
            qWarning() << "[Logo] No GL functions";
            return;
        }

        qDebug() << "[Logo] Creating logo quad...";

        // Logo quad on the plane: positioned in center-bottom area
        // Plane is in XY plane at z=0, with coordinates from -0.04 to 1.04
        // Logo positioned at bottom-right of the plane
        const float logoW = 0.18f;
        const float logoH = 0.045f;
        const float logoX = 0.75f;  // Right side
        const float logoY = 0.08f;   // Bottom area
        const float logoZ = 0.002f;  // Slightly above plane to avoid z-fighting

        QVector<float> vertices = {
            // tri 1: bottom-left, bottom-right, top-left
            logoX,         logoY,         logoZ,  0.0f, 1.0f,
            logoX + logoW, logoY,         logoZ,  1.0f, 1.0f,
            logoX,         logoY + logoH,  logoZ,  0.0f, 0.0f,
            // tri 2: bottom-right, top-right, top-left
            logoX + logoW, logoY,         logoZ,  1.0f, 1.0f,
            logoX + logoW, logoY + logoH,  logoZ,  1.0f, 0.0f,
            logoX,         logoY + logoH,  logoZ,  0.0f, 0.0f,
        };

        logoVertexCount = 6;

        vboLogo.create();
        if (!vboLogo.isCreated()) {
            qWarning() << "[Logo] VBO not created!";
            return;
        }

        vboLogo.bind();
        vboLogo.allocate(vertices.constData(), vertices.size() * sizeof(float));

        vaoLogo.create();
        if (!vaoLogo.isCreated()) {
            qWarning() << "[Logo] VAO not created!";
            vboLogo.release();
            return;
        }

        vaoLogo.bind();

        f->glEnableVertexAttribArray(0);  // position
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);  // UV
        f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

        vaoLogo.release();
        vboLogo.release();

        qDebug() << "[Logo] SUCCESS - VAO/VBO created, vertices:" << logoVertexCount;
    }

    void setupViewCubeResources(QOpenGLContext *ctx)
    {
        QOpenGLFunctions *f = ctx->functions();
        if (!f) {
            qWarning() << "[ViewCube] No GL functions";
            return;
        }

        const QByteArray vcVertEs = QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec2 aUV;
            layout(location = 2) in float aFace;
            uniform mat4 uMvp;
            uniform mat3 uRot;
            out vec2 vUV;
            flat out float vFace;
            flat out vec3 vNw;
            int faceId(float x) { return int(x + 0.5); }
            vec3 faceNormalLocal(int fi) {
                if (fi == 0) return vec3(0.0, 0.0, 1.0);
                if (fi == 1) return vec3(0.0, 0.0, -1.0);
                if (fi == 2) return vec3(-1.0, 0.0, 0.0);
                if (fi == 3) return vec3(1.0, 0.0, 0.0);
                if (fi == 4) return vec3(0.0, 1.0, 0.0);
                return vec3(0.0, -1.0, 0.0);
            }
            void main() {
                int fi = faceId(aFace);
                vFace = aFace;
                vUV = aUV;
                vec3 nLoc = faceNormalLocal(fi);
                vNw = normalize(uRot * nLoc);
                gl_Position = uMvp * vec4(aPos, 1.0);
            }
        )");
        const QByteArray vcVertGl = QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec2 aUV;
            layout(location = 2) in float aFace;
            uniform mat4 uMvp;
            uniform mat3 uRot;
            out vec2 vUV;
            flat out float vFace;
            flat out vec3 vNw;
            int faceId(float x) { return int(x + 0.5); }
            vec3 faceNormalLocal(int fi) {
                if (fi == 0) return vec3(0.0, 0.0, 1.0);
                if (fi == 1) return vec3(0.0, 0.0, -1.0);
                if (fi == 2) return vec3(-1.0, 0.0, 0.0);
                if (fi == 3) return vec3(1.0, 0.0, 0.0);
                if (fi == 4) return vec3(0.0, 1.0, 0.0);
                return vec3(0.0, -1.0, 0.0);
            }
            void main() {
                int fi = faceId(aFace);
                vFace = aFace;
                vUV = aUV;
                vec3 nLoc = faceNormalLocal(fi);
                vNw = normalize(uRot * nLoc);
                gl_Position = uMvp * vec4(aPos, 1.0);
            }
        )");
        const QByteArray vcFragEs = QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec2 vUV;
            flat in float vFace;
            flat in vec3 vNw;
            uniform int uHoverFace;
            uniform vec3 uLightDirWorld;
            uniform sampler2D uAtlas;
            out highp vec4 outColor;
            void main() {
                int fi = int(vFace + 0.5);
                vec3 n = normalize(vNw);
                vec3 L = normalize(uLightDirWorld);
                // lighting tint (very subtle) to keep faces looking slightly 3D
                float ndl = max(dot(n, L), 0.0);
                float tile = 1.0 / 6.0;
                vec2 atlasUV = vec2(vUV.x * tile + float(fi) * tile, vUV.y);
                vec4 t = texture(uAtlas, atlasUV);
                // apply slight shading to base depending on normal
                vec3 shaded = mix(vec3(1.0), vec3(0.96,0.96,0.98), 1.0 - ndl);
                // atlas already encodes white background and black glyphs
                outColor = vec4(t.rgb * shaded, t.a);
                // hover tint
                if (fi == uHoverFace) {
                    vec3 tint = vec3(0.70, 0.85, 1.00);
                    outColor.rgb = mix(outColor.rgb, tint, 0.35);
                }
            }
        )");
        const QByteArray vcFragGl = QByteArray(R"(
            #version 330 core
            in vec2 vUV;
            flat in float vFace;
            flat in vec3 vNw;
            uniform int uHoverFace;
            uniform vec3 uLightDirWorld;
            uniform sampler2D uAtlas;
            out vec4 outColor;
            void main() {
                int fi = int(vFace + 0.5);
                vec3 n = normalize(vNw);
                vec3 L = normalize(uLightDirWorld);
                float ndl = max(dot(n, L), 0.0);
                float tile = 1.0 / 6.0;
                vec2 atlasUV = vec2(vUV.x * tile + float(fi) * tile, vUV.y);
                vec4 t = texture(uAtlas, atlasUV);
                vec3 shaded = mix(vec3(1.0), vec3(0.96,0.96,0.98), 1.0 - ndl);
                outColor = vec4(t.rgb * shaded, t.a);
                if (fi == uHoverFace) {
                    vec3 tint = vec3(0.70, 0.85, 1.00);
                    outColor.rgb = mix(outColor.rgb, tint, 0.35);
                }
            }
        )");

        const QByteArray vSrc = ctx->isOpenGLES() ? vcVertEs : vcVertGl;
        const QByteArray fSrc = ctx->isOpenGLES() ? vcFragEs : vcFragGl;
        if (!viewCubeProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, vSrc)) {
            qWarning() << "[ViewCube] vertex shader failed:" << viewCubeProgram.log();
            return;
        }
        if (!viewCubeProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, fSrc)) {
            qWarning() << "[ViewCube] fragment shader failed:" << viewCubeProgram.log();
            return;
        }
        viewCubeProgram.bindAttributeLocation("aPos", 0);
        viewCubeProgram.bindAttributeLocation("aFace", 2);
        if (!viewCubeProgram.link()) {
            qWarning() << "[ViewCube] link failed:" << viewCubeProgram.log();
            return;
        }

        QVector<float> verts;
        auto addQuad = [&](int face, float x0, float y0, float z0, float u0, float v0, float x1, float y1, float z1,
                           float u1, float v1, float x2, float y2, float z2, float u2, float v2, float x3, float y3,
                           float z3, float u3, float v3) {
            auto vv = [&](float x, float y, float z, float u, float vv2) {
                verts << x << y << z << u << vv2 << float(face);
            };
            vv(x0, y0, z0, u0, v0);
            vv(x1, y1, z1, u1, v1);
            vv(x2, y2, z2, u2, v2);
            vv(x0, y0, z0, u0, v0);
            vv(x2, y2, z2, u2, v2);
            vv(x3, y3, z3, u3, v3);
        };

        // 立方体 6 面：统一外侧 CCW 绕序；aFace / atlas 列：0 +Z 上 1 -Z 下 2 -X 左 3 +X 右 4 +Y 后 5 -Y 前
        // +Z「上」：先绕纹理中心 180°，再对 u 翻转，消除左右镜像；仅本面，其余面不动
        addQuad(0,
                -0.5f, -0.5f, +0.5f, 0, 1,
                +0.5f, -0.5f, +0.5f, 1, 1,
                +0.5f, +0.5f, +0.5f, 1, 0,
                -0.5f, +0.5f, +0.5f, 0, 0);
        // -Z「下」：水平翻转纹理 u→1-u，消除字面左右镜像（仅本面）
        addQuad(1,
                +0.5f, -0.5f, -0.5f, 1, 0,
                -0.5f, -0.5f, -0.5f, 0, 0,
                -0.5f, +0.5f, -0.5f, 0, 1,
                +0.5f, +0.5f, -0.5f, 1, 1);
        // -X「左」：水平翻转纹理 u→1-u，消除字左右镜像（仅本面）
        addQuad(2,
                -0.5f, -0.5f, -0.5f, 1, 1,
                -0.5f, -0.5f, +0.5f, 1, 0,
                -0.5f, +0.5f, +0.5f, 0, 0,
                -0.5f, +0.5f, -0.5f, 0, 1);
        // +X「右」：在上次 UV 上再绕中心旋转 180°（u,v）→（1-u,1-v）
        addQuad(3,
                +0.5f, -0.5f, +0.5f, 0, 0,
                +0.5f, -0.5f, -0.5f, 0, 1,
                +0.5f, +0.5f, -0.5f, 1, 1,
                +0.5f, +0.5f, +0.5f, 1, 0);
        // +Y「后」：在既有 (u,1-v) 定向基础上，再绕纹理中心逆时针 90° → (u',v')=(1-v,u)；仅本面
        addQuad(4,
                -0.5f, +0.5f, -0.5f, 0, 0,
                -0.5f, +0.5f, +0.5f, 0, 1,
                +0.5f, +0.5f, +0.5f, 1, 1,
                +0.5f, +0.5f, -0.5f, 1, 0);
        // -Y 面「前」：翻转 v，避免字相对世界 +Z 倒立
        addQuad(5,
                -0.5f, -0.5f, -0.5f, 0, 1,
                +0.5f, -0.5f, -0.5f, 1, 1,
                +0.5f, -0.5f, +0.5f, 1, 0,
                -0.5f, -0.5f, +0.5f, 0, 0);

        viewCubeVertexCount = verts.size() / 6;
        vboViewCube.create();
        vboViewCube.bind();
        vboViewCube.allocate(verts.constData(), int(verts.size() * sizeof(float)));
        vaoViewCube.create();
        vaoViewCube.bind();
        const GLsizei stride = GLsizei(6 * sizeof(float));
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
        f->glEnableVertexAttribArray(2);
        f->glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)(5 * sizeof(float)));
        vaoViewCube.release();
        vboViewCube.release();

        // build edge lines for cube (black)
        QVector<float> edgeVerts;
        auto pushE = [&](float x, float y, float z) { edgeVerts << x << y << z << 0.0f << 0.0f << 0.0f; };
        // corners
        const QVector3D A(-0.5f, -0.5f, -0.5f);
        const QVector3D B(+0.5f, -0.5f, -0.5f);
        const QVector3D C(-0.5f, +0.5f, -0.5f);
        const QVector3D D(+0.5f, +0.5f, -0.5f);
        const QVector3D E(+ -0.5f, -0.5f, +0.5f); // placeholder to keep naming
        (void)E;
        // we'll push pairs explicitly
        // X edges
        pushE(-0.5f, -0.5f, -0.5f); pushE(+0.5f, -0.5f, -0.5f);
        pushE(-0.5f, +0.5f, -0.5f); pushE(+0.5f, +0.5f, -0.5f);
        pushE(-0.5f, -0.5f, +0.5f); pushE(+0.5f, -0.5f, +0.5f);
        pushE(-0.5f, +0.5f, +0.5f); pushE(+0.5f, +0.5f, +0.5f);
        // Y edges
        pushE(-0.5f, -0.5f, -0.5f); pushE(-0.5f, +0.5f, -0.5f);
        pushE(+0.5f, -0.5f, -0.5f); pushE(+0.5f, +0.5f, -0.5f);
        pushE(-0.5f, -0.5f, +0.5f); pushE(-0.5f, +0.5f, +0.5f);
        pushE(+0.5f, -0.5f, +0.5f); pushE(+0.5f, +0.5f, +0.5f);
        // Z edges
        pushE(-0.5f, -0.5f, -0.5f); pushE(-0.5f, -0.5f, +0.5f);
        pushE(+0.5f, -0.5f, -0.5f); pushE(+0.5f, -0.5f, +0.5f);
        pushE(-0.5f, +0.5f, -0.5f); pushE(-0.5f, +0.5f, +0.5f);
        pushE(+0.5f, +0.5f, -0.5f); pushE(+0.5f, +0.5f, +0.5f);

        viewCubeEdgeVertexCount = edgeVerts.size() / 6;
        vboViewCubeEdges.create();
        vboViewCubeEdges.bind();
        vboViewCubeEdges.allocate(edgeVerts.constData(), int(edgeVerts.size() * sizeof(float)));
        vaoViewCubeEdges.create();
        vaoViewCubeEdges.bind();
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
        vaoViewCubeEdges.release();
        vboViewCubeEdges.release();

        // 视图立方体配套坐标轴（position(3)+color(3)）
        // Axis vertices are defined relative to cube-corner origin (0,0,0).
        QVector<float> axisVerts;
        auto pushAxis = [&](float x, float y, float z, float r, float g, float b) {
            axisVerts << x << y << z << r << g << b;
        };
        // Local origin at corner (0,0,0). Axis length covers one cube edge (1.0)
        const float Ledge = 1.0f;
        const float a = 0.12f;    // arrow length
        const float w = 0.04f;    // arrow half-width

        // axis lines: start at (0,0,0)
        pushAxis(0.0f, 0.0f, 0.0f, 0.90f, 0.20f, 0.20f); // X start
        pushAxis(Ledge, 0.0f, 0.0f, 0.90f, 0.20f, 0.20f); // X end
        pushAxis(0.0f, 0.0f, 0.0f, 0.20f, 0.90f, 0.20f); // Y start
        pushAxis(0.0f, Ledge, 0.0f, 0.20f, 0.90f, 0.20f); // Y end
        pushAxis(0.0f, 0.0f, 0.0f, 0.20f, 0.45f, 0.95f); // Z start
        pushAxis(0.0f, 0.0f, Ledge, 0.20f, 0.45f, 0.95f); // Z end
        viewCubeAxisLineVertexCount = 6;

        // arrows (flat triangles) at ends
        // X arrow (at Ledge,0,0)
        pushAxis(Ledge, 0.0f, 0.0f, 0.90f, 0.20f, 0.20f);
        pushAxis(Ledge - a, +w, 0.0f, 0.90f, 0.20f, 0.20f);
        pushAxis(Ledge - a, -w, 0.0f, 0.90f, 0.20f, 0.20f);
        // Y arrow (at 0,Ledge,0)
        pushAxis(0.0f, Ledge, 0.0f, 0.20f, 0.90f, 0.20f);
        pushAxis(+w, Ledge - a, 0.0f, 0.20f, 0.90f, 0.20f);
        pushAxis(-w, Ledge - a, 0.0f, 0.20f, 0.90f, 0.20f);
        // Z arrow at +Z end（与线段 +Z 一致，与热床 Z 向上一致）
        pushAxis(0.0f, 0.0f, Ledge, 0.20f, 0.45f, 0.95f);
        pushAxis(+w, 0.0f, Ledge - a, 0.20f, 0.45f, 0.95f);
        pushAxis(-w, 0.0f, Ledge - a, 0.20f, 0.45f, 0.95f);

        viewCubeAxisVertexCount = axisVerts.size() / 6;

        vboViewCubeAxis.create();
        vboViewCubeAxis.bind();
        vboViewCubeAxis.allocate(axisVerts.constData(), int(axisVerts.size() * sizeof(float)));
        vaoViewCubeAxis.create();
        vaoViewCubeAxis.bind();
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                                 reinterpret_cast<const void *>(quintptr(3 * sizeof(float))));
        vaoViewCubeAxis.release();
        vboViewCubeAxis.release();

        // 生成面文字 atlas：6 列与 aFace 一致：上 下 左 右 后 前（+Z/-Z 两字与几何面对齐）
#if HAVE_FREETYPE
        if (!viewCubeAtlas) {
            const int cellW = 256;
            const int cellH = 256;
            const int cols = 6;
            const int atlasW = cellW * cols;
            const int atlasH = cellH;

            // Unicode：上 下 左 右 后 前（+Z/-Z/-X/+X/+Y/-Y）
            const std::vector<uint32_t> chars = { 0x4E0A, 0x4E0B, 0x5DE6, 0x53F3, 0x540E, 0x524D };

            // Allocate RGBA buffer (white background)
            std::vector<unsigned char> pixels(size_t(atlasW) * atlasH * 4, 255);

            FT_Library ft = nullptr;
            if (FT_Init_FreeType(&ft) == 0) {
                // 修改为你系统上的中文字体路径（若在 Windows 上通常可用）
                const char *fontPath = "C:/Windows/Fonts/msyh.ttf";
                FT_Face face = nullptr;
                if (FT_New_Face(ft, fontPath, 0, &face) == 0) {
                    // 设定像素大小
                    const int pixelSize = int(cellH * 0.75);
                    FT_Set_Pixel_Sizes(face, 0, pixelSize);

                    const int padding = 4;
                    for (int i = 0; i < (int)chars.size(); ++i) {
                        uint32_t c = chars[i];
                        if (FT_Load_Char(face, c, FT_LOAD_RENDER))
                            continue;
                        FT_GlyphSlot g = face->glyph;
                        int gw = g->bitmap.width;
                        int gh = g->bitmap.rows;

                        // place glyph centered in cell
                        int cellX = i * cellW;
                        int dstX = cellX + (cellW - gw) / 2;
                        int dstY = padding + (cellH - padding * 2 - gh) / 2;

                        for (int yy = 0; yy < gh; ++yy) {
                            for (int xx = 0; xx < gw; ++xx) {
                                unsigned char v = g->bitmap.buffer[yy * g->bitmap.pitch + xx];
                                int ax = dstX + xx;
                                int ay = dstY + yy;
                                if (ax < 0 || ax >= atlasW || ay < 0 || ay >= atlasH) continue;
                                size_t idx = (size_t(ay) * atlasW + ax) * 4;
                                // 将灰度值转换为黑字叠加在白底上的 RGB：rgb = 255 - v
                                unsigned char col = (unsigned char)(255 - v);
                                pixels[idx + 0] = col;
                                pixels[idx + 1] = col;
                                pixels[idx + 2] = col;
                                pixels[idx + 3] = 255;
                            }
                        }
                    }
                    FT_Done_Face(face);
                } else {
                    qWarning() << "[ViewCube] FT_New_Face failed, fontPath=" << QString::fromUtf8("C:/Windows/Fonts/msyh.ttf");
                }
                FT_Done_FreeType(ft);
            } else {
                qWarning() << "[ViewCube] FT_Init_FreeType failed";
            }

            // 上传到 QOpenGLTexture
            QImage img(atlasW, atlasH, QImage::Format_RGBA8888);
            // copy pixels into QImage
            for (int y = 0; y < atlasH; ++y) {
                unsigned char *scan = img.scanLine(y);
                memcpy(scan, pixels.data() + size_t(y) * atlasW * 4, size_t(atlasW) * 4);
            }
            viewCubeAtlas = new QOpenGLTexture(img);
            viewCubeAtlas->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
            viewCubeAtlas->setWrapMode(QOpenGLTexture::ClampToEdge);
        }
#else
        // Fallback: use QPainter to draw Chinese characters if FreeType not available
        if (!viewCubeAtlas) {
            const int cellW = 256;
            const int cellH = 256;
            const int cols = 6;
            QImage img(cellW * cols, cellH, QImage::Format_RGBA8888);
            img.fill(Qt::white);
            QPainter painter(&img);
            QFont font(QStringLiteral("Microsoft YaHei"));
            font.setBold(true);
            font.setPixelSize(int(cellH * 0.7));
            painter.setFont(font);
            painter.setPen(Qt::black);
            const QStringList labels = { QStringLiteral("上"), QStringLiteral("下"), QStringLiteral("左"), QStringLiteral("右"), QStringLiteral("后"), QStringLiteral("前") };
            for (int i = 0; i < cols; ++i) {
                QRect rc(i * cellW, 0, cellW, cellH);
                const QString &s = labels[i];
                QFontMetrics fm(font);
                int tw = fm.horizontalAdvance(s);
                int tx = rc.x() + (cellW - tw) / 2;
                int ty = rc.y() + (cellH + fm.ascent() - fm.descent()) / 2;
                painter.drawText(tx, ty, s);
            }
            painter.end();
            viewCubeAtlas = new QOpenGLTexture(img);
            viewCubeAtlas->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
            viewCubeAtlas->setWrapMode(QOpenGLTexture::ClampToEdge);
        }
#endif

        viewCubeOk = true;
        qDebug() << "[ViewCube] ready, verts:" << viewCubeVertexCount;
    }

    void render() override {
        ensureInitialized();

        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            qWarning() << "[Render] ERROR: No OpenGL context!";
            return;
        }
        QOpenGLFunctions *f = ctx->functions();
        if (!f) {
            qWarning() << "[Render] ERROR: No QOpenGLFunctions!";
            return;
        }
        
        // 保险起见：明确 viewport 与 FBO 尺寸一致（否则投影/裁剪可能不符合预期）
        QSize fbSize(800, 600);
        if (auto *fbo = framebufferObject())
            fbSize = fbo->size();
        f->glViewport(0, 0, fbSize.width(), fbSize.height());

        f->glDisable(GL_CULL_FACE);
        f->glDisable(GL_DEPTH_TEST);

        // 先画背景（覆盖整屏，不写深度），再清 depth 进入 3D 绘制
        if (initialized && bgTexture && vaoBg.isCreated() && bgProgram.bind()) {
            bgTexture->bind(0);
            bgProgram.setUniformValue("uBgTex", 0);
            vaoBg.bind();
            f->glDrawArrays(GL_TRIANGLES, 0, bgVertexCount);
            vaoBg.release();
            bgProgram.release();
            bgTexture->release();
        } else {
            // 背景图未加载时：填充暗灰
            f->glClearColor(18.0f / 255.0f, 18.0f / 255.0f, 18.0f / 255.0f, 1.0f);
            f->glClear(GL_COLOR_BUFFER_BIT);
        }

        f->glClear(GL_DEPTH_BUFFER_BIT);
        f->glEnable(GL_DEPTH_TEST);

        QMatrix4x4 projection;
        const float aspect = fbSize.height() > 0 ? float(fbSize.width()) / float(fbSize.height()) : (800.0f / 600.0f);
        // 旋转时避免视锥裁剪导致“被隐藏”
        projection.perspective(45.0f, aspect, 0.001f, 100.0f);

        // 鼠标交互：绕平面几何中心轨道相机（Ctrl+右键平移）
        const QVector3D orbitCenter = m_orbitTarget;
        const float yaw = qDegreesToRadians(m_yawDeg);
        const float pitch = qDegreesToRadians(m_pitchDeg);
        const float cp = qCos(pitch);
        // Z-up 轨道相机：Z 为竖直方向
        const QVector3D offset(
            m_distance * cp * qSin(yaw),
            m_distance * cp * qCos(yaw),
            m_distance * qSin(pitch)
        );
        const QVector3D eye = orbitCenter + offset;
        QMatrix4x4 view;
        const QVector3D fwd = orbitCenter - eye;
        view.lookAt(eye, orbitCenter, stableLookAtUp(fwd, m_syncedLookAtUp));

        // 模型绕平面几何中心：姿态由四元数累积（左键拖动与屏幕左右/上下一致）
        QMatrix4x4 model;
        model.translate(orbitCenter);
        model.rotate(m_planeOrientation);
        model.translate(-orbitCenter);

        QMatrix4x4 mvp = projection * view * model;

        const QMatrix4x4 invView = view.inverted();
        const QVector3D camRight = QVector3D(invView(0, 0), invView(1, 0), invView(2, 0)).normalized();
        const QVector3D camUp = QVector3D(invView(0, 1), invView(1, 1), invView(2, 1)).normalized();

        if (!initialized) {
            // 初始化失败（例如 shader 编译/链接失败），这里直接返回避免使用未就绪资源
            return;
        }

        if (!loggedFirstFrame) {
            qDebug() << "[Render] First frame rendered";
            loggedFirstFrame = true;
        }

        if (m_meshGpuDirty) {
            m_meshGpuDirty = false;
            uploadImportedMesh(f);
        }

        if (meshDrawVertexCount > 0 && m_syncedMeshVersion != m_meshDiagVersion) {
            m_meshDiagVersion = m_syncedMeshVersion;
            logMeshVisibilityDiagnostics(mvp, eye, orbitCenter);
        }

        // 画平面（金属板 + 刻度网格）
        if (vaoPlane.isCreated() && planeProgram.bind()) {
            // 平面做透明渐变：开启混合，并避免透明区域写入深度导致遮挡
            f->glEnable(GL_BLEND);
            f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            f->glDepthMask(GL_FALSE);

            planeProgram.setUniformValue("model", model);
            planeProgram.setUniformValue("mvp", mvp);
            planeProgram.setUniformValue("uCameraPos", eye);
            const QVector3D center = orbitCenter;
            // 3 个点光源：略偏上方，产生更明显高光
            planeProgram.setUniformValue("uLightPos0", center + QVector3D(+0.26f, +0.22f, 0.42f));
            planeProgram.setUniformValue("uLightPos1", center + QVector3D(-0.32f, +0.14f, 0.36f));
            planeProgram.setUniformValue("uLightPos2", center + QVector3D(+0.04f, -0.34f, 0.30f));
            planeProgram.setUniformValue("uLightCol0", QVector3D(1.00f, 0.98f, 0.95f) * 0.32f);
            planeProgram.setUniformValue("uLightCol1", QVector3D(0.78f, 0.90f, 1.05f) * 0.28f);
            planeProgram.setUniformValue("uLightCol2", QVector3D(0.85f, 1.00f, 0.90f) * 0.26f);
            vaoPlane.bind();
            f->glDrawArrays(GL_TRIANGLES, 0, planeVertexCount);
            vaoPlane.release();
            planeProgram.release();

            f->glDepthMask(GL_TRUE);
            f->glDisable(GL_BLEND);
        }

        // 导入的三角网格（与热床同一 model 变换）；预览且已加载轨迹时隐藏模型
        if (meshDrawVertexCount > 0 && vaoMesh.isCreated()
            && !(m_syncedPreviewMode && m_syncedTrajNonEmpty)) {
            if (!meshProgram.bind()) {
                qWarning() << "[MeshGPU] meshProgram.bind FAILED log:" << meshProgram.log();
            } else {
                f->glEnable(GL_DEPTH_TEST);
                f->glDepthMask(GL_TRUE);
                f->glDisable(GL_CULL_FACE);
                meshProgram.setUniformValue("uUseVertexColor", 0.0f);
                meshProgram.setUniformValue("model", model);
                meshProgram.setUniformValue("mvp", mvp);
                meshProgram.setUniformValue("uNormalMat", model.normalMatrix());
                meshProgram.setUniformValue("uCameraPos", eye);
                const QVector3D center = orbitCenter;
                meshProgram.setUniformValue("uLightPos0", center + QVector3D(+0.26f, +0.22f, 0.42f));
                meshProgram.setUniformValue("uLightPos1", center + QVector3D(-0.32f, +0.14f, 0.36f));
                meshProgram.setUniformValue("uLightPos2", center + QVector3D(+0.04f, -0.34f, 0.30f));
                // 模型：灰白更亮 + 更明显光照
                meshProgram.setUniformValue("uLightCol0", QVector3D(1.00f, 0.98f, 0.95f) * 0.55f);
                meshProgram.setUniformValue("uLightCol1", QVector3D(0.78f, 0.90f, 1.05f) * 0.48f);
                meshProgram.setUniformValue("uLightCol2", QVector3D(0.85f, 1.00f, 0.90f) * 0.45f);
                meshProgram.setUniformValue("uMeshAlbedo", QVector3D(0.86f, 0.87f, 0.88f));
                meshProgram.setUniformValue("uDrawMode", 0.0f);
                vaoMesh.bind();
                if (!m_syncedDrawChunks.isEmpty()) {
                    for (const MeshDrawChunk &ch : m_syncedDrawChunks) {
                        if (ch.vertexCount <= 0 || !ch.sceneVisible)
                            continue;
                        // 应用模型位置偏移和旋转
                        QMatrix4x4 chunkModel = model;
                        chunkModel.translate(ch.positionOffset);
                        chunkModel.rotate(ch.rotation);
                        appendChunkMirrorTransform(chunkModel, ch.geomCenterLocal, ch.mirrorScale);
                        if (!qFuzzyCompare(ch.uniformScale, 1.0f)) {
                            chunkModel.translate(ch.geomCenterLocal);
                            chunkModel.scale(ch.uniformScale);
                            chunkModel.translate(-ch.geomCenterLocal);
                        }
                        const QMatrix4x4 chunkMvp = projection * view * chunkModel;
                        meshProgram.setUniformValue("model", chunkModel);
                        meshProgram.setUniformValue("mvp", chunkMvp);
                        meshProgram.setUniformValue("uNormalMat", chunkModel.normalMatrix());
                        f->glDrawArrays(GL_TRIANGLES, ch.firstVertex, ch.vertexCount);
                    }
                } else {
                    f->glDrawArrays(GL_TRIANGLES, 0, meshDrawVertexCount);
                }

                bool anyAct = false;
                for (const MeshDrawChunk &ch : m_syncedDrawChunks) {
                    if (ch.vertexCount <= 0 || !ch.sceneVisible)
                        continue;
                    if (ch.active)
                        anyAct = true;
                }
                if (anyAct) {
                    f->glEnable(GL_BLEND);
                    f->glBlendFunc(GL_ONE, GL_ONE);
                    f->glBlendEquation(GL_FUNC_ADD);
                    // 叠加高亮会重复绘制同一几何，深度值通常与首遍相等；
                    // 默认 GL_LESS 会导致第二遍全部被深度测试丢弃，从而看不到高亮。
                    f->glDepthFunc(GL_LEQUAL);
                    f->glDepthMask(GL_FALSE);
                    meshProgram.setUniformValue("uDrawMode", 2.0f);
                    for (const MeshDrawChunk &ch : m_syncedDrawChunks) {
                        if (!ch.active || ch.vertexCount <= 0 || !ch.sceneVisible)
                            continue;
                        QMatrix4x4 chunkModel = model;
                        chunkModel.translate(ch.positionOffset);
                        chunkModel.rotate(ch.rotation);
                        appendChunkMirrorTransform(chunkModel, ch.geomCenterLocal, ch.mirrorScale);
                        if (!qFuzzyCompare(ch.uniformScale, 1.0f)) {
                            chunkModel.translate(ch.geomCenterLocal);
                            chunkModel.scale(ch.uniformScale);
                            chunkModel.translate(-ch.geomCenterLocal);
                        }
                        const QMatrix4x4 chunkMvp = projection * view * chunkModel;
                        meshProgram.setUniformValue("model", chunkModel);
                        meshProgram.setUniformValue("mvp", chunkMvp);
                        meshProgram.setUniformValue("uNormalMat", chunkModel.normalMatrix());
                        f->glDrawArrays(GL_TRIANGLES, ch.firstVertex, ch.vertexCount);
                    }
                    meshProgram.setUniformValue("uDrawMode", 0.0f);
                    f->glDepthMask(GL_TRUE);
                    f->glDepthFunc(GL_LESS);
                    f->glDisable(GL_BLEND);
                }

                vaoMesh.release();
                meshProgram.release();
                const GLenum gle = f->glGetError();
                if (gle != GL_NO_ERROR)
                    qWarning() << "[MeshGPU] glGetError after mesh draw:" << Qt::hex << gle;
            }
        } else if (m_syncedMeshVersion > 0 && meshDrawVertexCount <= 0) {
            static int s_warnEmpty = 0;
            if (s_warnEmpty++ < 5)
                qWarning() << "[MeshGPU] have mesh version" << m_syncedMeshVersion << "but meshDrawVertexCount=0"
                           << "vaoMesh.created=" << vaoMesh.isCreated();
        }

        if (m_trajDirty && vboTraj.isCreated()) {
            vboTraj.bind();
            if (m_trajInterleaved.isEmpty())
                vboTraj.allocate(nullptr, 0);
            else
                vboTraj.allocate(m_trajInterleaved.constData(),
                                 int(m_trajInterleaved.size() * sizeof(float)));
            vboTraj.release();
            trajVertexCount = int(m_trajInterleaved.size() / 6);
            m_trajDirty = false;
        }

        if (m_syncedPreviewMode && trajVertexCount > 0 && vaoTraj.isCreated() && trajProgram.bind()) {
            trajProgram.setUniformValue("mvp", mvp);
            vaoTraj.bind();
            f->glLineWidth(1.25f);
            f->glDrawArrays(GL_LINES, 0, trajVertexCount);
            vaoTraj.release();
            trajProgram.release();
        }

        // 轨迹预览：当前刀位点处的垂直喷头（与导入网格同一 mesh shader / 场景 model 变换）
        if (m_syncedPreviewMode && trajVertexCount > 0 && nozzleVertexCount > 0 && vaoNozzle.isCreated()
            && meshProgram.bind()) {
            f->glEnable(GL_DEPTH_TEST);
            f->glDepthMask(GL_TRUE);
            f->glEnable(GL_CULL_FACE);
            f->glCullFace(GL_BACK);

            QMatrix4x4 nozzleModel = model;
            nozzleModel.translate(QVector3D(m_syncedTrajTipX, m_syncedTrajTipY, m_syncedTrajTipZ));
            const QMatrix4x4 nozzleMvp = projection * view * nozzleModel;

            meshProgram.setUniformValue("model", nozzleModel);
            meshProgram.setUniformValue("mvp", nozzleMvp);
            meshProgram.setUniformValue("uNormalMat", nozzleModel.normalMatrix());
            meshProgram.setUniformValue("uCameraPos", eye);
            const QVector3D center = orbitCenter;
            meshProgram.setUniformValue("uLightPos0", center + QVector3D(+0.26f, +0.22f, 0.42f));
            meshProgram.setUniformValue("uLightPos1", center + QVector3D(-0.32f, +0.14f, 0.36f));
            meshProgram.setUniformValue("uLightPos2", center + QVector3D(+0.04f, -0.34f, 0.30f));
            meshProgram.setUniformValue("uLightCol0", QVector3D(1.00f, 0.98f, 0.95f) * 0.55f);
            meshProgram.setUniformValue("uLightCol1", QVector3D(0.78f, 0.90f, 1.05f) * 0.48f);
            meshProgram.setUniformValue("uLightCol2", QVector3D(0.85f, 1.00f, 0.90f) * 0.45f);
            meshProgram.setUniformValue("uMeshAlbedo", QVector3D(0.86f, 0.87f, 0.88f));
            meshProgram.setUniformValue("uUseVertexColor", 1.0f);
            meshProgram.setUniformValue("uDrawMode", 0.0f);

            vaoNozzle.bind();
            f->glDrawArrays(GL_TRIANGLES, 0, nozzleVertexCount);
            vaoNozzle.release();
            meshProgram.release();

            f->glDisable(GL_CULL_FACE);
        }

        // 画原点坐标轴（带箭头）
        if (vaoAxis.isCreated() && axisProgram.bind()) {
            axisProgram.setUniformValue("mvp", mvp);
            vaoAxis.bind();
            f->glLineWidth(2.0f);
            f->glDrawArrays(GL_LINES, 0, axisLineVertexCount);
            f->glLineWidth(1.0f);
            f->glDrawArrays(GL_TRIANGLES, axisLineVertexCount, axisVertexCount - axisLineVertexCount);
            vaoAxis.release();
            axisProgram.release();
        }

        // 轴标签：billboard，笔画始终沿视平面，便于阅读
        if (vaoAxisLabel.isCreated() && axisLabelVertexCount > 0 && axisLabelProgram.bind()) {
            axisLabelProgram.setUniformValue("model", model);
            axisLabelProgram.setUniformValue("view", view);
            axisLabelProgram.setUniformValue("projection", projection);
            axisLabelProgram.setUniformValue("uCamRight", camRight);
            axisLabelProgram.setUniformValue("uCamUp", camUp);
            vaoAxisLabel.bind();
            f->glLineWidth(2.0f);
            f->glDrawArrays(GL_LINES, 0, axisLabelVertexCount);
            f->glLineWidth(1.0f);
            vaoAxisLabel.release();
            axisLabelProgram.release();
        }

        // 左下角视图立方体（与平面姿态同步；悬停高亮在 shader）
        if (viewCubeOk && vaoViewCube.isCreated() && viewCubeProgram.bind()) {
            GLint vpPrev[4] = {};
            f->glGetIntegerv(GL_VIEWPORT, vpPrev);
            const int gw = kGizmoSize;
            const int gh = kGizmoSize;
            const int vx = kGizmoMargin;
            const int vy = kGizmoMargin;
            f->glViewport(vx, vy, gw, gh);
            // enable depth test for cube faces so edges behind faces are occluded
            f->glEnable(GL_DEPTH_TEST);
            f->glDepthMask(GL_TRUE);
            f->glDisable(GL_BLEND);
            // use polygon offset on faces to avoid z-fighting when drawing edge lines on top
            f->glEnable(GL_POLYGON_OFFSET_FILL);
            f->glPolygonOffset(1.0f, 1.0f);

            const QMatrix4x4 P = viewCubeProj(gw, gh);
            const QMatrix4x4 V = viewCubeView(m_yawDeg, m_pitchDeg, m_syncedLookAtUp);
            const QMatrix4x4 M = viewCubeModel(m_planeOrientation);
            const QMatrix4x4 mvpGizmo = P * V * M;
            viewCubeProgram.setUniformValue("uMvp", mvpGizmo);
            viewCubeProgram.setUniformValue("uRot", M.normalMatrix());
            viewCubeProgram.setUniformValue("uHoverFace", m_hoverViewCubeFace);
            viewCubeProgram.setUniformValue("uLightDirWorld",
                                            QVector3D(0.28f, 0.45f, 0.84f).normalized());
            if (viewCubeAtlas) {
                viewCubeAtlas->bind(0);
                viewCubeProgram.setUniformValue("uAtlas", 0);
            }
            // 实心立方体：剔除背面（前提：6 个面绕序已统一为外侧 CCW）
            f->glEnable(GL_CULL_FACE);
            f->glCullFace(GL_BACK);
            vaoViewCube.bind();
            f->glDrawArrays(GL_TRIANGLES, 0, viewCubeVertexCount);
            vaoViewCube.release();
            viewCubeProgram.release();
            // disable polygon offset after drawing faces
            f->glDisable(GL_POLYGON_OFFSET_FILL);

            if (viewCubeAtlas) {
                viewCubeAtlas->release();
            }

            // draw cube edges (black) on top
            if (viewCubeEdgeVertexCount > 0 && vaoViewCubeEdges.isCreated() && axisProgram.bind()) {
                axisProgram.setUniformValue("mvp", mvpGizmo);
                vaoViewCubeEdges.bind();
                f->glLineWidth(2.0f);
                f->glDrawArrays(GL_LINES, 0, viewCubeEdgeVertexCount);
                f->glLineWidth(1.0f);
                vaoViewCubeEdges.release();
                axisProgram.release();
            }

            // 立方体坐标轴：原点为立方体顶点 (-0.5,-0.5,-0.5)，+X/+Y/+Z 沿三条棱；
            // 与立方体同一旋转。该角在默认相机下多在立方体背面，线段会被深度缓冲挡掉，故绘制轴时关闭深度测试。
            if (vaoViewCubeAxis.isCreated() && axisProgram.bind()) {
                const QVector3D cornerLocal(-0.5f, -0.5f, -0.5f);
                QMatrix4x4 modelAxes = viewCubeModel(m_planeOrientation);
                modelAxes.translate(cornerLocal);
                const QMatrix4x4 mvpAxes = P * V * modelAxes;
                axisProgram.setUniformValue("mvp", mvpAxes);
                f->glDisable(GL_DEPTH_TEST);
                vaoViewCubeAxis.bind();
                f->glLineWidth(2.0f);
                f->glDrawArrays(GL_LINES, 0, viewCubeAxisLineVertexCount);
                f->glLineWidth(1.0f);
                f->glDrawArrays(GL_TRIANGLES, viewCubeAxisLineVertexCount,
                                viewCubeAxisVertexCount - viewCubeAxisLineVertexCount);
                vaoViewCubeAxis.release();
                axisProgram.release();
                f->glEnable(GL_DEPTH_TEST);
            }

            f->glDisable(GL_CULL_FACE);
            f->glViewport(vpPrev[0], vpPrev[1], vpPrev[2], vpPrev[3]);
            f->glEnable(GL_DEPTH_TEST);
        }

        const bool hasMeasurePaint = !m_syncedMeasureTraces.isEmpty() || m_syncedMeasureHasFirstPoint;
        if (hasMeasurePaint && fbSize.width() > 0 && fbSize.height() > 0) {
            auto projectToScreen = [&](const QVector3D &world, QPointF &out) -> bool {
                const QVector4D clip = projection * view * QVector4D(world, 1.0f);
                const float w = clip.w();
                if (qAbs(w) < 1e-8f)
                    return false;
                const float nx = clip.x() / w;
                const float ny = clip.y() / w;
                out.setX((nx * 0.5f + 0.5f) * float(fbSize.width()));
                out.setY((1.0f - (ny * 0.5f + 0.5f)) * float(fbSize.height()));
                return true;
            };

            f->glDisable(GL_DEPTH_TEST);
            QOpenGLPaintDevice paintDevice(fbSize);
            QPainter painter(&paintDevice);
            painter.setRenderHint(QPainter::Antialiasing, true);
            QFont labelFont = painter.font();
            labelFont.setPixelSize(14);
            painter.setFont(labelFont);

            for (int ti = 0; ti < m_syncedMeasureTraces.size(); ++ti) {
                const MeasureTrace &tr = m_syncedMeasureTraces.at(ti);
                QPointF p0;
                QPointF p1;
                if (!projectToScreen(tr.pointA, p0) || !projectToScreen(tr.pointB, p1))
                    continue;
                painter.setPen(QPen(QColor(236, 66, 66), 2.0));
                painter.drawLine(p0, p1);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(236, 66, 66));
                painter.drawEllipse(p0, 4.5, 4.5);
                painter.drawEllipse(p1, 4.5, 4.5);
                painter.setPen(QColor(255, 255, 255));
                const QString p0Lab = (m_syncedMeasureTraces.size() > 1)
                    ? (QStringLiteral("P0-") + QString::number(ti + 1))
                    : QStringLiteral("P0");
                const QString p1Lab = (m_syncedMeasureTraces.size() > 1)
                    ? (QStringLiteral("P1-") + QString::number(ti + 1))
                    : QStringLiteral("P1");
                painter.drawText(p0 + QPointF(7.0, -7.0), p0Lab);
                painter.drawText(p1 + QPointF(7.0, -7.0), p1Lab);
                const QPointF mid = (p0 + p1) * 0.5;
                const QString lenText = QString::number(tr.distanceMm, 'f', 2) + QStringLiteral(" mm");
                const QFontMetricsF fm(painter.font());
                const QSizeF textSize = fm.size(Qt::TextSingleLine, lenText);
                const QRectF bgRect(mid.x() - textSize.width() * 0.5 - 6.0,
                                    mid.y() - textSize.height() - 10.0,
                                    textSize.width() + 12.0,
                                    textSize.height() + 8.0);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 155));
                painter.drawRoundedRect(bgRect, 4.0, 4.0);
                painter.setPen(QColor(255, 96, 96));
                painter.drawText(bgRect, Qt::AlignCenter, lenText);
            }

            if (m_syncedMeasureHasFirstPoint) {
                QPointF p0;
                QPointF p1;
                if (projectToScreen(m_syncedMeasurePointA, p0) && projectToScreen(m_syncedMeasurePointB, p1)) {
                    painter.setPen(QPen(QColor(236, 66, 66), 2.0));
                    painter.drawLine(p0, p1);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(236, 66, 66));
                    painter.drawEllipse(p0, 4.5, 4.5);
                    painter.drawEllipse(p1, 4.5, 4.5);
                    painter.setPen(QColor(255, 255, 255));
                    painter.drawText(p0 + QPointF(7.0, -7.0), QStringLiteral("P0"));
                    painter.drawText(p1 + QPointF(7.0, -7.0), QStringLiteral("P"));
                }
            }
            painter.end();
        }

    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        qDebug() << "[FBO] Creating framebuffer object, size:" << size;
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        // 启用4x MSAA抗锯齿，使刻度线边缘更平滑
        format.setSamples(4);
        return new QOpenGLFramebufferObject(size, format);
    }

private:
    void setupPreviewNozzleGeometry(QOpenGLContext *ctx)
    {
        if (!ctx || !meshProgram.isLinked() || vboNozzle.isCreated())
            return;
        QVector<float> inter;
        appendPreviewNozzleMesh(inter);
        if (inter.isEmpty())
            return;
        QOpenGLFunctions *gf = ctx->functions();
        if (!gf)
            return;
        vboNozzle.create();
        vaoNozzle.create();
        vboNozzle.bind();
        vboNozzle.allocate(inter.constData(), int(inter.size() * int(sizeof(float))));
        vaoNozzle.bind();
        const GLsizei nstride = GLsizei(9 * int(sizeof(float)));
        gf->glEnableVertexAttribArray(0);
        gf->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, nstride, nullptr);
        gf->glEnableVertexAttribArray(1);
        gf->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, nstride,
                                  reinterpret_cast<const void *>(quintptr(3 * sizeof(float))));
        gf->glEnableVertexAttribArray(2);
        gf->glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, nstride,
                                  reinterpret_cast<const void *>(quintptr(6 * sizeof(float))));
        vaoNozzle.release();
        vboNozzle.release();
        nozzleVertexCount = int(inter.size() / 9);
        qDebug() << "[ViewportRenderer] Preview nozzle GPU verts" << nozzleVertexCount;
    }

    void uploadImportedMesh(QOpenGLFunctions *f) {
        if (!f)
            return;
        const int cpuFloats = m_meshRenderBuf && m_meshRenderBuf.get() ? m_meshRenderBuf->size() : 0;
        qInfo() << "[MeshGPU] uploadImportedMesh begin cpuFloats=" << cpuFloats;
        QElapsedTimer upT;
        upT.start();
        if (!vboMesh.isCreated())
            vboMesh.create();
        if (!vaoMesh.isCreated())
            vaoMesh.create();

        vboMesh.bind();
        if (!m_meshRenderBuf || m_meshRenderBuf->isEmpty()) {
            vboMesh.allocate(nullptr, 0);
            meshDrawVertexCount = 0;
            qInfo() << "[MeshGPU] upload empty VBO";
        } else {
            const int nFloats = m_meshRenderBuf->size();
            if (nFloats % 6 != 0)
                qWarning() << "[MeshGPU] BAD stride floats%6=" << (nFloats % 6);
            const int bytes = nFloats * int(sizeof(float));
            vboMesh.allocate(m_meshRenderBuf->constData(), bytes);
            meshDrawVertexCount = nFloats / 6;
            qInfo() << "[MeshGPU] VBO allocate bytes=" << bytes << "drawVerts=" << meshDrawVertexCount
                    << "ms=" << upT.elapsed();
        }
        vaoMesh.bind();
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * int(sizeof(float)), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * int(sizeof(float)),
                                 reinterpret_cast<const void *>(quintptr(3 * sizeof(float))));
        f->glDisableVertexAttribArray(2);
        f->glVertexAttrib3f(2, 1.0f, 1.0f, 1.0f);
        vaoMesh.release();
        vboMesh.release();
        const GLenum gle = f->glGetError();
        if (gle != GL_NO_ERROR)
            qWarning() << "[MeshGPU] glGetError after upload:" << Qt::hex << gle;

        if (meshDrawVertexCount > 0 && m_meshRenderBuf) {
            const float *d = m_meshRenderBuf->constData();
            QVector3D ubmin(1e30f, 1e30f, 1e30f), ubmax(-1e30f, -1e30f, -1e30f);
            for (int i = 0; i < meshDrawVertexCount; ++i) {
                ubmin.setX(qMin(ubmin.x(), d[i * 6 + 0]));
                ubmin.setY(qMin(ubmin.y(), d[i * 6 + 1]));
                ubmin.setZ(qMin(ubmin.z(), d[i * 6 + 2]));
                ubmax.setX(qMax(ubmax.x(), d[i * 6 + 0]));
                ubmax.setY(qMax(ubmax.y(), d[i * 6 + 1]));
                ubmax.setZ(qMax(ubmax.z(), d[i * 6 + 2]));
            }
            qInfo() << "[MeshGPU] upload done AABB min" << ubmin << "max" << ubmax << "v0" << d[0] << d[1] << d[2];
        }
    }

    void logMeshVisibilityDiagnostics(const QMatrix4x4 &mvp, const QVector3D &eye,
                                      const QVector3D &orbitCenter)
    {
        if (!m_meshRenderBuf || m_meshRenderBuf->isEmpty() || meshDrawVertexCount <= 0)
            return;
        const float *d = m_meshRenderBuf->constData();
        const int nv = meshDrawVertexCount;
        QVector3D bmin(1e30f, 1e30f, 1e30f);
        QVector3D bmax(-1e30f, -1e30f, -1e30f);
        for (int i = 0; i < nv; ++i) {
            const float x = d[i * 6 + 0], y = d[i * 6 + 1], z = d[i * 6 + 2];
            bmin.setX(qMin(bmin.x(), x));
            bmin.setY(qMin(bmin.y(), y));
            bmin.setZ(qMin(bmin.z(), z));
            bmax.setX(qMax(bmax.x(), x));
            bmax.setY(qMax(bmax.y(), y));
            bmax.setZ(qMax(bmax.z(), z));
        }
        qInfo() << "[MeshDiag] ===== version" << m_syncedMeshVersion << "drawVerts" << nv;
        qInfo() << "[MeshDiag] object AABB min" << bmin << "max" << bmax;
        qInfo() << "[MeshDiag] eye" << eye << "target" << orbitCenter;
        qInfo() << "[MeshDiag] meshProgram.isLinked" << meshProgram.isLinked();

        const QVector3D corners[8] = {
            QVector3D(bmin.x(), bmin.y(), bmin.z()), QVector3D(bmax.x(), bmin.y(), bmin.z()),
            QVector3D(bmin.x(), bmax.y(), bmin.z()), QVector3D(bmax.x(), bmax.y(), bmin.z()),
            QVector3D(bmin.x(), bmin.y(), bmax.z()), QVector3D(bmax.x(), bmin.y(), bmax.z()),
            QVector3D(bmin.x(), bmax.y(), bmax.z()), QVector3D(bmax.x(), bmax.y(), bmax.z()),
        };
        int inNdc = 0;
        for (int c = 0; c < 8; ++c) {
            const QVector4D cl = mvp * QVector4D(corners[c], 1.0f);
            const float w = cl.w();
            if (qAbs(w) < 1e-8f) {
                qInfo() << "[MeshDiag] corner" << c << "degenerate clip" << cl;
                continue;
            }
            const float nx = cl.x() / w;
            const float ny = cl.y() / w;
            const float nz = cl.z() / w;
            if (nx >= -1.2f && nx <= 1.2f && ny >= -1.2f && ny <= 1.2f && nz >= -1.2f && nz <= 1.2f)
                ++inNdc;
            if (c < 4)
                qInfo() << "[MeshDiag] corner" << c << "obj" << corners[c] << "clip" << cl << "ndc" << nx << ny << nz;
        }
        qInfo() << "[MeshDiag] corners in NDC cube ~[-1.2,1.2]:" << inNdc << "/8";

        for (int i = 0; i < qMin(3, nv); ++i) {
            const QVector3D p(d[i * 6], d[i * 6 + 1], d[i * 6 + 2]);
            const QVector4D cl = mvp * QVector4D(p, 1.0f);
            qInfo() << "[MeshDiag] vtx" << i << "obj" << p << "clip" << cl;
        }
        qInfo() << "[MeshDiag] =====";
    }

    void synchronize(QQuickFramebufferObject *item) override {
        auto *vp = static_cast<OpenGLViewport *>(item);
        m_yawDeg = vp->yawDegrees();
        m_pitchDeg = vp->pitchDegrees();
        m_distance = vp->distance();
        m_planeOrientation = vp->planeOrientation();
        m_hoverViewCubeFace = vp->hoverViewCubeFace();
        m_orbitTarget = vp->orbitTarget();
        m_syncedLookAtUp = vp->lookAtUpWorld();

        m_syncedMeasureHasFirstPoint = vp->m_measureHasFirstPoint;
        m_syncedMeasurePointA = vp->m_measurePointA;
        m_syncedMeasurePointB = vp->m_measurePointB;
        m_syncedMeasureTraces = vp->m_measureTraces;

        const quint64 ver = vp->m_meshDataVersion;
        if (ver != m_syncedMeshVersion) {
            m_syncedMeshVersion = ver;
            // 只拷贝 shared_ptr，不对 QVector 做深拷贝（大 STL 否则主线程卡数秒）
            m_meshRenderBuf = vp->m_importedMeshBuf;
            m_meshGpuDirty = true;
            const int nf = m_meshRenderBuf && m_meshRenderBuf.get() ? m_meshRenderBuf->size() : 0;
            qInfo() << "[MeshGPU] synchronize mesh version=" << ver << "cpuFloats=" << nf
                    << "expectedVerts=" << (nf / 6);
            qInfo() << "[MeshGPU] sync vp->m_importedMeshBuf.get()="
                    << reinterpret_cast<const void *>(vp->m_importedMeshBuf.get())
                    << "use_count=" << (vp->m_importedMeshBuf ? vp->m_importedMeshBuf.use_count() : 0)
                    << "renderer buf.get()=" << reinterpret_cast<const void *>(m_meshRenderBuf.get());
        }

        // 每帧同步分块与勾选状态（数据量小，避免仅 bump 版本时与渲染线程不同步）
        m_syncedDrawChunks.clear();
        m_syncedDrawChunks.reserve(vp->m_meshChunks.size());
        for (int i = 0; i < vp->m_meshChunks.size(); ++i) {
            const auto &c = vp->m_meshChunks[i];
            MeshDrawChunk d;
            d.firstVertex = c.firstVertex;
            d.vertexCount = c.vertexCount;
            d.active = c.active;
            d.sceneVisible = c.sceneVisible;
            d.positionOffset = c.positionOffset;
            d.rotation = c.rotation;
            d.mirrorScale = c.mirrorScale;
            d.geomCenterLocal = c.geomCenterLocal;
            d.uniformScale = c.uniformScale;
            m_syncedDrawChunks.push_back(d);
        }

        m_syncedPreviewMode = vp->m_previewMode;
        m_syncedTrajNonEmpty = !vp->m_trajSegments.isEmpty();
        m_syncedTrajTipX = vp->m_trajTipX;
        m_syncedTrajTipY = vp->m_trajTipY;
        m_syncedTrajTipZ = vp->m_trajTipZ;

        const quint64 pv = vp->m_trajPathVersion;
        const qreal pr = vp->m_trajProgress;
        const int dl = vp->m_trajDisplayLayer;
        const bool st = vp->m_previewShowTravel;
        if (pv != m_lastTrajPathVersion || qAbs(pr - m_lastTrajProgress) > 1e-10 || dl != m_lastTrajDisplayLayer
            || st != m_lastTrajShowTravel) {
            m_lastTrajPathVersion = pv;
            m_lastTrajProgress = pr;
            m_lastTrajDisplayLayer = dl;
            m_lastTrajShowTravel = st;
            buildTrajectoryInterleaved(vp->m_trajSegments, pr, dl, vp->m_trajLayerZs, vp->m_trajCumTimeSec, st,
                                       m_trajInterleaved);
            m_trajDirty = true;
        }
    }

    QOpenGLShaderProgram planeProgram;
    QOpenGLShaderProgram meshProgram;
    QOpenGLShaderProgram trajProgram;
    QOpenGLShaderProgram axisProgram;
    QOpenGLShaderProgram axisLabelProgram;
    QOpenGLShaderProgram bgProgram;
    QOpenGLShaderProgram viewCubeProgram;
    QOpenGLBuffer vboPlane, vboGrid, vboAxis, vboAxisLabel, vboViewCube, vboMesh, vboTraj, vboNozzle;
    QOpenGLVertexArrayObject vaoPlane, vaoGrid, vaoAxis, vaoAxisLabel, vaoViewCube, vaoMesh, vaoTraj,
        vaoNozzle;
    QOpenGLBuffer vboViewCubeAxis;
    QOpenGLVertexArrayObject vaoViewCubeAxis;
    QOpenGLBuffer vboViewCubeEdges;
    QOpenGLVertexArrayObject vaoViewCubeEdges;
    QOpenGLTexture *bgTexture = nullptr;
    QOpenGLTexture *viewCubeAtlas = nullptr;
    // Logo
    QOpenGLShaderProgram logoProgram;
    QOpenGLBuffer vboLogo;
    QOpenGLVertexArrayObject vaoLogo;
    QOpenGLTexture *logoTexture = nullptr;
    int logoVertexCount = 0;
    int viewCubeVertexCount = 0;
    int viewCubeEdgeVertexCount = 0;
    int viewCubeAxisVertexCount = 0;
    int viewCubeAxisLineVertexCount = 0;
    bool viewCubeOk = false;
    QOpenGLBuffer vboBg;
    QOpenGLVertexArrayObject vaoBg;
    int planeVertexCount = 0;
    int gridVertexCount = 0;
    int axisVertexCount = 0;
    int axisLineVertexCount = 0;
    int axisLabelVertexCount = 0;
    int bgVertexCount = 0;
    bool initialized = false;
    bool loggedFirstFrame = false;
    QElapsedTimer timer;

    float m_yawDeg = 180.0f;
    float m_pitchDeg = 30.0f;
    float m_distance = 1.35f;
    QQuaternion m_planeOrientation;
    QVector3D m_orbitTarget{kBedCenterXY, kBedCenterXY, 0.0f};
    QVector3D m_syncedLookAtUp{0.0f, 0.0f, 1.0f};
    int m_hoverViewCubeFace = -1;

    quint64 m_syncedMeshVersion = 0;
    quint64 m_meshDiagVersion = 0;
    QVector<MeshDrawChunk> m_syncedDrawChunks;
    std::shared_ptr<QVector<float>> m_meshRenderBuf;
    bool m_meshGpuDirty = false;
    int meshDrawVertexCount = 0;
    bool m_syncedMeasureHasFirstPoint = false;
    QVector3D m_syncedMeasurePointA;
    QVector3D m_syncedMeasurePointB;
    QVector<MeasureTrace> m_syncedMeasureTraces;

    bool m_syncedPreviewMode = false;
    bool m_syncedTrajNonEmpty = false;
    QVector<float> m_trajInterleaved;
    bool m_trajDirty = true;
    quint64 m_lastTrajPathVersion = std::numeric_limits<quint64>::max();
    qreal m_lastTrajProgress = -1.0;
    int m_lastTrajDisplayLayer = std::numeric_limits<int>::min();
    bool m_lastTrajShowTravel = true;
    int trajVertexCount = 0;
    int nozzleVertexCount = 0;
    float m_syncedTrajTipX = 0.f;
    float m_syncedTrajTipY = 0.f;
    float m_syncedTrajTipZ = 0.f;
};

OpenGLViewport::OpenGLViewport(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    qDebug() << "[OpenGLViewport] Constructor called";
    setMirrorVertically(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    // face snap animation timer
    m_faceAnimTimer = new QTimer(this);
    m_faceAnimTimer->setInterval(16);
    connect(m_faceAnimTimer, &QTimer::timeout, this, [this]() { this->faceAnimStep(); });
}

void OpenGLViewport::componentComplete()
{
    QQuickFramebufferObject::componentComplete();
    emit undoAvailableChanged();
    emit redoAvailableChanged();
}

QQuickFramebufferObject::Renderer *OpenGLViewport::createRenderer() const
{
    qDebug() << "[OpenGLViewport] createRenderer called";
    return new ViewportRenderer();
}

QVector3D OpenGLViewport::orbitTarget() const
{
    return QVector3D(kBedCenterXY, kBedCenterXY, 0.0f) + m_viewPanWorld;
}

bool OpenGLViewport::canPushUndoSnapshot() const
{
    return !m_undoPostRestoreSuppress && !m_undoRedoApplying && !m_meshExportAsyncBusy
        && !m_gcodeImportInProgress && !m_importInProgress;
}

OpenGLViewport::UndoWorkspaceState OpenGLViewport::captureUndoWorkspaceState() const
{
    UndoWorkspaceState s;
    if (m_importedMeshBuf && !m_importedMeshBuf->isEmpty())
        s.importedMeshBuf = std::make_shared<QVector<float>>(*m_importedMeshBuf);
    s.importedMeshVertexCount = m_importedMeshVertexCount;
    s.meshChunks = m_meshChunks;
    s.yawDeg = m_yawDeg;
    s.pitchDeg = m_pitchDeg;
    s.distance = m_distance;
    s.lookAtUpWorld = m_lookAtUpWorld;
    s.planeOrientation = m_planeOrientation;
    s.viewPanWorld = m_viewPanWorld;
    s.previewMode = m_previewMode;
    s.smartSelectEnabled = m_smartSelectEnabled;
    s.interactionTool = m_interactionTool;
    s.previewShowTravel = m_previewShowTravel;
    s.trajSegments = m_trajSegments;
    s.trajLayerZs = m_trajLayerZs;
    s.trajSourceLines = m_trajSourceLines;
    s.trajGcodeText = m_trajGcodeText;
    s.trajSummary = m_trajSummary;
    s.trajSourceLineCount = m_trajSourceLineCount;
    s.trajProgress = m_trajProgress;
    s.trajDisplayLayer = m_trajDisplayLayer;
    s.trajPlaybackLine = m_trajPlaybackLine;
    s.measureTraces = m_measureTraces;
    s.measureHasFirstPoint = m_measureHasFirstPoint;
    s.measurePointA = m_measurePointA;
    s.measurePointB = m_measurePointB;
    s.measuredDistanceMm = m_measuredDistanceMm;
    return s;
}

void OpenGLViewport::restoreUndoWorkspaceState(const UndoWorkspaceState &st)
{
    m_undoRedoApplying = true;

    m_rotatingModel = false;
    m_rotateModelIndex = -1;
    m_rotateStartQuaternion = QQuaternion();
    m_rotateLastPos = QPointF(0, 0);
    m_rotateDragActive = false;
    m_draggingMesh = false;
    m_dragMeshIndex = -1;
    m_dragging = false;
    m_panning = false;
    unsetCursor();

    m_importedMeshBuf = st.importedMeshBuf;
    m_importedMeshVertexCount = st.importedMeshVertexCount;
    m_meshChunks = st.meshChunks;
    m_yawDeg = st.yawDeg;
    m_pitchDeg = st.pitchDeg;
    m_distance = st.distance;
    m_lookAtUpWorld = st.lookAtUpWorld;
    m_planeOrientation = st.planeOrientation;
    m_viewPanWorld = st.viewPanWorld;
    m_previewMode = st.previewMode;
    m_smartSelectEnabled = st.smartSelectEnabled;
    m_interactionTool = st.interactionTool;
    m_previewShowTravel = st.previewShowTravel;
    m_trajSegments = st.trajSegments;
    m_trajLayerZs = st.trajLayerZs;
    m_trajSourceLines = st.trajSourceLines;
    m_trajGcodeText = st.trajGcodeText;
    m_trajSummary = st.trajSummary;
    m_trajSourceLineCount = st.trajSourceLineCount;
    m_trajProgress = st.trajProgress;
    m_trajDisplayLayer = st.trajDisplayLayer;
    m_trajPlaybackLine = st.trajPlaybackLine;
    m_measureTraces = st.measureTraces;
    m_measureHasFirstPoint = st.measureHasFirstPoint;
    m_measurePointA = st.measurePointA;
    m_measurePointB = st.measurePointB;
    m_measuredDistanceMm = st.measuredDistanceMm;

    m_playbackGcodeWindowBuildInFlight = false;
    m_playbackGcodeWindowPending = false;

    ++m_meshDataVersion;
    ++m_trajPathVersion;
    rebuildTrajectoryCumulativeTime();
    recomputeTrajectoryPlaybackLine();
    requestPlaybackGcodeWindowUpdate(true);

    m_undoRedoApplying = false;
    m_undoPostRestoreSuppress = true;
    QTimer::singleShot(0, this, [this]() { m_undoPostRestoreSuppress = false; });

    emit meshModelsChanged();
    emit previewModeChanged();
    emit smartSelectEnabledChanged();
    emit interactionToolChanged();
    emit previewShowTravelChanged();
    emit measurementChanged();
    emit progressChanged();
    emit displayLayerChanged();
    emit displayLayerZInfoChanged();
    emit pathDataChanged();
    emit playbackLineChanged();
    emit playbackGcodeWindowChanged();
    emit playbackFeedChanged();
    emit playbackTipChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::pushUndoSnapshot()
{
    if (!canPushUndoSnapshot())
        return;
    m_undoStack.push_back(captureUndoWorkspaceState());
    while (m_undoStack.size() > kMaxUndoDepth)
        m_undoStack.removeFirst();
    m_redoStack.clear();
    emit undoAvailableChanged();
    emit redoAvailableChanged();
}

bool OpenGLViewport::undo()
{
    if (m_undoStack.isEmpty())
        return false;
    UndoWorkspaceState cur = captureUndoWorkspaceState();
    UndoWorkspaceState prev = std::move(m_undoStack.last());
    m_undoStack.pop_back();
    m_redoStack.push_back(std::move(cur));
    restoreUndoWorkspaceState(prev);
    emit undoAvailableChanged();
    emit redoAvailableChanged();
    return true;
}

bool OpenGLViewport::redo()
{
    if (m_redoStack.isEmpty())
        return false;
    UndoWorkspaceState cur = captureUndoWorkspaceState();
    UndoWorkspaceState nxt = std::move(m_redoStack.last());
    m_redoStack.pop_back();
    m_undoStack.push_back(std::move(cur));
    restoreUndoWorkspaceState(nxt);
    emit undoAvailableChanged();
    emit redoAvailableChanged();
    return true;
}

void OpenGLViewport::clearMeshSceneNoUndo()
{
    m_meshChunks.clear();
    m_importedMeshBuf.reset();
    m_importedMeshVertexCount = 0;
    ++m_meshDataVersion;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::syncMeshesFromSlicerModel(const Slic3r::Model* model, bool forceThroughBusy)
{
    if (!model)
        return;
    if (!forceThroughBusy && (m_importInProgress || m_meshExportAsyncBusy || m_gcodeImportInProgress)) {
        qWarning() << "[SlicerSync] skipped (viewport import/export busy)";
        return;
    }

    clearMeshSceneNoUndo();

    auto newBuf = std::make_shared<QVector<float>>();
    QVector<ImportedMeshChunk> chunks;
    int vertexOffset = 0;

    for (Slic3r::ModelObject* mo : model->objects) {
        if (!mo || !mo->has_solid_mesh())
            continue;

        Slic3r::TriangleMesh combined;
        try {
            combined = mo->mesh();
        } catch (const std::exception& ex) {
            qWarning() << "[SlicerSync] mesh() failed:" << QString::fromUtf8(mo->name.c_str()) << ex.what();
            continue;
        }

        const indexed_triangle_set& its = combined.its;
        if (its.indices.empty() || its.vertices.empty())
            continue;

        const int vertsBefore = vertexOffset;
        constexpr float kMmToM = 0.001f;
        newBuf->reserve(newBuf->size() + int(its.indices.size()) * 18);

        for (size_t fi = 0; fi < its.indices.size(); ++fi) {
            const auto& tri = its.indices[fi];
            const stl_vertex v0 = its.vertices[tri(0)];
            const stl_vertex v1 = its.vertices[tri(1)];
            const stl_vertex v2 = its.vertices[tri(2)];
            stl_vertex n = (v1 - v0).cross(v2 - v0);
            const float len = n.norm();
            if (len > 1e-20f)
                n /= len;
            else
                n = stl_vertex(0.f, 0.f, 1.f);

            auto pushV = [&](const stl_vertex& p) {
                newBuf->append(p(0) * kMmToM);
                newBuf->append(p(1) * kMmToM);
                newBuf->append(p(2) * kMmToM);
                newBuf->append(n(0));
                newBuf->append(n(1));
                newBuf->append(n(2));
            };
            pushV(v0);
            pushV(v1);
            pushV(v2);
        }

        vertexOffset = int(newBuf->size() / 6);
        const int chunkVerts = vertexOffset - vertsBefore;
        if (chunkVerts <= 0)
            continue;

        ImportedMeshChunk chunk;
        chunk.name = QString::fromUtf8(mo->name.c_str());
        if (chunk.name.isEmpty())
            chunk.name = QStringLiteral("object");
        chunk.filePath = QString::fromUtf8(mo->input_file.c_str());
        chunk.firstVertex = vertsBefore;
        chunk.vertexCount = chunkVerts;
        chunk.active = false;
        chunk.sceneVisible = true;
        chunks.push_back(chunk);
    }

    for (int i = 0; i < chunks.size(); ++i)
        fillGeomCenterForChunk(chunks[i], *newBuf);

    m_meshChunks = std::move(chunks);
    m_importedMeshBuf = std::move(newBuf);
    m_importedMeshVertexCount = m_importedMeshBuf ? int(m_importedMeshBuf->size() / 6) : 0;
    ++m_meshDataVersion;
    ensureOneActiveModel();
    emit meshModelsChanged();
    update();
    if (auto* w = window())
        w->update();
}

void OpenGLViewport::deleteModelAtNoUndo(int index)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;

    const auto &deletedChunk = m_meshChunks[index];
    const int deletedFirstVertex = deletedChunk.firstVertex;
    const int deletedVertexCount = deletedChunk.vertexCount;

    m_meshChunks.removeAt(index);

    for (int i = index; i < m_meshChunks.size(); ++i)
        m_meshChunks[i].firstVertex -= deletedVertexCount;

    const int totalVerts = m_importedMeshVertexCount - deletedVertexCount;
    auto newBuf = std::make_shared<QVector<float>>();
    newBuf->reserve(totalVerts * 6);

    for (int v = 0; v < deletedFirstVertex; ++v) {
        newBuf->append((*m_importedMeshBuf)[v * 6 + 0]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 1]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 2]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 3]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 4]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 5]);
    }

    const int afterDeletedStart = deletedFirstVertex + deletedVertexCount;
    for (int v = afterDeletedStart; v < m_importedMeshVertexCount; ++v) {
        newBuf->append((*m_importedMeshBuf)[v * 6 + 0]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 1]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 2]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 3]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 4]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 5]);
    }

    m_importedMeshBuf = newBuf;
    m_importedMeshVertexCount = totalVerts;

    ++m_meshDataVersion;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::resetView()
{
    pushUndoSnapshot();
    m_yawDeg = kDefaultYawDeg;
    m_pitchDeg = kDefaultPitchDeg;
    m_distance = kDefaultDistance;
    m_planeOrientation = QQuaternion();
    m_lookAtUpWorld = QVector3D(0.0f, 0.0f, 1.0f);
    m_viewPanWorld = QVector3D(0.0f, 0.0f, 0.0f);
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setPreviewMode(bool on)
{
    if (m_previewMode == on)
        return;
    pushUndoSnapshot();
    m_previewMode = on;
    emit previewModeChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setPreviewShowTravel(bool on)
{
    if (m_previewShowTravel == on)
        return;
    pushUndoSnapshot();
    m_previewShowTravel = on;
    emit previewShowTravelChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setInteractionTool(int tool)
{
    const int t = qBound(0, tool, 4);
    if (m_interactionTool == t)
        return;
    m_interactionTool = t;
    if (m_interactionTool != 1) {
        m_rotateDragActive = false;
        if (m_rotatingModel)
            stopRotateModel();
    }
    if (m_interactionTool != 2)
        m_measureHasFirstPoint = false;
    emit interactionToolChanged();
}

void OpenGLViewport::fillGeomCenterForChunk(ImportedMeshChunk &chunk, const QVector<float> &vertices)
{
    if (chunk.vertexCount <= 0)
        return;
    const int v0 = chunk.firstVertex;
    const int v1 = v0 + chunk.vertexCount;
    if (v0 < 0 || v1 * 6 > vertices.size())
        return;
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float minZ = std::numeric_limits<float>::max();
    float maxX = -std::numeric_limits<float>::max();
    float maxY = -std::numeric_limits<float>::max();
    float maxZ = -std::numeric_limits<float>::max();
    for (int v = v0; v < v1; ++v) {
        minX = qMin(minX, vertices[v * 6 + 0]);
        minY = qMin(minY, vertices[v * 6 + 1]);
        minZ = qMin(minZ, vertices[v * 6 + 2]);
        maxX = qMax(maxX, vertices[v * 6 + 0]);
        maxY = qMax(maxY, vertices[v * 6 + 1]);
        maxZ = qMax(maxZ, vertices[v * 6 + 2]);
    }
    chunk.geomCenterLocal = QVector3D((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f);
}

void OpenGLViewport::clearMeasurementTraces()
{
    if (m_measureTraces.isEmpty() && !m_measureHasFirstPoint)
        return;
    pushUndoSnapshot();
    m_measureTraces.clear();
    m_measureHasFirstPoint = false;
    m_measuredDistanceMm = 0.0;
    emit measurementChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::mirrorModelAt(int meshIndex, int localAxis)
{
    if (meshIndex < 0 || meshIndex >= m_meshChunks.size())
        return;
    pushUndoSnapshot();
    if (m_importedMeshBuf && !m_importedMeshBuf->isEmpty())
        fillGeomCenterForChunk(m_meshChunks[meshIndex], *m_importedMeshBuf);
    int ax = localAxis;
    if (ax < 0 || ax > 2)
        ax = 0;
    ImportedMeshChunk &ch = m_meshChunks[meshIndex];
    if (ax == 0)
        ch.mirrorScale.setX(-ch.mirrorScale.x());
    else if (ax == 1)
        ch.mirrorScale.setY(-ch.mirrorScale.y());
    else
        ch.mirrorScale.setZ(-ch.mirrorScale.z());
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

QMatrix4x4 OpenGLViewport::meshChunkWorldMatrix(int meshIndex) const
{
    const ImportedMeshChunk &chunk = m_meshChunks.at(meshIndex);
    const QVector3D orbitCenter = orbitTarget();
    QMatrix4x4 sceneModel;
    sceneModel.translate(orbitCenter);
    sceneModel.rotate(m_planeOrientation);
    sceneModel.translate(-orbitCenter);
    QMatrix4x4 m = sceneModel;
    m.translate(chunk.positionOffset);
    m.rotate(chunk.rotation);
    appendChunkMirrorTransform(m, chunk.geomCenterLocal, chunk.mirrorScale);
    if (!qFuzzyCompare(chunk.uniformScale, 1.0f)) {
        m.translate(chunk.geomCenterLocal);
        m.scale(chunk.uniformScale);
        m.translate(-chunk.geomCenterLocal);
    }
    return m;
}

bool OpenGLViewport::gatherMeshWorldTriangles(int meshIndex, QVector<QVector3D> &outTriangleVertices) const
{
    outTriangleVertices.clear();
    if (!m_importedMeshBuf || m_importedMeshBuf->isEmpty())
        return false;
    if (meshIndex < 0 || meshIndex >= m_meshChunks.size())
        return false;
    const ImportedMeshChunk &chunk = m_meshChunks.at(meshIndex);
    if (chunk.vertexCount < 3 || (chunk.vertexCount % 3) != 0)
        return false;
    const int v0 = chunk.firstVertex;
    const int v1 = v0 + chunk.vertexCount;
    if (v0 < 0 || v1 * 6 > m_importedMeshBuf->size())
        return false;
    const QMatrix4x4 M = meshChunkWorldMatrix(meshIndex);
    const float *d = m_importedMeshBuf->constData();
    outTriangleVertices.reserve(chunk.vertexCount);
    for (int vi = v0; vi < v1; ++vi) {
        const QVector4D pl(d[vi * 6 + 0], d[vi * 6 + 1], d[vi * 6 + 2], 1.0f);
        const QVector4D pw = M * pl;
        if (qAbs(pw.w()) < 1e-20f)
            return false;
        outTriangleVertices.append((pw / pw.w()).toVector3D());
    }
    return true;
}

QString OpenGLViewport::sanitizedExportBaseName(const QString &fileOrMeshName)
{
    QString base = QFileInfo(fileOrMeshName).completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("mesh");
    static const QRegularExpression re(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
    base.replace(re, QStringLiteral("_"));
    base = base.trimmed();
    if (base.isEmpty())
        base = QStringLiteral("mesh");
    return base;
}

bool OpenGLViewport::writeTriangleMeshStlBinary(const QString &path, const QVector<QVector3D> &triVerts,
                                                QString *errMsg)
{
    const int n = triVerts.size();
    if (n < 3 || (n % 3) != 0) {
        if (errMsg)
            *errMsg = QStringLiteral("无效的三角网格");
        return false;
    }
    const quint32 triCount = quint32(n / 3);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errMsg)
            *errMsg = f.errorString();
        return false;
    }
    {
        char header[80];
        std::memset(header, 0, sizeof(header));
        const QByteArray tag = QByteArrayLiteral("HuafuSlicer binary STL");
        std::memcpy(header, tag.constData(), qMin(size_t(80), size_t(tag.size())));
        if (f.write(header, 80) != 80) {
            if (errMsg)
                *errMsg = QStringLiteral("写入 STL 头失败");
            return false;
        }
    }
    {
        const quint32 leCount = qToLittleEndian(triCount);
        if (f.write(reinterpret_cast<const char *>(&leCount), 4) != 4) {
            if (errMsg)
                *errMsg = QStringLiteral("写入 STL 面数失败");
            return false;
        }
    }
    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
    for (int t = 0; t < n; t += 3) {
        const QVector3D &p0 = triVerts[t];
        const QVector3D &p1 = triVerts[t + 1];
        const QVector3D &p2 = triVerts[t + 2];
        QVector3D nr = QVector3D::crossProduct(p1 - p0, p2 - p0);
        if (nr.lengthSquared() > 1e-30f)
            nr.normalize();
        else
            nr = QVector3D(0.0f, 0.0f, 1.0f);
        ds << nr.x() << nr.y() << nr.z();
        ds << p0.x() << p0.y() << p0.z();
        ds << p1.x() << p1.y() << p1.z();
        ds << p2.x() << p2.y() << p2.z();
        ds << quint16(0);
    }
    if (ds.status() != QDataStream::Ok) {
        if (errMsg)
            *errMsg = QStringLiteral("写入 STL 数据失败");
        return false;
    }
    f.close();
    return true;
}

bool OpenGLViewport::writeTriangleMeshStlAscii(const QString &path, const QString &solidName,
                                               const QVector<QVector3D> &triVerts, QString *errMsg)
{
    const int n = triVerts.size();
    if (n < 3 || (n % 3) != 0) {
        if (errMsg)
            *errMsg = QStringLiteral("无效的三角网格");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errMsg)
            *errMsg = f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    QString solid = solidName;
    solid.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (solid.isEmpty())
        solid = QStringLiteral("exported");
    ts << "solid " << solid << "\n";
    for (int t = 0; t < n; t += 3) {
        const QVector3D &p0 = triVerts[t];
        const QVector3D &p1 = triVerts[t + 1];
        const QVector3D &p2 = triVerts[t + 2];
        QVector3D nr = QVector3D::crossProduct(p1 - p0, p2 - p0);
        if (nr.lengthSquared() > 1e-30f)
            nr.normalize();
        else
            nr = QVector3D(0.0f, 0.0f, 1.0f);
        ts << "facet normal " << QString::number(nr.x(), 'e', 9) << ' ' << QString::number(nr.y(), 'e', 9) << ' '
           << QString::number(nr.z(), 'e', 9) << "\n";
        ts << " outer loop\n";
        ts << "  vertex " << QString::number(p0.x(), 'e', 9) << ' ' << QString::number(p0.y(), 'e', 9) << ' '
           << QString::number(p0.z(), 'e', 9) << "\n";
        ts << "  vertex " << QString::number(p1.x(), 'e', 9) << ' ' << QString::number(p1.y(), 'e', 9) << ' '
           << QString::number(p1.z(), 'e', 9) << "\n";
        ts << "  vertex " << QString::number(p2.x(), 'e', 9) << ' ' << QString::number(p2.y(), 'e', 9) << ' '
           << QString::number(p2.z(), 'e', 9) << "\n";
        ts << " endloop\n";
        ts << "endfacet\n";
    }
    ts << "endsolid " << solid << "\n";
    f.close();
    return true;
}

bool OpenGLViewport::writeTriangleMeshObj(const QString &path, const QVector<QVector3D> &triVerts, QString *errMsg)
{
    const int n = triVerts.size();
    if (n < 3 || (n % 3) != 0) {
        if (errMsg)
            *errMsg = QStringLiteral("无效的三角网格");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errMsg)
            *errMsg = f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    // MeshLoader::loadObj 将文件顶点按「常见 Y-up OBJ」用 mapObjVertex(v)=(vx,vz,vy) 映射到场景 Z-up。
    // 故导出时必须写入 F=(px,pz,py)，使 mapObjVertex(F) 还原为世界坐标 p=(px,py,pz)。
    ts << "# Exported by HuafuSlicer (scene Z-up vertices stored permuted for import Y-up mapping)\n";
    int idx = 1;
    for (int t = 0; t < n; t += 3) {
        for (int k = 0; k < 3; ++k) {
            const QVector3D &p = triVerts[t + k];
            const float fx = p.x();
            const float fy = p.z();
            const float fz = p.y();
            ts << "v " << QString::number(fx, 'g', 12) << ' ' << QString::number(fy, 'g', 12) << ' '
               << QString::number(fz, 'g', 12) << "\n";
        }
        ts << "f " << idx << ' ' << (idx + 1) << ' ' << (idx + 2) << "\n";
        idx += 3;
    }
    f.close();
    return true;
}

namespace {

/** 根据磁盘上已有 STL 判断写入时应使用二进制(0)还是 ASCII(1) */
int stlDiskFormatHint(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const qint64 sz = f.size();
    if (sz >= 84) {
        f.seek(80);
        const QByteArray c = f.read(4);
        if (c.size() == 4) {
            quint32 tris = 0;
            std::memcpy(&tris, c.constData(), 4);
            tris = qFromLittleEndian(tris);
            if (tris > 0) {
                const qint64 expected = qint64(tris) * 50 + 84;
                if (sz == expected)
                    return 0;
            }
        }
    }
    f.seek(0);
    const QByteArray head = f.read(256).toLower();
    if (head.startsWith("solid"))
        return 1;
    return 0;
}

} // namespace

bool OpenGLViewport::exportMeshToFile(int meshIndex, const QUrl &fileUrl, int format)
{
    if (meshIndex < 0 || meshIndex >= m_meshChunks.size())
        return false;
    QString path = fileUrl.toLocalFile();
    if (path.isEmpty())
        return false;
    {
        QFileInfo fi(path);
        const QString dirPath = fi.path();
        const QString base = fi.completeBaseName();
        if (format == 2)
            path = QDir(dirPath).filePath(base + QStringLiteral(".obj"));
        else
            path = QDir(dirPath).filePath(base + QStringLiteral(".stl"));
    }
    QVector<QVector3D> tri;
    if (!gatherMeshWorldTriangles(meshIndex, tri))
        return false;
    QString err;
    const QString solidName = sanitizedExportBaseName(m_meshChunks.at(meshIndex).name);
    bool ok = false;
    if (format == 2)
        ok = writeTriangleMeshObj(path, tri, &err);
    else if (format == 1)
        ok = writeTriangleMeshStlAscii(path, solidName, tri, &err);
    else
        ok = writeTriangleMeshStlBinary(path, tri, &err);
    if (ok) {
        ImportedMeshChunk &ch = m_meshChunks[meshIndex];
        ch.filePath = path;
        ch.name = QFileInfo(path).fileName();
        emit meshModelsChanged();
    }
    return ok;
}

bool OpenGLViewport::meshHasPersistentStorage(int meshIndex) const
{
    if (meshIndex < 0 || meshIndex >= m_meshChunks.size())
        return false;
    const QString orig = m_meshChunks.at(meshIndex).filePath.trimmed();
    if (orig.isEmpty() || !QFile::exists(orig))
        return false;
    const QString suf = QFileInfo(orig).suffix().toLower();
    return suf == QLatin1String("stl") || suf == QLatin1String("obj");
}

bool OpenGLViewport::saveMeshInPlace(int meshIndex)
{
    if (!meshHasPersistentStorage(meshIndex))
        return false;
    const QString orig = m_meshChunks.at(meshIndex).filePath.trimmed();
    const QString suf = QFileInfo(orig).suffix().toLower();
    int fmt = 0;
    if (suf == QLatin1String("obj"))
        fmt = 2;
    else if (suf == QLatin1String("stl"))
        fmt = stlDiskFormatHint(orig);
    else
        return false;
    return exportMeshToFile(meshIndex, QUrl::fromLocalFile(orig), fmt);
}

bool OpenGLViewport::exportMeshQuickSave(int meshIndex, int format)
{
    if (meshIndex < 0 || meshIndex >= m_meshChunks.size())
        return false;
    int fmt = format;
    if (fmt < 0 || fmt > 2)
        fmt = 0;
    const ImportedMeshChunk &c = m_meshChunks.at(meshIndex);
    QFileInfo fi(c.filePath);
    QString dir = fi.absolutePath();
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString base = sanitizedExportBaseName(fi.completeBaseName().isEmpty() ? c.name : fi.fileName());
    QString suffix;
    if (fmt == 2)
        suffix = QStringLiteral("_saved.obj");
    else if (fmt == 1)
        suffix = QStringLiteral("_saved_ascii.stl");
    else
        suffix = QStringLiteral("_saved.stl");
    const QString path = QDir(dir).filePath(base + suffix);
    return exportMeshToFile(meshIndex, QUrl::fromLocalFile(path), fmt);
}

bool OpenGLViewport::exportAllMeshesToFolder(const QUrl &folderUrl, int format)
{
    const QString dirPath = folderUrl.toLocalFile();
    if (dirPath.isEmpty())
        return false;
    QDir dir(dirPath);
    if (!dir.exists())
        return false;
    if (m_meshChunks.isEmpty())
        return false;
    for (int i = 0; i < m_meshChunks.size(); ++i) {
        if (!m_meshChunks[i].sceneVisible)
            continue;
        const QString ext = (format == 2) ? QStringLiteral(".obj") : QStringLiteral(".stl");
        QString base = sanitizedExportBaseName(m_meshChunks[i].name);
        QString path = dir.filePath(base + ext);
        int suffix = 0;
        while (QFile::exists(path)) {
            ++suffix;
            path = dir.filePath(base + QStringLiteral("_") + QString::number(suffix) + ext);
        }
        if (!exportMeshToFile(i, QUrl::fromLocalFile(path), format))
            return false;
    }
    return true;
}

bool OpenGLViewport::tryBuildMeshExportJob(int meshIndex, const QUrl &fileUrl, int format,
                                           MeshExportCpuJob &out, QString *errMsg) const
{
    auto fail = [&](const QString &s) -> bool {
        if (errMsg)
            *errMsg = s;
        return false;
    };
    out = MeshExportCpuJob{};
    out.meshIndex = meshIndex;
    out.format = format;
    if (meshIndex < 0 || meshIndex >= m_meshChunks.size())
        return fail(tr("无效的模型索引"));
    QString path = fileUrl.toLocalFile();
    if (path.isEmpty())
        return fail(tr("无效的文件路径"));
    {
        QFileInfo fi(path);
        const QString dirPath = fi.path();
        const QString base = fi.completeBaseName();
        if (format == 2)
            path = QDir(dirPath).filePath(base + QStringLiteral(".obj"));
        else
            path = QDir(dirPath).filePath(base + QStringLiteral(".stl"));
    }
    out.outputPath = path;
    if (!m_importedMeshBuf || m_importedMeshBuf->isEmpty())
        return fail(tr("没有可用的网格数据"));
    const ImportedMeshChunk &chunk = m_meshChunks.at(meshIndex);
    if (chunk.vertexCount < 3 || (chunk.vertexCount % 3) != 0)
        return fail(tr("网格数据不完整"));
    const int v0 = chunk.firstVertex;
    const int v1 = v0 + chunk.vertexCount;
    if (v0 < 0 || v1 * 6 > m_importedMeshBuf->size())
        return fail(tr("顶点范围无效"));
    out.solidName = sanitizedExportBaseName(m_meshChunks.at(meshIndex).name);
    out.worldM = meshChunkWorldMatrix(meshIndex);
    out.localVerts.resize(chunk.vertexCount * 6);
    std::memcpy(out.localVerts.data(), m_importedMeshBuf->constData() + size_t(v0) * 6 * sizeof(float),
                size_t(out.localVerts.size()) * sizeof(float));
    return true;
}

bool OpenGLViewport::transformJobToFile(const MeshExportCpuJob &job, QString *errMsg)
{
    const int nFloat = job.localVerts.size();
    if (nFloat < 18 || (nFloat % 6) != 0) {
        if (errMsg)
            *errMsg = QStringLiteral("invalid triangle mesh");
        return false;
    }
    const int nVerts = nFloat / 6;
    QVector<QVector3D> tri;
    tri.reserve(nVerts);
    const QMatrix4x4 &M = job.worldM;
    const float *d = job.localVerts.constData();
    for (int vi = 0; vi < nVerts; ++vi) {
        const QVector4D pl(d[vi * 6 + 0], d[vi * 6 + 1], d[vi * 6 + 2], 1.0f);
        const QVector4D pw = M * pl;
        if (qAbs(pw.w()) < 1e-20f) {
            if (errMsg)
                *errMsg = QStringLiteral("transform singular");
            return false;
        }
        tri.append((pw / pw.w()).toVector3D());
    }
    if (job.format == 2)
        return writeTriangleMeshObj(job.outputPath, tri, errMsg);
    if (job.format == 1)
        return writeTriangleMeshStlAscii(job.outputPath, job.solidName, tri, errMsg);
    return writeTriangleMeshStlBinary(job.outputPath, tri, errMsg);
}

void OpenGLViewport::dispatchSingleMeshExport(MeshExportCpuJob job)
{
    QPointer<OpenGLViewport> self(this);
    auto errBox = QSharedPointer<QString>::create();
    auto fut = QtConcurrent::run([job, errBox]() -> bool {
        QString err;
        if (!OpenGLViewport::transformJobToFile(job, &err)) {
            *errBox = err;
            return false;
        }
        return true;
    });
    auto *watcher = new QFutureWatcher<bool>(this);
    QObject::connect(watcher, &QFutureWatcher<bool>::finished, this, [self, watcher, job, errBox]() {
        if (!self) {
            watcher->deleteLater();
            return;
        }
        self->m_meshExportAsyncBusy = false;
        const bool ok = watcher->result();
        watcher->deleteLater();
        const QString msg = ok ? QString() : (*errBox);
        if (ok && job.meshIndex >= 0 && job.meshIndex < self->m_meshChunks.size()) {
            ImportedMeshChunk &ch = self->m_meshChunks[job.meshIndex];
            ch.filePath = job.outputPath;
            ch.name = QFileInfo(job.outputPath).fileName();
            emit self->meshModelsChanged();
        }
        emit self->meshSingleExportFinished(ok, job.meshIndex, msg);
    });
    watcher->setFuture(fut);
}

void OpenGLViewport::dispatchBulkMeshExport(QVector<MeshExportCpuJob> jobs)
{
    QPointer<OpenGLViewport> self(this);
    auto errBox = QSharedPointer<QString>::create();
    auto fut = QtConcurrent::run([jobs, errBox]() -> bool {
        for (const MeshExportCpuJob &job : jobs) {
            QString err;
            if (!OpenGLViewport::transformJobToFile(job, &err)) {
                *errBox = err;
                return false;
            }
        }
        return true;
    });
    auto *watcher = new QFutureWatcher<bool>(this);
    QObject::connect(watcher, &QFutureWatcher<bool>::finished, this, [self, watcher, jobs, errBox]() {
        if (!self) {
            watcher->deleteLater();
            return;
        }
        self->m_meshExportAsyncBusy = false;
        const bool ok = watcher->result();
        watcher->deleteLater();
        if (ok) {
            for (const MeshExportCpuJob &j : jobs) {
                if (j.meshIndex >= 0 && j.meshIndex < self->m_meshChunks.size()) {
                    ImportedMeshChunk &ch = self->m_meshChunks[j.meshIndex];
                    ch.filePath = j.outputPath;
                    ch.name = QFileInfo(j.outputPath).fileName();
                }
            }
            emit self->meshModelsChanged();
        }
        emit self->meshBulkExportFinished(ok, ok ? QString() : (*errBox));
    });
    watcher->setFuture(fut);
}

void OpenGLViewport::exportMeshToFileAsync(int meshIndex, const QUrl &fileUrl, int format)
{
    if (m_meshExportAsyncBusy) {
        emit meshSingleExportFinished(false, meshIndex, tr("有其他导出任务正在进行，请稍候"));
        return;
    }
    MeshExportCpuJob job;
    QString err;
    if (!tryBuildMeshExportJob(meshIndex, fileUrl, format, job, &err)) {
        emit meshSingleExportFinished(false, meshIndex, err);
        return;
    }
    m_meshExportAsyncBusy = true;
    dispatchSingleMeshExport(std::move(job));
}

void OpenGLViewport::exportAllMeshesToFolderAsync(const QUrl &folderUrl, int format)
{
    if (m_meshExportAsyncBusy) {
        emit meshBulkExportFinished(false, tr("有其他导出任务正在进行，请稍候"));
        return;
    }
    const QString dirPath = folderUrl.toLocalFile();
    if (dirPath.isEmpty()) {
        emit meshBulkExportFinished(false, tr("无效的文件夹路径"));
        return;
    }
    QDir dir(dirPath);
    if (!dir.exists()) {
        emit meshBulkExportFinished(false, tr("目标文件夹不存在"));
        return;
    }
    QVector<MeshExportCpuJob> jobs;
    jobs.reserve(m_meshChunks.size());
    for (int i = 0; i < m_meshChunks.size(); ++i) {
        if (!m_meshChunks[i].sceneVisible)
            continue;
        const QString ext = (format == 2) ? QStringLiteral(".obj") : QStringLiteral(".stl");
        QString base = sanitizedExportBaseName(m_meshChunks[i].name);
        QString path = dir.filePath(base + ext);
        int suffix = 0;
        while (QFile::exists(path)) {
            ++suffix;
            path = dir.filePath(base + QStringLiteral("_") + QString::number(suffix) + ext);
        }
        MeshExportCpuJob job;
        QString err;
        if (!tryBuildMeshExportJob(i, QUrl::fromLocalFile(path), format, job, &err)) {
            emit meshBulkExportFinished(false, err);
            return;
        }
        jobs.append(std::move(job));
    }
    if (jobs.isEmpty()) {
        emit meshBulkExportFinished(false, tr("没有可见模型可导出"));
        return;
    }
    m_meshExportAsyncBusy = true;
    dispatchBulkMeshExport(std::move(jobs));
}

void OpenGLViewport::saveMeshInPlaceAsync(int meshIndex)
{
    if (!meshHasPersistentStorage(meshIndex)) {
        emit meshSingleExportFinished(false, meshIndex, tr("当前模型没有可覆盖的 STL/OBJ 源文件"));
        return;
    }
    const QString orig = m_meshChunks.at(meshIndex).filePath.trimmed();
    const QString suf = QFileInfo(orig).suffix().toLower();
    int fmt = 0;
    if (suf == QLatin1String("obj"))
        fmt = 2;
    else if (suf == QLatin1String("stl"))
        fmt = stlDiskFormatHint(orig);
    else {
        emit meshSingleExportFinished(false, meshIndex, tr("仅支持 STL/OBJ 覆盖保存"));
        return;
    }
    exportMeshToFileAsync(meshIndex, QUrl::fromLocalFile(orig), fmt);
}

void OpenGLViewport::setProgress(qreal p)
{
    const qreal np = qBound(0.0, p, 1.0);
    // 不能用 qFuzzyCompare：超长打印下单帧 progress 增量极小，跳过会导致轨迹/喷嘴卡住一整段
    m_trajProgress = np;
    emit progressChanged();
    recomputeTrajectoryPlaybackLine();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setDisplayLayer(int layer)
{
    if (layer == m_trajDisplayLayer)
        return;
    m_trajDisplayLayer = layer;
    emit displayLayerChanged();
    emit displayLayerZInfoChanged();
    recomputeTrajectoryPlaybackLine();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setPlaybackGcodeWindowSize(int lines)
{
    const int n = qBound(20, lines, 4000);
    if (m_playbackGcodeWindowSize == n)
        return;
    m_playbackGcodeWindowSize = n;
    emit playbackGcodeWindowSizeChanged();
    requestPlaybackGcodeWindowUpdate(true);
}

qreal OpenGLViewport::displayLayerHeightMm() const
{
    if (m_trajLayerZs.isEmpty())
        return 0;
    const int li = qBound(0, m_trajDisplayLayer, m_trajLayerZs.size() - 1);
    return qreal(m_trajLayerZs[li]) * 1000.0;
}

void OpenGLViewport::rebuildTrajectoryCumulativeTime()
{
    m_trajCumTimeSec.clear();
    m_trajTotalTimeSec = 0.0;
    m_trajFeatureStats.clear();
    const int n = m_trajSegments.size();
    if (n <= 0)
        return;
    m_trajCumTimeSec.resize(n + 1);
    m_trajCumTimeSec[0] = 0.0;
    for (int i = 0; i < n; ++i) {
        m_trajTotalTimeSec += qMax(1e-9, double(m_trajSegments[i].durationSec));
        m_trajCumTimeSec[i + 1] = m_trajTotalTimeSec;
    }
    rebuildTrajectoryFeatureStats();
}

void OpenGLViewport::rebuildTrajectoryFeatureStats()
{
    m_trajFeatureStats.clear();
    using MK = GCodeParser::MoveKind;
    constexpr int nk = int(MK::ExtrudeSkirtBrim) + 1;
    std::array<double, nk> acc{};
    acc.fill(0.0);
    for (const auto &s : m_trajSegments) {
        const int ki = int(s.kind);
        if (ki >= 0 && ki < nk)
            acc[size_t(ki)] += double(s.durationSec);
    }
    const double T = m_trajTotalTimeSec;
    if (T <= 1e-12)
        return;

    static const std::pair<MK, QString> kRows[] = {
        {MK::ExtrudeOuterWall, QStringLiteral("外壁")},
        {MK::ExtrudeInnerWall, QStringLiteral("内壁")},
        {MK::ExtrudeInfillSolid, QStringLiteral("塑料实心填充")},
        {MK::ExtrudeInfillSparse, QStringLiteral("塑料稀疏填充")},
        {MK::ExtrudeInfill, QStringLiteral("填充")},
        {MK::ExtrudeSkin, QStringLiteral("表皮/顶底")},
        {MK::ExtrudeSupport, QStringLiteral("支撑")},
        {MK::ExtrudeSupportInterface, QStringLiteral("界面支撑")},
        {MK::ExtrudeSkirtBrim, QStringLiteral("裙边/边缘")},
        {MK::ExtrudeFiber, QStringLiteral("纤维")},
        {MK::ExtrudeOther, QStringLiteral("其他挤出")},
        {MK::Travel, QStringLiteral("空走")},
    };
    for (const auto &row : kRows) {
        const double sec = acc[int(row.first)];
        if (sec < 1e-4)
            continue;
        const QColor col = GCodeParser::colorForKind(row.first);
        QVariantMap m;
        m.insert(QStringLiteral("label"), row.second);
        m.insert(QStringLiteral("color"), col.name(QColor::HexRgb));
        m.insert(QStringLiteral("timeSec"), sec);
        m.insert(QStringLiteral("timeMin"), sec / 60.0);
        m.insert(QStringLiteral("ratio"), sec / T);
        m.insert(QStringLiteral("percent"), 100.0 * sec / T);
        m_trajFeatureStats.append(m);
    }
}

int OpenGLViewport::visibleSegmentCountForProgress(qreal progress) const
{
    return trajectoryVisibleEnd(m_trajSegments, progress, m_trajCumTimeSec);
}

qreal OpenGLViewport::progressAtCompletedSegmentCount(int k) const
{
    const int n = m_trajSegments.size();
    if (n <= 0 || m_trajTotalTimeSec <= 1e-12)
        return 0.0;
    if (m_trajCumTimeSec.size() != n + 1)
        return qBound(0.0, qreal(k) / qreal(n), 1.0);
    const int kk = qBound(0, k, n);
    return qreal(m_trajCumTimeSec[kk] / m_trajTotalTimeSec);
}

int OpenGLViewport::completedSegmentCountAtProgress(qreal p) const
{
    return visibleSegmentCountForProgress(p);
}

void OpenGLViewport::recomputeTrajectoryPlaybackLine()
{
    auto tipChanged = [this](float nx, float ny, float nz) {
        if (qAbs(m_trajTipX - nx) > 1e-6f || qAbs(m_trajTipY - ny) > 1e-6f || qAbs(m_trajTipZ - nz) > 1e-6f) {
            m_trajTipX = nx;
            m_trajTipY = ny;
            m_trajTipZ = nz;
            emit playbackTipChanged();
        }
    };

    auto pushFeed = [this](float f) {
        if (!qFuzzyCompare(m_trajPlaybackFeedMmMin, f)) {
            m_trajPlaybackFeedMmMin = f;
            emit playbackFeedChanged();
        }
    };

    if (m_trajSegments.isEmpty()) {
        if (m_trajPlaybackLine != 1) {
            m_trajPlaybackLine = 1;
            emit playbackLineChanged();
        }
        pushFeed(0.f);
        tipChanged(0.f, 0.f, 0.f);
        requestPlaybackGcodeWindowUpdate(false);
        return;
    }
    const int n = m_trajSegments.size();
    const TrajPlaybackSample sm = sampleTrajectoryAtProgress(m_trajSegments, m_trajProgress, m_trajCumTimeSec);
    const int visEnd = qBound(0, sm.fullSegCount, n);
    if (visEnd <= 0 && sm.partialIndex < 0) {
        if (m_trajPlaybackLine != 1) {
            m_trajPlaybackLine = 1;
            emit playbackLineChanged();
        }
        pushFeed(0.f);
        tipChanged(0.f, 0.f, 0.f);
        requestPlaybackGcodeWindowUpdate(false);
        return;
    }
    int line = 1;
    float nx = 0.f, ny = 0.f, nz = 0.f;
    bool any = false;
    float lastFeed = 0.f;
    for (int i = 0; i < visEnd; ++i) {
        const auto &s = m_trajSegments[i];
        line = s.sourceLine;
        nx = s.bx;
        ny = s.by;
        nz = s.bz;
        any = true;
        lastFeed = s.feedMmMin;
    }
    if (sm.partialIndex >= 0 && sm.partialIndex < n) {
        const auto &ps = m_trajSegments[sm.partialIndex];
        const float t = qBound(0.f, sm.partialAlpha, 1.f);
        nx = ps.ax + (ps.bx - ps.ax) * t;
        ny = ps.ay + (ps.by - ps.ay) * t;
        nz = ps.az + (ps.bz - ps.az) * t;
        line = ps.sourceLine;
        lastFeed = ps.feedMmMin;
        any = true;
    }
    if (!any) {
        nx = ny = nz = 0.f;
        lastFeed = 0.f;
    }
    if (line != m_trajPlaybackLine) {
        m_trajPlaybackLine = line;
        emit playbackLineChanged();
    }
    pushFeed(lastFeed);
    tipChanged(nx, ny, nz);
    requestPlaybackGcodeWindowUpdate(false);
}

void OpenGLViewport::requestPlaybackGcodeWindowUpdate(bool force)
{
    const int lineCount = m_trajSourceLines.size();
    if (lineCount <= 0) {
        ++m_playbackGcodeWindowRequestToken; // 让在途任务结果失效
        m_playbackGcodeWindowPending = false;
        const bool changed = !m_playbackGcodeWindowText.isEmpty()
                             || m_playbackGcodeWindowStartLine != 1
                             || m_playbackGcodeWindowEndLine != 0
                             || m_playbackGcodeWindowFocusLine != 0;
        m_playbackGcodeWindowText.clear();
        m_playbackGcodeWindowStartLine = 1;
        m_playbackGcodeWindowEndLine = 0;
        m_playbackGcodeWindowFocusLine = 0;
        if (changed)
            emit playbackGcodeWindowChanged();
        return;
    }

    const int kWindowSize = qBound(20, m_playbackGcodeWindowSize, 4000);
    const int focusLine = qBound(1, m_trajPlaybackLine, lineCount);
    const int halfWindow = kWindowSize / 2;
    const int maxStart = qMax(1, lineCount - kWindowSize + 1);
    const int startLine = qBound(1, focusLine - halfWindow, maxStart);
    const int endLine = qMin(lineCount, startLine + kWindowSize - 1);

    if (!force && !m_playbackGcodeWindowText.isEmpty() && focusLine == m_playbackGcodeWindowFocusLine
        && startLine == m_playbackGcodeWindowStartLine
        && endLine == m_playbackGcodeWindowEndLine) {
        return;
    }

    // 200~1000 行窗口文本构建很小，直接主线程同步更新更稳定，避免拖动时并发竞态导致偶发空白。
    const int lineNoWidth = qMax(6, QString::number(endLine).size());
    QString text;
    text.reserve((endLine - startLine + 1) * 48);
    for (int lineNo = startLine; lineNo <= endLine; ++lineNo) {
        text += (lineNo == focusLine) ? QStringLiteral("▶ ") : QStringLiteral("  ");
        text += QStringLiteral("%1 | ").arg(lineNo, lineNoWidth, 10, QLatin1Char('0'));
        text += m_trajSourceLines.at(lineNo - 1);
        if (lineNo < endLine)
            text += QLatin1Char('\n');
    }

    const bool changed = (m_playbackGcodeWindowText != text)
                         || (m_playbackGcodeWindowStartLine != startLine)
                         || (m_playbackGcodeWindowEndLine != endLine)
                         || (m_playbackGcodeWindowFocusLine != focusLine);
    m_playbackGcodeWindowText = std::move(text);
    m_playbackGcodeWindowStartLine = startLine;
    m_playbackGcodeWindowEndLine = endLine;
    m_playbackGcodeWindowFocusLine = focusLine;
    m_playbackGcodeWindowBuildInFlight = false;
    m_playbackGcodeWindowPending = false;
    if (changed)
        emit playbackGcodeWindowChanged();
}

void OpenGLViewport::launchPlaybackGcodeWindowBuild(int startLine, int endLine, int focusLine,
                                                    const QStringList &rows)
{
    m_playbackGcodeWindowBuildInFlight = true;
    const quint64 token = ++m_playbackGcodeWindowRequestToken;
    QPointer<OpenGLViewport> guard(this);
    (void)QtConcurrent::run([guard, token, startLine, endLine, focusLine, rows]() mutable {
        if (!guard)
            return;
        const int lineNoWidth = qMax(6, QString::number(endLine).size());
        QString text;
        text.reserve(rows.size() * 48);
        for (int i = 0; i < rows.size(); ++i) {
            const int lineNo = startLine + i;
            text += (lineNo == focusLine) ? QStringLiteral("▶ ") : QStringLiteral("  ");
            text += QStringLiteral("%1 | ").arg(lineNo, lineNoWidth, 10, QLatin1Char('0'));
            text += rows.at(i);
            if (i + 1 < rows.size())
                text += QLatin1Char('\n');
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, token, startLine, endLine, focusLine, text = std::move(text)]() mutable {
                if (!guard)
                    return;
                guard->m_playbackGcodeWindowBuildInFlight = false;
                if (token == guard->m_playbackGcodeWindowRequestToken) {
                    const bool changed = (guard->m_playbackGcodeWindowText != text)
                                         || (guard->m_playbackGcodeWindowStartLine != startLine)
                                         || (guard->m_playbackGcodeWindowEndLine != endLine)
                                         || (guard->m_playbackGcodeWindowFocusLine != focusLine);
                    guard->m_playbackGcodeWindowText = std::move(text);
                    guard->m_playbackGcodeWindowStartLine = startLine;
                    guard->m_playbackGcodeWindowEndLine = endLine;
                    guard->m_playbackGcodeWindowFocusLine = focusLine;
                    if (changed)
                        emit guard->playbackGcodeWindowChanged();
                }

                if (guard->m_playbackGcodeWindowPending) {
                    const int ps = guard->m_playbackGcodeWindowPendingStartLine;
                    const int pe = guard->m_playbackGcodeWindowPendingEndLine;
                    const int pf = guard->m_playbackGcodeWindowPendingFocusLine;
                    guard->m_playbackGcodeWindowPending = false;

                    const int lineCount = guard->m_trajSourceLines.size();
                    if (lineCount > 0 && ps >= 1 && pe >= ps && pe <= lineCount) {
                        QStringList pendingRows;
                        pendingRows.reserve(pe - ps + 1);
                        for (int lineNo = ps; lineNo <= pe; ++lineNo)
                            pendingRows.append(guard->m_trajSourceLines.at(lineNo - 1));
                        guard->launchPlaybackGcodeWindowBuild(ps, pe, qBound(ps, pf, pe), pendingRows);
                    }
                }
            },
            Qt::QueuedConnection);
    });
}

void OpenGLViewport::fitTrajectoryDistance()
{
    if (m_trajSegments.isEmpty())
        return;
    const QVector3D c = orbitTarget();
    float maxR = 0.0f;
    for (const auto &s : m_trajSegments) {
        const QVector3D a(s.ax, s.ay, s.az);
        const QVector3D b(s.bx, s.by, s.bz);
        maxR = qMax(maxR, (a - c).length());
        maxR = qMax(maxR, (b - c).length());
    }
    m_distance = qBound(0.35f, maxR * 2.15f, 12.0f);
}

void OpenGLViewport::reportGcodeImportProgress(double p)
{
    if (!m_gcodeImportInProgress)
        return;
    const qreal raw = qBound(0.0, qreal(p), 1.0);
    // 解析线程在写入场景前就可能回调到 100%；进度条在数据提交完成前最高只显示 99.9%
    const qreal np = qMin(raw, qreal(0.999));
    if (qAbs(m_gcodeImportProgress - np) < 0.002 && np < qreal(0.997))
        return;
    m_gcodeImportProgress = np;
    emit gcodeImportProgressChanged();
}

void OpenGLViewport::completeGcodeImport(GCodeParser::ParseResult result, QString path)
{
    if (!result.ok) {
        m_gcodeImportInProgress = false;
        m_gcodeImportProgress = 0.0;
        emit gcodeImportProgressChanged();
        emit gcodeLoadFinished(false, result.errorMessage);
        update();
        if (auto *w = window())
            w->update();
        return;
    }

    m_trajSegments = std::move(result.segments);
    m_trajLayerZs = result.layerZs;
    m_trajSourceLines = std::move(result.sourceLines);
    m_trajGcodeText = std::move(result.rawText);
    m_trajSourceLineCount = result.lineCount;
    rebuildTrajectoryCumulativeTime();
    m_trajSummary = QStringLiteral("段数 %1 · 层 %2 · 行 %3")
                        .arg(m_trajSegments.size())
                        .arg(m_trajLayerZs.size())
                        .arg(m_trajSourceLineCount);
    ++m_trajPathVersion;
    // 从起点开始仿真；若置为 1.0 则播放键一按即到达终点，Timer 立刻停，看起来像无反应
    m_trajProgress = 0.0;
    m_trajDisplayLayer = m_trajLayerZs.isEmpty() ? 0 : (m_trajLayerZs.size() - 1);
    fitTrajectoryDistance();
    emit progressChanged();
    emit displayLayerChanged();
    emit displayLayerZInfoChanged();
    emit pathDataChanged();
    recomputeTrajectoryPlaybackLine();
    requestPlaybackGcodeWindowUpdate(true);
    update();
    if (auto *w = window())
        w->update();

    m_gcodeImportProgress = 1.0;
    emit gcodeImportProgressChanged();
    emit gcodeLoadFinished(true, QFileInfo(path).fileName());

    QTimer::singleShot(0, this, [this]() {
        m_gcodeImportInProgress = false;
        emit gcodeImportProgressChanged();
    });
}

void OpenGLViewport::loadGcode(const QUrl &fileUrl)
{
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty()) {
        emit gcodeLoadFinished(false, QStringLiteral("无效路径"));
        return;
    }
    if (m_gcodeImportInProgress) {
        emit gcodeLoadFinished(false, QStringLiteral("正在导入中，请稍候"));
        return;
    }

    pushUndoSnapshot();
    m_gcodeImportInProgress = true;
    m_gcodeImportProgress = 0.0;
    emit gcodeImportProgressChanged();

    OpenGLViewport *self = this;
    (void)QtConcurrent::run([self, path]() {
        GCodeParser::ProgressCallback cb = [self](int done, int total) {
            const double p = (total > 0) ? double(done) / double(total) : 0.0;
            QMetaObject::invokeMethod(self, "reportGcodeImportProgress", Qt::QueuedConnection,
                                      Q_ARG(double, p));
        };
        GCodeParser::ParseResult r = GCodeParser::parseFile(path, 2'000'000, cb);
        QTimer::singleShot(0, self, [self, r = std::move(r), path]() mutable {
            self->completeGcodeImport(std::move(r), path);
        });
    });
}

void OpenGLViewport::resetPreviewCamera()
{
    pushUndoSnapshot();
    m_yawDeg = kDefaultYawDeg;
    m_pitchDeg = kDefaultPitchDeg;
    m_lookAtUpWorld = QVector3D(0.0f, 0.0f, 1.0f);
    m_planeOrientation = QQuaternion();
    if (!m_trajSegments.isEmpty())
        fitTrajectoryDistance();
    else
        m_distance = kDefaultDistance;
    update();
    if (auto *w = window())
        w->update();
}

QVariantList OpenGLViewport::meshModels() const
{
    QVariantList list;
    list.reserve(m_meshChunks.size());
    for (int i = 0; i < m_meshChunks.size(); ++i) {
        const ImportedMeshChunk &c = m_meshChunks[i];
        QVariantMap m;
        m.insert(QStringLiteral("meshIndex"), i);
        m.insert(QStringLiteral("name"), c.name);
        m.insert(QStringLiteral("filePath"), c.filePath);
        m.insert(QStringLiteral("vertices"), c.vertexCount);
        m.insert(QStringLiteral("faces"), c.vertexCount / 3);
        m.insert(QStringLiteral("active"), c.active);
        m.insert(QStringLiteral("sceneVisible"), c.sceneVisible);
        list.append(m);
    }
    return list;
}

void OpenGLViewport::ensureOneActiveModel()
{
    if (!m_smartSelectEnabled)
        return;
    bool any = false;
    for (const ImportedMeshChunk &c : m_meshChunks) {
        if (c.active) {
            any = true;
            break;
        }
    }
    if (!any && !m_meshChunks.isEmpty()) {
        m_meshChunks[0].active = true;
        emit meshModelsChanged();
        update();
        if (auto *w = window())
            w->update();
    }
}

void OpenGLViewport::setSmartSelectEnabled(bool enabled)
{
    if (m_smartSelectEnabled == enabled)
        return;

    pushUndoSnapshot();
    m_smartSelectEnabled = enabled;
    emit smartSelectEnabledChanged();

    // 关闭智能选择时，如果当前没有激活的模型，不做任何操作
    // 开启智能选择时，如果没有激活的模型，自动激活第一个
    if (enabled) {
        ensureOneActiveModel();
        emit meshModelsChanged();
        update();
        if (auto *w = window())
            w->update();
    }
}

void OpenGLViewport::setModelActive(int index, bool ctrlModifier)
{
    if (index < 0 || index >= m_meshChunks.size()) {
        for (ImportedMeshChunk &c : m_meshChunks)
            c.active = false;
        emit meshModelsChanged();
        update();
        if (auto *w = window())
            w->update();
        return;
    }
    if (ctrlModifier)
        m_meshChunks[index].active = !m_meshChunks[index].active;
    else {
        for (int i = 0; i < m_meshChunks.size(); ++i)
            m_meshChunks[i].active = (i == index);
    }
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setModelSceneVisible(int index, bool visible)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;
    if (m_meshChunks[index].sceneVisible == visible)
        return;
    pushUndoSnapshot();
    m_meshChunks[index].sceneVisible = visible;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::deleteActiveModels()
{
    QVector<int> toDel;
    toDel.reserve(m_meshChunks.size());
    for (int i = 0; i < m_meshChunks.size(); ++i) {
        if (m_meshChunks[i].active)
            toDel.append(i);
    }
    if (toDel.isEmpty())
        return;
    pushUndoSnapshot();
    std::sort(toDel.begin(), toDel.end(), [](int a, int b) { return a > b; });
    for (int idx : std::as_const(toDel))
        deleteModelAtNoUndo(idx);
}

void OpenGLViewport::deleteModelAt(int index)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;

    pushUndoSnapshot();
    if (m_meshChunks.size() == 1) {
        clearMeshSceneNoUndo();
        return;
    }
    deleteModelAtNoUndo(index);
}

void OpenGLViewport::clearAllModels()
{
    pushUndoSnapshot();
    clearMeshSceneNoUndo();
}

void OpenGLViewport::autoArrangeModels()
{
    if (!m_importedMeshBuf || m_importedMeshBuf->isEmpty() || m_meshChunks.isEmpty())
        return;

    pushUndoSnapshot();

    struct Footprint {
        int meshIndex = -1;
        float minX = 0.f;
        float maxX = 0.f;
        float minY = 0.f;
        float maxY = 0.f;
        float minZ = 0.f;
        float width = 0.f;
        float depth = 0.f;
        float centerX = 0.f;
        float centerY = 0.f;
        float area = 0.f;
    };
    struct Rect {
        float x0 = 0.f;
        float y0 = 0.f;
        float x1 = 0.f;
        float y1 = 0.f;
    };

    QVector<Footprint> items;
    items.reserve(m_meshChunks.size());
    const QVector<float> &verts = *m_importedMeshBuf;

    for (int i = 0; i < m_meshChunks.size(); ++i) {
        const ImportedMeshChunk &chunk = m_meshChunks[i];
        if (!chunk.sceneVisible || chunk.vertexCount <= 0)
            continue;
        if (chunk.firstVertex < 0 || (chunk.firstVertex + chunk.vertexCount) * 6 > verts.size())
            continue;

        float minX = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float minZ = std::numeric_limits<float>::max();

        const QVector3D c = chunk.geomCenterLocal;
        const QVector3D &s = chunk.mirrorScale;
        const float us = chunk.uniformScale;
        for (int v = chunk.firstVertex; v < chunk.firstVertex + chunk.vertexCount; ++v) {
            const QVector3D p0(verts[v * 6 + 0], verts[v * 6 + 1], verts[v * 6 + 2]);
            const QVector3D ps(c.x() + us * (p0.x() - c.x()),
                               c.y() + us * (p0.y() - c.y()),
                               c.z() + us * (p0.z() - c.z()));
            const QVector3D pl(c.x() + s.x() * (ps.x() - c.x()),
                                 c.y() + s.y() * (ps.y() - c.y()),
                                 c.z() + s.z() * (ps.z() - c.z()));
            const QVector3D p = chunk.rotation.rotatedVector(pl);
            minX = qMin(minX, p.x());
            maxX = qMax(maxX, p.x());
            minY = qMin(minY, p.y());
            maxY = qMax(maxY, p.y());
            minZ = qMin(minZ, p.z());
        }

        const float w = qMax(0.001f, maxX - minX);
        const float d = qMax(0.001f, maxY - minY);
        Footprint fp;
        fp.meshIndex = i;
        fp.minX = minX;
        fp.maxX = maxX;
        fp.minY = minY;
        fp.maxY = maxY;
        fp.minZ = minZ;
        fp.width = w;
        fp.depth = d;
        fp.centerX = 0.5f * (minX + maxX);
        fp.centerY = 0.5f * (minY + maxY);
        fp.area = w * d;
        items.push_back(fp);
    }
    if (items.isEmpty())
        return;

    std::sort(items.begin(), items.end(), [](const Footprint &a, const Footprint &b) {
        return a.area > b.area;
    });

    const float bedMin = 0.0f;
    const float bedMax = kBedPlaneExtentMeters;
    const float bedCenter = 0.5f * (bedMin + bedMax);
    const float gap = 0.004f; // 4mm 间隙，避免互相贴边
    QVector<Rect> placed;
    placed.reserve(items.size());

    auto overlaps = [&](const Rect &r) {
        for (const Rect &o : std::as_const(placed)) {
            if (!(r.x1 <= o.x0 || r.x0 >= o.x1 || r.y1 <= o.y0 || r.y0 >= o.y1))
                return true;
        }
        return false;
    };

    auto insideBed = [&](const Rect &r) {
        return r.x0 >= bedMin && r.y0 >= bedMin && r.x1 <= bedMax && r.y1 <= bedMax;
    };

    auto buildRect = [&](const Footprint &fp, float centerX, float centerY) {
        const float hw = 0.5f * fp.width;
        const float hd = 0.5f * fp.depth;
        Rect r;
        r.x0 = centerX - hw - gap * 0.5f;
        r.y0 = centerY - hd - gap * 0.5f;
        r.x1 = centerX + hw + gap * 0.5f;
        r.y1 = centerY + hd + gap * 0.5f;
        return r;
    };

    auto tryFindSpiralSpot = [&](const Footprint &fp, QVector3D &outOffset) {
        const float hw = 0.5f * fp.width;
        const float hd = 0.5f * fp.depth;
        const float minCx = bedMin + hw + gap;
        const float maxCx = bedMax - hw - gap;
        const float minCy = bedMin + hd + gap;
        const float maxCy = bedMax - hd - gap;
        if (minCx > maxCx || minCy > maxCy)
            return false;

        auto testCenter = [&](float cx, float cy) {
            cx = qBound(minCx, cx, maxCx);
            cy = qBound(minCy, cy, maxCy);
            const Rect r = buildRect(fp, cx, cy);
            if (!insideBed(r) || overlaps(r))
                return false;
            placed.push_back(r);
            outOffset = QVector3D(cx - fp.centerX, cy - fp.centerY, -fp.minZ);
            return true;
        };

        if (testCenter(bedCenter, bedCenter))
            return true;

        const float step = qMax(0.008f, qMin(fp.width, fp.depth) * 0.35f);
        const int maxRing = int(qCeil((bedMax - bedMin) / step)) + 4;
        for (int ring = 1; ring <= maxRing; ++ring) {
            const float r = ring * step;
            for (int sx = -ring; sx <= ring; ++sx) {
                const float cx = bedCenter + sx * step;
                if (testCenter(cx, bedCenter - r) || testCenter(cx, bedCenter + r))
                    return true;
            }
            for (int sy = -ring + 1; sy <= ring - 1; ++sy) {
                const float cy = bedCenter + sy * step;
                if (testCenter(bedCenter - r, cy) || testCenter(bedCenter + r, cy))
                    return true;
            }
        }

        // 兜底：全床网格扫描（仍按中心距离优先）
        struct Candidate {
            float cx = 0.f;
            float cy = 0.f;
            float d2 = 0.f;
        };
        QVector<Candidate> cands;
        for (float cy = minCy; cy <= maxCy + 1e-6f; cy += step) {
            for (float cx = minCx; cx <= maxCx + 1e-6f; cx += step) {
                Candidate c;
                c.cx = cx;
                c.cy = cy;
                const float dx = cx - bedCenter;
                const float dy = cy - bedCenter;
                c.d2 = dx * dx + dy * dy;
                cands.push_back(c);
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) {
            return a.d2 < b.d2;
        });
        for (const Candidate &c : std::as_const(cands)) {
            if (testCenter(c.cx, c.cy))
                return true;
        }
        return false;
    };

    bool anyPlaced = false;
    for (const Footprint &fp : std::as_const(items)) {
        QVector3D newOffset;
        if (tryFindSpiralSpot(fp, newOffset)) {
            m_meshChunks[fp.meshIndex].positionOffset = newOffset;
            anyPlaced = true;
        }
    }

    if (!anyPlaced)
        return;

    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::startRotateModel(int index)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;

    pushUndoSnapshot();
    m_rotatingModel = true;
    m_rotateModelIndex = index;
    m_rotateStartQuaternion = m_meshChunks[index].rotation;
    m_rotateLastPos = QPointF(0, 0);
    setCursor(QCursor(Qt::CrossCursor));
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::stopRotateModel()
{
    m_rotatingModel = false;
    m_rotateModelIndex = -1;
    m_rotateStartQuaternion = QQuaternion();
    m_rotateLastPos = QPointF(0, 0);
    setCursor(QCursor(Qt::ArrowCursor));
    update();
    if (auto *w = window())
        w->update();
}

int OpenGLViewport::getModelAt(int x, int y)
{
    return pickMeshAt(QPointF(x, y));
}

void OpenGLViewport::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_lastPos = event->position();
        m_pressPos = event->position();

        if (m_interactionTool == 2) {
            QVector3D hitWorld;
            if (!intersectBedPlaneWorld(m_pressPos, hitWorld)) {
                event->ignore();
                return;
            }
            if (!m_measureHasFirstPoint) {
                m_measurePointA = hitWorld;
                m_measurePointB = hitWorld;
                m_measureHasFirstPoint = true;
                m_measuredDistanceMm = 0.0;
            } else {
                m_measurePointB = hitWorld;
                pushUndoSnapshot();
                MeasureTrace tr;
                tr.pointA = m_measurePointA;
                tr.pointB = m_measurePointB;
                tr.distanceMm = qreal((m_measurePointB - m_measurePointA).length()) * 1000.0;
                m_measureTraces.push_back(tr);
                m_measureHasFirstPoint = false;
                m_measuredDistanceMm = tr.distanceMm;
            }
            emit measurementChanged();
            event->accept();
            return;
        }

        if (m_interactionTool == 3) {
            const int hitMeshIndex = pickMeshAt(m_pressPos);
            if (hitMeshIndex >= 0 && !(m_previewMode && !m_trajSegments.isEmpty())) {
                setModelActive(hitMeshIndex, false);
                mirrorModelAt(hitMeshIndex, 0);
                event->accept();
                return;
            }
            event->ignore();
            return;
        }

        if (m_interactionTool == 1) {
            const int hitMeshIndex = pickMeshAt(m_pressPos);
            if (hitMeshIndex >= 0 && !(m_previewMode && !m_trajSegments.isEmpty())) {
                setModelActive(hitMeshIndex, false);
                startRotateModel(hitMeshIndex);
                m_rotateDragActive = true;
                event->accept();
                return;
            }
            event->ignore();
            return;
        }

        const int hitMeshIndex = pickMeshAt(m_pressPos);
        if (hitMeshIndex >= 0 && !(m_previewMode && !m_trajSegments.isEmpty())) {
            const bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;
            setModelActive(hitMeshIndex, ctrl);
            pushUndoSnapshot();
            m_draggingMesh = true;
            m_dragMeshIndex = hitMeshIndex;
            m_pressMeshPos = m_meshChunks[hitMeshIndex].positionOffset;
            m_dragging = false;
            setCursor(QCursor(Qt::SizeAllCursor));
            intersectBedPlaneWorld(m_pressPos, m_pressBedHitWorld);
            event->accept();
            return;
        }

        // 命中左下角视图立方体时须 accept，否则 press 被 ignore 后 release 可能收不到，点击面无法转视角
        if (pickViewCubeFaceAt(m_pressPos) >= 0) {
            event->accept();
            return;
        }

        event->ignore();
        return;
    }

    if (event->button() == Qt::RightButton) {
        m_lastPos = event->position();
        m_pressPos = event->position();

        if (event->modifiers() & Qt::ControlModifier) {
            // Ctrl+右键：平移视图（原中键平移在部分环境下无法稳定触发）
            m_panning = true;
            m_dragging = false;
            m_draggingMesh = false;
            m_dragMeshIndex = -1;
            m_pressViewCubeFace = -1;
            m_pressMeshIndexForMenu = -1;
            setCursor(QCursor(Qt::ClosedHandCursor));
            grabMouse();
            event->accept();
            return;
        }

        m_pressViewCubeFace = pickViewCubeFaceAt(m_pressPos);

        // 右键：按住拖动旋转视图；右键轻点（无拖动）命中模型则弹出菜单
        m_pressMeshIndexForMenu = pickMeshAt(m_pressPos);
        if (m_pressMeshIndexForMenu >= 0) {
            setModelActive(m_pressMeshIndexForMenu, false);
        }

        m_dragging = true;
        m_draggingMesh = false;
        m_dragMeshIndex = -1;
        // 手动拖转热床时用默认 up，与点击立方体「正视某面」的 roll 区分开
        m_lookAtUpWorld = QVector3D(0.0f, 0.0f, 1.0f);
        event->accept();
        return;
    }
    event->ignore();
}

void OpenGLViewport::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF p = event->position();
    const QPointF d = p - m_lastPos;
    m_lastPos = p;

    if (m_interactionTool == 2 && m_measureHasFirstPoint) {
        QVector3D hitWorld;
        if (intersectBedPlaneWorld(p, hitWorld)) {
            m_measurePointB = hitWorld;
            emit measurementChanged();
            update();
            if (auto *w = window())
                w->update();
            event->accept();
            return;
        }
    }

    if (m_panning) {
        const QVector3D orbitCenter = orbitTarget();
        const float panScale = 0.0018f * m_distance;
        const float yaw = qDegreesToRadians(m_yawDeg);
        const float pitch = qDegreesToRadians(m_pitchDeg);
        const float cp = qCos(pitch);
        const QVector3D offset(
            m_distance * cp * qSin(yaw),
            m_distance * cp * qCos(yaw),
            m_distance * qSin(pitch)
        );
        const QVector3D eye = orbitCenter + offset;
        QMatrix4x4 view;
        view.lookAt(eye, orbitCenter, stableLookAtUp(orbitCenter - eye, m_lookAtUpWorld));
        const QMatrix4x4 invView = view.inverted();
        QVector3D camRight(invView(0, 0), invView(1, 0), invView(2, 0));
        QVector3D camUp(invView(0, 1), invView(1, 1), invView(2, 1));
        camRight.normalize();
        camUp.normalize();
        const QVector3D planeNormal = m_planeOrientation.rotatedVector(QVector3D(0.0f, 0.0f, 1.0f)).normalized();
        QVector3D panDelta = camRight * float(-d.x()) * panScale + camUp * (float(d.y()) * panScale);
        panDelta = rejectAlongUnitNormal(panDelta, planeNormal);
        m_viewPanWorld += panDelta;
        update();
        event->accept();
        return;
    }

    // 模型拖动模式
    if (m_draggingMesh && m_dragMeshIndex >= 0 && m_dragMeshIndex < m_meshChunks.size()) {
        if (m_meshChunks.isEmpty()) {
            event->ignore();
            return;
        }

        // 在热板平面上拖动：用鼠标射线与（随 m_planeOrientation 旋转的）热板平面求交
        QVector3D hitWorld;
        if (!intersectBedPlaneWorld(p, hitWorld)) {
            event->ignore();
            return;
        }

        const QVector3D worldDelta = hitWorld - m_pressBedHitWorld;
        const QVector3D localDelta = m_planeOrientation.conjugated().rotatedVector(worldDelta);
        // 在热床平面（局部 XY）上平移，不限定在可打印矩形内；Z 保持按下时偏移，避免拖出 z=0 面
        QVector3D newOffset(
            m_pressMeshPos.x() + localDelta.x(),
            m_pressMeshPos.y() + localDelta.y(),
            m_pressMeshPos.z());
        m_meshChunks[m_dragMeshIndex].positionOffset = newOffset;

        update();
        event->accept();
        return;
    }

    // 模型旋转模式（控制球）
    if (m_rotatingModel && m_rotateDragActive && m_rotateModelIndex >= 0
        && m_rotateModelIndex < m_meshChunks.size()) {
        if (m_meshChunks.isEmpty()) {
            event->ignore();
            return;
        }

        // 计算相机相关的旋转向量
        const QVector3D orbitCenter = orbitTarget();
        const float yaw = qDegreesToRadians(m_yawDeg);
        const float pitch = qDegreesToRadians(m_pitchDeg);
        const float cp = qCos(pitch);
        const QVector3D offset(
            m_distance * cp * qSin(yaw),
            m_distance * cp * qCos(yaw),
            m_distance * qSin(pitch)
        );
        const QVector3D eye = orbitCenter + offset;
        QMatrix4x4 view;
        view.lookAt(eye, orbitCenter, stableLookAtUp(orbitCenter - eye, m_lookAtUpWorld));
        const QMatrix4x4 invView = view.inverted();
        QVector3D camRight(invView(0, 0), invView(1, 0), invView(2, 0));
        QVector3D camUp(invView(0, 1), invView(1, 1), invView(2, 1));
        camRight.normalize();
        camUp.normalize();

        // 与当前视角对齐：左右拖动 → 绕相机 up；上下拖动 → 绕相机 right
        const float rotSens = 0.40f;
        const QQuaternion qH = QQuaternion::fromAxisAndAngle(camUp, float(d.x()) * rotSens);
        const QQuaternion qV = QQuaternion::fromAxisAndAngle(camRight, float(-d.y()) * rotSens);
        QQuaternion newRotation = qV * qH * m_rotateStartQuaternion;

        auto computeLocalGeomCenter = [this](int idx, QVector3D &c) -> bool {
            if (idx < 0 || idx >= m_meshChunks.size() || !m_importedMeshBuf || m_importedMeshBuf->isEmpty())
                return false;
            const auto &chunk = m_meshChunks[idx];
            if (chunk.vertexCount <= 0)
                return false;
            const int v0 = chunk.firstVertex;
            const int v1 = v0 + chunk.vertexCount;
            if (v0 < 0 || v1 * 6 > m_importedMeshBuf->size())
                return false;
            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            float maxZ = -std::numeric_limits<float>::max();
            for (int v = v0; v < v1; ++v) {
                const float x = (*m_importedMeshBuf)[v * 6 + 0];
                const float y = (*m_importedMeshBuf)[v * 6 + 1];
                const float z = (*m_importedMeshBuf)[v * 6 + 2];
                minX = qMin(minX, x);
                minY = qMin(minY, y);
                minZ = qMin(minZ, z);
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
                maxZ = qMax(maxZ, z);
            }
            c = QVector3D((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f);
            return true;
        };

        newRotation.normalize();
        ImportedMeshChunk &chunk = m_meshChunks[m_rotateModelIndex];
        QVector3D localCenter;
        if (computeLocalGeomCenter(m_rotateModelIndex, localCenter)) {
            const QVector3D worldCenter = chunk.positionOffset + chunk.rotation.rotatedVector(localCenter);
            chunk.rotation = newRotation;
            chunk.positionOffset = worldCenter - chunk.rotation.rotatedVector(localCenter);
        } else {
            chunk.rotation = newRotation;
        }
        m_rotateStartQuaternion = chunk.rotation;

        update();
        event->accept();
        return;
    }

    // 场景旋转模式
    if (!m_dragging) {
        event->ignore();
        return;
    }

    const float sens = 0.35f;
    const QVector3D orbitCenter = orbitTarget();
    const float yaw = qDegreesToRadians(m_yawDeg);
    const float pitch = qDegreesToRadians(m_pitchDeg);
    const float cp = qCos(pitch);
    // Z-up 轨道相机：Z 为竖直方向
    const QVector3D offset(
        m_distance * cp * qSin(yaw),
        m_distance * cp * qCos(yaw),
        m_distance * qSin(pitch)
    );
    const QVector3D eye = orbitCenter + offset;
    QMatrix4x4 view;
    view.lookAt(eye, orbitCenter, stableLookAtUp(orbitCenter - eye, m_lookAtUpWorld));
    const QMatrix4x4 invView = view.inverted();
    QVector3D camRight(invView(0, 0), invView(1, 0), invView(2, 0));
    QVector3D camUp(invView(0, 1), invView(1, 1), invView(2, 1));
    camRight.normalize();
    camUp.normalize();

    // 与当前视角对齐：左右拖动 → 绕相机 up；上下拖动 → 绕相机 right（符号与拖动方向一致）
    const QQuaternion qH = QQuaternion::fromAxisAndAngle(camUp, float(d.x()) * sens);
    const QQuaternion qV = QQuaternion::fromAxisAndAngle(camRight, float(d.y()) * sens);
    m_planeOrientation = (qV * qH * m_planeOrientation).normalized();

    update();
    event->accept();
}

void OpenGLViewport::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_rotateDragActive) {
            m_rotateDragActive = false;
            if (m_rotatingModel)
                stopRotateModel();
            event->accept();
            return;
        }
        if (m_draggingMesh) {
            m_draggingMesh = false;
            m_dragMeshIndex = -1;
            unsetCursor();
            event->accept();
            return;
        }
            // If it was a click (no drag) on the view-cube, snap to that face
            const QPointF releaseDelta = event->position() - m_pressPos;
            if (releaseDelta.manhattanLength() < 6.0) {
                const int face = pickViewCubeFaceAt(event->position());
                if (face >= 0) {
                    snapCameraToModelFace(face);
                    event->accept();
                    return;
                }
            }
        event->ignore();
        return;
    }

    if (event->button() == Qt::RightButton) {
        if (m_panning) {
            m_panning = false;
            ungrabMouse();
            unsetCursor();
            event->accept();
            return;
        }

        if (m_rotatingModel) {
            // 模型旋转结束，但保持旋转模式直到显式退出
            event->accept();
            return;
        }

        if (m_draggingMesh) {
            // 模型拖动结束
            m_draggingMesh = false;
            m_dragMeshIndex = -1;
            unsetCursor();
            event->accept();
            return;
        }

        const QPointF releaseDelta = event->position() - m_pressPos;
        if (m_pressViewCubeFace >= 0) {
            if (releaseDelta.manhattanLength() < 6.0)
                snapCameraToModelFace(m_pressViewCubeFace);
        } else {
            // 右键轻点模型：请求 QML 弹出菜单
            if (releaseDelta.manhattanLength() < 6.0 && m_pressMeshIndexForMenu >= 0) {
                emit contextMenuRequested(m_pressMeshIndexForMenu,
                                          event->position().x(),
                                          event->position().y());
            }
        }
        // 不再在右键松手时强制热床回正：会打断用户姿态，且会清空视图立方体触发的轨道视角动画
        m_dragging = false;
        m_pressViewCubeFace = -1;
        m_pressMeshIndexForMenu = -1;
        event->accept();
        return;
    }

    event->ignore();
}

void OpenGLViewport::wheelEvent(QWheelEvent *event)
{
    // angleDelta().y(): 通常每格 120
    const QPoint numDegrees = event->angleDelta() / 8;
    const float steps = numDegrees.y() / 15.0f;

    if (qFuzzyIsNull(steps)) {
        event->ignore();
        return;
    }

    if (m_interactionTool == 4) {
        if (m_meshChunks.isEmpty() || (m_previewMode && !m_trajSegments.isEmpty())) {
            event->ignore();
            return;
        }
        constexpr float kScaleStep = 0.07f;
        const float factor = std::pow(1.0f + kScaleStep, steps);
        bool wouldChange = false;
        for (int i = 0; i < m_meshChunks.size(); ++i) {
            if (!m_meshChunks[i].active || !m_meshChunks[i].sceneVisible)
                continue;
            const float s = m_meshChunks[i].uniformScale;
            const float ns = qBound(0.02f, s * factor, 50.0f);
            if (!qFuzzyCompare(ns, s)) {
                wouldChange = true;
                break;
            }
        }
        if (!wouldChange) {
            event->ignore();
            return;
        }
        pushUndoSnapshot();
        for (int i = 0; i < m_meshChunks.size(); ++i) {
            if (!m_meshChunks[i].active || !m_meshChunks[i].sceneVisible)
                continue;
            float &s = m_meshChunks[i].uniformScale;
            const float ns = qBound(0.02f, s * factor, 50.0f);
            if (!qFuzzyCompare(ns, s))
                s = ns;
        }
        emit meshModelsChanged();
        update();
        if (auto *w = window())
            w->update();
        event->accept();
        return;
    }

    const float zoomSpeed = 0.04f;
    m_distance = qBound(0.25f, m_distance - steps * zoomSpeed, 6.0f);
    update();
    event->accept();
}

int OpenGLViewport::pickViewCubeFaceAt(const QPointF &itemPos) const
{
    const qreal iw = qMax(qreal(1.0), width());
    const qreal ih = qMax(qreal(1.0), height());
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : qreal(1.0);
    const int fbW = qMax(1, int(qRound(iw * dpr)));
    const int fbH = qMax(1, int(qRound(ih * dpr)));
    return pickViewCubeFaceImpl(itemPos, iw, ih, fbW, fbH, m_planeOrientation, m_yawDeg, m_pitchDeg,
                                m_lookAtUpWorld);
}

int OpenGLViewport::pickMeshAt(const QPointF &itemPos) const
{
    if (m_meshChunks.isEmpty() || !m_importedMeshBuf || m_importedMeshBuf->isEmpty())
        return -1;

    const qreal iw = qMax(qreal(1.0), width());
    const qreal ih = qMax(qreal(1.0), height());
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : qreal(1.0);
    const int fbW = qMax(1, int(qRound(iw * dpr)));
    const int fbH = qMax(1, int(qRound(ih * dpr)));

    // 转换为OpenGL屏幕坐标 (Y轴翻转)
    const double mx = itemPos.x() * double(fbW) / double(iw);
    const double myTop = itemPos.y() * double(fbH) / double(ih);
    const double myGl = fbH - 1 - myTop;

    // 构建相机矩阵（与渲染一致）
    const QVector3D orbitCenter = orbitTarget();
    const float yaw = qDegreesToRadians(m_yawDeg);
    const float pitch = qDegreesToRadians(m_pitchDeg);
    const float cp = qCos(pitch);
    const QVector3D offset(
        m_distance * cp * qSin(yaw),
        m_distance * cp * qCos(yaw),
        m_distance * qSin(pitch)
    );
    const QVector3D eye = orbitCenter + offset;

    QMatrix4x4 proj;
    const float aspect = fbH > 0 ? float(fbW) / float(fbH) : (800.0f / 600.0f);
    proj.perspective(45.0f, aspect, 0.001f, 100.0f);

    QMatrix4x4 view;
    view.lookAt(eye, orbitCenter, stableLookAtUp(orbitCenter - eye, m_lookAtUpWorld));

    const QMatrix4x4 invProj = proj.inverted();
    const QMatrix4x4 invView = view.inverted();

    const double ndcX = (mx / fbW) * 2.0 - 1.0;
    const double ndcY = (myGl / fbH) * 2.0 - 1.0;

    QVector4D near4(ndcX, ndcY, 0.0, 1.0);
    QVector4D far4(ndcX, ndcY, 1.0, 1.0);

    QVector4D nearWorld4 = invProj * near4;
    QVector4D farWorld4 = invProj * far4;
    nearWorld4 /= nearWorld4.w();
    farWorld4 /= farWorld4.w();

    nearWorld4 = invView * nearWorld4;
    farWorld4 = invView * farWorld4;

    const QVector3D rayOrigin(nearWorld4.x(), nearWorld4.y(), nearWorld4.z());
    const QVector3D rayDir((farWorld4 - nearWorld4).toVector3D().normalized());

    // 与 mesh 绘制一致：sceneModel * translate(offset) * rotate(chunk)
    QMatrix4x4 sceneModel;
    sceneModel.translate(orbitCenter);
    sceneModel.rotate(m_planeOrientation);
    sceneModel.translate(-orbitCenter);

    const auto &vertices = *m_importedMeshBuf;
    int bestHit = -1;
    float bestT = std::numeric_limits<float>::max();

    auto raySlabAabb = [](const QVector3D &ro, const QVector3D &rd,
                          float minX, float maxX, float minY, float maxY, float minZ, float maxZ,
                          float &outTEnter) -> bool {
        float tMin = 0.0f;
        float tMax = std::numeric_limits<float>::max();

        if (qFuzzyIsNull(rd.x())) {
            if (ro.x() < minX || ro.x() > maxX)
                return false;
        } else {
            float t1 = (minX - ro.x()) / rd.x();
            float t2 = (maxX - ro.x()) / rd.x();
            if (t1 > t2)
                qSwap(t1, t2);
            tMin = qMax(tMin, t1);
            tMax = qMin(tMax, t2);
            if (tMin > tMax)
                return false;
        }

        if (qFuzzyIsNull(rd.y())) {
            if (ro.y() < minY || ro.y() > maxY)
                return false;
        } else {
            float t1 = (minY - ro.y()) / rd.y();
            float t2 = (maxY - ro.y()) / rd.y();
            if (t1 > t2)
                qSwap(t1, t2);
            tMin = qMax(tMin, t1);
            tMax = qMin(tMax, t2);
            if (tMin > tMax)
                return false;
        }

        if (qFuzzyIsNull(rd.z())) {
            if (ro.z() < minZ || ro.z() > maxZ)
                return false;
        } else {
            float t1 = (minZ - ro.z()) / rd.z();
            float t2 = (maxZ - ro.z()) / rd.z();
            if (t1 > t2)
                qSwap(t1, t2);
            tMin = qMax(tMin, t1);
            tMax = qMin(tMax, t2);
            if (tMin > tMax)
                return false;
        }

        // 沿世界射线参数 t 与局部 ro + t*rd 一致（仿射 model 矩阵）
        if (tMax < 1e-6f)
            return false;
        outTEnter = (tMin > 1e-6f) ? tMin : 1e-6f;
        return true;
    };

    for (int chunkIdx = 0; chunkIdx < m_meshChunks.size(); ++chunkIdx) {
        if (!m_meshChunks[chunkIdx].sceneVisible)
            continue;

        const auto &chunk = m_meshChunks[chunkIdx];

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float minZ = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();

        for (int v = chunk.firstVertex; v < chunk.firstVertex + chunk.vertexCount; ++v) {
            const float x = vertices[v * 6 + 0];
            const float y = vertices[v * 6 + 1];
            const float z = vertices[v * 6 + 2];
            minX = qMin(minX, x);
            maxX = qMax(maxX, x);
            minY = qMin(minY, y);
            maxY = qMax(maxY, y);
            minZ = qMin(minZ, z);
            maxZ = qMax(maxZ, z);
        }

        QMatrix4x4 chunkModel = sceneModel;
        chunkModel.translate(chunk.positionOffset);
        chunkModel.rotate(chunk.rotation);
        appendChunkMirrorTransform(chunkModel, chunk.geomCenterLocal, chunk.mirrorScale);
        if (!qFuzzyCompare(chunk.uniformScale, 1.0f)) {
            chunkModel.translate(chunk.geomCenterLocal);
            chunkModel.scale(chunk.uniformScale);
            chunkModel.translate(-chunk.geomCenterLocal);
        }

        bool invertible = false;
        const QMatrix4x4 invChunk = chunkModel.inverted(&invertible);
        if (!invertible)
            continue;

        const QVector3D roL = invChunk.map(rayOrigin);
        QVector3D rdL = invChunk.mapVector(rayDir);
        if (rdL.lengthSquared() < 1e-20f)
            continue;
        rdL.normalize();

        float tEnter = 0.0f;
        if (!raySlabAabb(roL, rdL, minX, maxX, minY, maxY, minZ, maxZ, tEnter))
            continue;

        if (tEnter < bestT) {
            bestT = tEnter;
            bestHit = chunkIdx;
        }
    }

    return bestHit;
}

bool OpenGLViewport::rayFromItemPos(const QPointF &itemPos, QVector3D &rayOrigin, QVector3D &rayDir) const
{
    const qreal iw = qMax(qreal(1.0), width());
    const qreal ih = qMax(qreal(1.0), height());
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : qreal(1.0);
    const int fbW = qMax(1, int(qRound(iw * dpr)));
    const int fbH = qMax(1, int(qRound(ih * dpr)));

    // 转换为 OpenGL 屏幕坐标 (Y 轴翻转)
    const double mx = itemPos.x() * double(fbW) / double(iw);
    const double myTop = itemPos.y() * double(fbH) / double(ih);
    const double myGl = fbH - 1 - myTop;

    // 相机矩阵（需与渲染保持一致）
    const QVector3D orbitCenter = orbitTarget();
    const float yaw = qDegreesToRadians(m_yawDeg);
    const float pitch = qDegreesToRadians(m_pitchDeg);
    const float cp = qCos(pitch);
    const QVector3D offset(
        m_distance * cp * qSin(yaw),
        m_distance * cp * qCos(yaw),
        m_distance * qSin(pitch)
    );
    const QVector3D eye = orbitCenter + offset;

    QMatrix4x4 proj;
    const float aspect = fbH > 0 ? float(fbW) / float(fbH) : (800.0f / 600.0f);
    proj.perspective(45.0f, aspect, 0.001f, 100.0f);

    QMatrix4x4 view;
    view.lookAt(eye, orbitCenter, stableLookAtUp(orbitCenter - eye, m_lookAtUpWorld));

    const QMatrix4x4 invProj = proj.inverted();
    const QMatrix4x4 invView = view.inverted();

    const double ndcX = (mx / fbW) * 2.0 - 1.0;
    const double ndcY = (myGl / fbH) * 2.0 - 1.0;

    QVector4D near4(ndcX, ndcY, 0.0, 1.0);
    QVector4D far4(ndcX, ndcY, 1.0, 1.0);

    QVector4D nearWorld4 = invProj * near4;
    QVector4D farWorld4 = invProj * far4;
    if (qFuzzyIsNull(nearWorld4.w()) || qFuzzyIsNull(farWorld4.w()))
        return false;
    nearWorld4 /= nearWorld4.w();
    farWorld4 /= farWorld4.w();

    nearWorld4 = invView * nearWorld4;
    farWorld4 = invView * farWorld4;

    rayOrigin = QVector3D(nearWorld4.x(), nearWorld4.y(), nearWorld4.z());
    rayDir = (farWorld4 - nearWorld4).toVector3D().normalized();
    return true;
}

bool OpenGLViewport::intersectBedPlaneWorld(const QPointF &itemPos, QVector3D &hitWorld) const
{
    QVector3D ro, rd;
    if (!rayFromItemPos(itemPos, ro, rd))
        return false;

    const QVector3D planePoint = orbitTarget();
    const QVector3D planeNormal = m_planeOrientation.rotatedVector(QVector3D(0.0f, 0.0f, 1.0f)).normalized();
    const float denom = QVector3D::dotProduct(planeNormal, rd);
    if (qFuzzyIsNull(denom))
        return false;

    const float t = QVector3D::dotProduct(planeNormal, (planePoint - ro)) / denom;
    if (t < 0.0f)
        return false;

    hitWorld = ro + rd * t;
    return true;
}

void OpenGLViewport::snapCameraToModelFace(int faceIndex)
{
    // 点击视图立方体：轨道相机转到正视该面（与热床 m_planeOrientation 对齐的世界法线）
    // Face id: 0 +Z 上 1 -Z 下 2 左(-X) 3 右(+X) 4 +Y 后 5 -Y 前
    if (faceIndex < 0 || faceIndex > 5)
        return;

    static const QVector3D kFaceOut[6] = {
        QVector3D(0.0f, 0.0f, 1.0f),  // 0 上 +Z
        QVector3D(0.0f, 0.0f, -1.0f), // 1 下
        QVector3D(-1.0f, 0.0f, 0.0f), // 2 左
        QVector3D(1.0f, 0.0f, 0.0f),  // 3 右
        QVector3D(0.0f, 1.0f, 0.0f),  // 4 后
        QVector3D(0.0f, -1.0f, 0.0f), // 5 前
    };

    QVector3D dir = m_planeOrientation.rotatedVector(kFaceOut[faceIndex]);
    if (dir.lengthSquared() < 1e-20f)
        return;
    dir.normalize();

    // 各面局部 up：与视图立方体 UV/几何一致，使正视时汉字直立（+Y「后」需 -Z 为屏上）
    static const QVector3D kUpLocal[6] = {
        QVector3D(0.0f, 1.0f, 0.0f),  // 0 +Z 上
        QVector3D(0.0f, -1.0f, 0.0f), // 1 -Z 下（与「上」相反，避免底视时「下」倒立）
        QVector3D(0.0f, 0.0f, 1.0f),  // 2 -X 左
        QVector3D(0.0f, 0.0f, 1.0f),  // 3 +X 右
        QVector3D(0.0f, 0.0f, -1.0f), // 4 +Y 后
        QVector3D(0.0f, 0.0f, 1.0f),  // 5 -Y 前
    };
    {
        QVector3D upW = m_planeOrientation.rotatedVector(kUpLocal[faceIndex]);
        if (upW.lengthSquared() < 1e-20f)
            upW = QVector3D(0.0f, 0.0f, 1.0f);
        else
            upW.normalize();
        m_lookAtUpWorld = upW;
    }

    const float targetPitchRad = qAsin(qBound(-1.0f, dir.z(), 1.0f));
    float targetPitch = qRadiansToDegrees(targetPitchRad);
    const float cp = qCos(targetPitchRad);
    float targetYaw = m_yawDeg;
    if (qAbs(cp) > 0.02f)
        targetYaw = qRadiansToDegrees(qAtan2(dir.x(), dir.y()));

    targetPitch = qBound(-89.0f, targetPitch, 89.0f);

    m_faceAnimActive = false;

    m_orbitSnapYawStart = m_yawDeg;
    m_orbitSnapPitchStart = m_pitchDeg;
    m_orbitSnapYawEnd = targetYaw;
    m_orbitSnapPitchEnd = targetPitch;
    m_orbitViewSnapActive = true;
    m_faceAnimElapsed.restart();
    if (m_faceAnimTimer && !m_faceAnimTimer->isActive())
        m_faceAnimTimer->start();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::faceAnimStep()
{
    const qint64 ms = m_faceAnimElapsed.elapsed();
    const float t = qMin(1.0f, float(ms) / float(m_faceAnimDurationMs));
    const float tt = t * t * (3.0f - 2.0f * t);

    if (m_orbitViewSnapActive) {
        m_yawDeg = lerpAngleDeg(m_orbitSnapYawStart, m_orbitSnapYawEnd, tt);
        m_pitchDeg = m_orbitSnapPitchStart + (m_orbitSnapPitchEnd - m_orbitSnapPitchStart) * tt;
        m_pitchDeg = qBound(-89.0f, m_pitchDeg, 89.0f);
        update();
        if (auto *w = window())
            w->update();
        if (t >= 1.0f) {
            m_yawDeg = m_orbitSnapYawEnd;
            m_pitchDeg = qBound(-89.0f, m_orbitSnapPitchEnd, 89.0f);
            m_orbitViewSnapActive = false;
        }
    }

    if (m_faceAnimActive) {
        QQuaternion q = QQuaternion::slerp(m_faceAnimStart, m_faceAnimTarget, tt);
        m_planeOrientation = q.normalized();
        update();
        if (auto *w = window())
            w->update();
        if (t >= 1.0f) {
            m_planeOrientation = m_faceAnimTarget.normalized();
            m_faceAnimActive = false;
        }
    }

    if (!m_orbitViewSnapActive && !m_faceAnimActive) {
        if (m_faceAnimTimer && m_faceAnimTimer->isActive())
            m_faceAnimTimer->stop();
    }
}

void OpenGLViewport::hoverMoveEvent(QHoverEvent *event)
{
    if (m_interactionTool == 2 && m_measureHasFirstPoint) {
        QVector3D hitWorld;
        if (intersectBedPlaneWorld(event->position(), hitWorld)) {
            m_measurePointB = hitWorld;
            emit measurementChanged();
            update();
            event->accept();
            return;
        }
    }

    if (m_panning) {
        event->accept();
        return;
    }
    const int f = pickViewCubeFaceAt(event->position());
    if (f != m_hoverViewCubeFace) {
        m_hoverViewCubeFace = f;
        update();
    }
    setCursor(f >= 0 ? QCursor(Qt::PointingHandCursor) : QCursor(Qt::ArrowCursor));
    event->accept();
}

void OpenGLViewport::hoverLeaveEvent(QHoverEvent *event)
{
    if (m_hoverViewCubeFace >= 0) {
        m_hoverViewCubeFace = -1;
        update();
    }
    unsetCursor();
    event->accept();
    QQuickFramebufferObject::hoverLeaveEvent(event);
}

void OpenGLViewport::importModel(const QUrl &fileUrl)
{
    const QString path = fileUrl.toLocalFile();
    qInfo() << "[Import] importModel url=" << fileUrl.toString() << "localPath=" << path
            << "thread=" << QThread::currentThreadId();

    if (path.isEmpty()) {
        qWarning() << "[Import] empty local path";
        emit modelImportFinished(false, QStringLiteral("无效的文件路径"));
        return;
    }
    if (m_importInProgress) {
        qWarning() << "[Import] busy, skip";
        emit modelImportFinished(false, QStringLiteral("正在导入中，请稍候"));
        return;
    }
    m_importInProgress = true;
    emit importInProgressChanged();

    using ImportTuple = std::tuple<bool, QString, std::shared_ptr<QVector<float>>>;
    auto future = QtConcurrent::run([path]() -> ImportTuple {
        qInfo() << "[Import] WORKER start thread=" << QThread::currentThreadId() << "path=" << path;
        QElapsedTimer wt;
        wt.start();
        QString err;
        auto mesh = std::make_shared<QVector<float>>();
        const bool ok = MeshLoader::loadTriangleMesh(path, *mesh, &err);
        qInfo() << "[Import] WORKER end ms=" << wt.elapsed() << "ok=" << ok << "floats=" << mesh->size()
                << "verts=" << (mesh->size() / 6);
        if (!ok)
            mesh.reset();
        return std::make_tuple(ok, std::move(err), std::move(mesh));
    });

    auto *watcher = new QFutureWatcher<ImportTuple>(this);
    QObject::connect(
        watcher,
        &QFutureWatcher<ImportTuple>::finished,
        this,
        [this, watcher, path]() {
            m_importInProgress = false;
            emit importInProgressChanged();
            // result() 只拷贝 tuple + shared_ptr，不拷贝顶点数组
            const ImportTuple pack = watcher->result();
            watcher->deleteLater();

            const bool ok = std::get<0>(pack);
            const QString err = std::get<1>(pack);
            std::shared_ptr<QVector<float>> mesh = std::get<2>(pack);

            qInfo() << "[Import] MAIN slot thread=" << QThread::currentThreadId() << "ok=" << ok
                    << "floats=" << (mesh && mesh.get() ? mesh->size() : 0);

            if (!ok) {
                emit modelImportFinished(false, err);
                return;
            }
            if (!mesh || mesh->isEmpty() || (mesh->size() % 6) != 0) {
                qWarning() << "[Import] bad mesh empty or stride";
                emit modelImportFinished(false, QStringLiteral("网格数据无效"));
                return;
            }

            pushUndoSnapshot();
            const int prevTotalVerts =
                (!m_importedMeshBuf || m_importedMeshBuf->isEmpty()) ? 0 : int(m_importedMeshBuf->size() / 6);
            const int chunkVerts = int(mesh->size() / 6);

            // 多文件导入：渲染线程会在 synchronize()/uploadImportedMesh() 读取 m_importedMeshBuf。
            // 这里不能对同一个 QVector 做原地 reserve/append（可能触发重分配，造成跨线程悬空指针）。
            // 采用“构建新 buffer -> 原子替换 shared_ptr”的方式保证读写安全。
            if (!m_importedMeshBuf || m_importedMeshBuf->isEmpty()) {
                m_importedMeshBuf = std::move(mesh);
            } else {
                auto newBuf = std::make_shared<QVector<float>>();
                newBuf->reserve(m_importedMeshBuf->size() + mesh->size());
                newBuf->append(*m_importedMeshBuf);
                newBuf->append(*mesh);
                m_importedMeshBuf = std::move(newBuf);
            }

            ImportedMeshChunk chunk;
            const QFileInfo fi(path);
            chunk.name = fi.fileName();
            chunk.filePath = fi.absoluteFilePath();
            chunk.firstVertex = prevTotalVerts;
            chunk.vertexCount = chunkVerts;
            chunk.active = false;
            chunk.sceneVisible = true;
            m_meshChunks.push_back(chunk);
            fillGeomCenterForChunk(m_meshChunks.last(), *m_importedMeshBuf);
            ensureOneActiveModel();

            m_importedMeshVertexCount = m_importedMeshBuf->size() / 6;
            ++m_meshDataVersion;
            qInfo() << "[Import] MAIN applied meshDataVersion=" << m_meshDataVersion
                    << "vertexCount=" << m_importedMeshVertexCount << "buf.get="
                    << reinterpret_cast<const void *>(m_importedMeshBuf.get())
                    << "use_count=" << m_importedMeshBuf.use_count();
            emit meshModelsChanged();
            // 触发一次立即渲染：让 renderer 尽快 synchronize 并上传 VBO
            update();
            if (auto *w = window())
                w->update();
            emit modelImportFinished(true,
                                     QStringLiteral("已导入 %1 个顶点").arg(m_importedMeshVertexCount));
        });
    watcher->setFuture(future);
}
