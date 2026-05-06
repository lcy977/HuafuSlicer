#include "OpenGLViewport.h"
#include "MeshLoader.h"

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
#include <cmath>
#include <QtOpenGL/QOpenGLFramebufferObject>
#include <QtOpenGL/QOpenGLShaderProgram>
#include <QtOpenGL/QOpenGLTexture>
#include <QMetaObject>
#include <QDebug>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <QUrl>
#include <QColor>
#include <QFileInfo>
#include <QTimer>
#include <QVariantMap>
#include <memory>
#include <tuple>
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

static QImage buildViewCubeLabelAtlas()
{
    const QString labels[6] = {
        QStringLiteral("前面"), QStringLiteral("后面"),
        QStringLiteral("顶部"), QStringLiteral("底部"),
        QStringLiteral("右边"), QStringLiteral("左边")
    };
    constexpr int cellW = 128;
    constexpr int cellH = 128;
    constexpr int atlasW = cellW * 6;
    QImage img(atlasW, cellH, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(48, 52, 58));

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f(QStringLiteral("Microsoft YaHei UI"));
    if (!f.exactMatch()) {
        f.setFamily(QStringLiteral("Microsoft YaHei"));
    }
    f.setPixelSize(26);
    p.setFont(f);

    for (int i = 0; i < 6; ++i) {
        const int x0 = i * cellW;
        p.setPen(QPen(QColor(95, 102, 115), 1));
        p.setBrush(QColor(82, 88, 98));
        p.drawRect(x0 + 1, 1, cellW - 2, cellH - 2);
        p.setPen(QColor(235, 238, 245));
        const QRect r(x0, 0, cellW, cellH);
        p.drawText(r, Qt::AlignCenter, labels[i]);
    }
    p.end();
    return img;
}

static QMatrix4x4 viewCubeProj(int gw, int gh)
{
    QMatrix4x4 P;
    const float aspect = gh > 0 ? float(gw) / float(gh) : 1.0f;
    P.perspective(45.0f, aspect, 0.05f, 100.0f);
    return P;
}

static QMatrix4x4 viewCubeView()
{
    QMatrix4x4 V;
    // 视图立方体使用稳定的 Y-up 相机（避免 up 与视线共线导致 lookAt 退化）
    V.lookAt(QVector3D(0.0f, 0.0f, 2.85f), QVector3D(0.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f));
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
    if (hit.z() >= hi - eps)
        faceOut = 0;
    else if (hit.z() <= lo + eps)
        faceOut = 1;
    else if (hit.y() >= hi - eps)
        faceOut = 2;
    else if (hit.y() <= lo + eps)
        faceOut = 3;
    else if (hit.x() >= hi - eps)
        faceOut = 4;
    else if (hit.x() <= lo + eps)
        faceOut = 5;
    else
        return false;
    return true;
}

static int pickViewCubeFaceImpl(const QPointF &itemPos, qreal itemW, qreal itemH, int fbW, int fbH,
                                const QQuaternion &planeOrientation)
{
    if (fbW < 1 || fbH < 1 || itemW <= 0.0 || itemH <= 0.0)
        return -1;

    const double mx = itemPos.x() * double(fbW) / double(itemW);
    const double myTop = itemPos.y() * double(fbH) / double(itemH);
    const int myGl = fbH - 1 - int(qBound(0, int(qRound(myTop)), fbH - 1));

    const int x0 = fbW - kGizmoMargin - kGizmoSize;
    const int y0 = fbH - kGizmoMargin - kGizmoSize;
    const int gw = kGizmoSize;
    const int gh = kGizmoSize;

    if (mx < double(x0) || mx >= double(x0 + gw) || myGl < y0 || myGl >= y0 + gh)
        return -1;

    const float px = float(mx - double(x0));
    const float py = float(myGl - y0);
    const float ndcX = (px + 0.5f) / float(gw) * 2.0f - 1.0f;
    const float ndcY = (py + 0.5f) / float(gh) * 2.0f - 1.0f;

    const QMatrix4x4 P = viewCubeProj(gw, gh);
    const QMatrix4x4 V = viewCubeView();
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

static int trajectoryVisibleEnd(const QVector<GCodeParser::Segment> &segs,
                                  qreal progress,
                                  const QVector<double> &cumTimeSec)
{
    const int n = segs.size();
    if (n <= 0)
        return 0;
    if (cumTimeSec.size() == n + 1 && cumTimeSec.constLast() > 1e-12) {
        const double T = qBound(0.0, double(progress), 1.0) * cumTimeSec.constLast();
        int k = 0;
        while (k < n && cumTimeSec[k + 1] <= T)
            ++k;
        return k;
    }
    return int(qFloor(progress * qreal(n) + 1e-9));
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
    int visEnd = trajectoryVisibleEnd(segs, progress, cumTimeSec);
    visEnd = qBound(0, visEnd, n);

    float zCut = 1.0e6f;
    if (!layerZs.isEmpty()) {
        const int li = qBound(0, displayLayer, layerZs.size() - 1);
        zCut = layerZs[li] + 1e-4f;
    }

    out.reserve(size_t(visEnd) * 12);
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
}
} // namespace

struct MeshDrawChunk {
    int firstVertex = 0;
    int vertexCount = 0;
    bool selected = false;
    bool active = false;
    bool sceneVisible = true;
    QVector3D positionOffset = QVector3D(0, 0, 0);
    QQuaternion rotation = QQuaternion();
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
        if (vaoMesh.isCreated()) vaoMesh.destroy();
        if (vaoBg.isCreated()) vaoBg.destroy();
        if (vaoTraj.isCreated()) vaoTraj.destroy();
        if (vboPlane.isCreated()) vboPlane.destroy();
        if (vboGrid.isCreated()) vboGrid.destroy();
        if (vboAxis.isCreated()) vboAxis.destroy();
        if (vboAxisLabel.isCreated()) vboAxisLabel.destroy();
        if (vboViewCube.isCreated()) vboViewCube.destroy();
        if (vboViewCubeAxis.isCreated()) vboViewCubeAxis.destroy();
        if (vboMesh.isCreated()) vboMesh.destroy();
        if (vboBg.isCreated()) vboBg.destroy();
        if (vboTraj.isCreated()) vboTraj.destroy();
        delete bgTexture;
        bgTexture = nullptr;
        delete viewCubeAtlas;
        viewCubeAtlas = nullptr;
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
            uniform mat4 model;
            uniform mat4 mvp;
            uniform mat3 uNormalMat;
            out vec3 vWorldPos;
            out vec3 vWorldNormal;
            void main() {
                vec4 wp = model * vec4(position, 1.0);
                vWorldPos = wp.xyz;
                vWorldNormal = normalize(uNormalMat * normal);
                gl_Position = mvp * vec4(position, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 position;
            layout(location = 1) in vec3 normal;
            uniform mat4 model;
            uniform mat4 mvp;
            uniform mat3 uNormalMat;
            out vec3 vWorldPos;
            out vec3 vWorldNormal;
            void main() {
                vec4 wp = model * vec4(position, 1.0);
                vWorldPos = wp.xyz;
                vWorldNormal = normalize(uNormalMat * normal);
                gl_Position = mvp * vec4(position, 1.0);
            }
        )");

        const QByteArray meshFragmentShaderSource = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
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
            uniform vec3 uMeshAlbedo;
            uniform float uDrawMode;
            void main() {
                float nlen0 = length(vWorldNormal);
                vec3 N0 = (nlen0 > 1e-6) ? (vWorldNormal / nlen0) : vec3(0.0, 0.0, 1.0);
                vec3 Vdir0 = (uCameraPos - vWorldPos);
                float vlen0 = max(length(Vdir0), 1e-6);
                vec3 V0 = Vdir0 / vlen0;
                // 叠加：uDrawMode 1.0=选中红，2.0=激活绿（用 float 避免部分驱动 int uniform 异常）
                if (uDrawMode > 1.5) {
                    float edge = pow(1.0 - max(dot(N0, V0), 0.0), 1.55);
                    float rim = pow(1.0 - max(dot(N0, V0), 0.0), 3.2) * 0.55;
                    vec3 fluo = vec3(0.22, 1.0, 0.48) * (0.38 + 0.90 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                if (uDrawMode > 0.5) {
                    float edge = pow(1.0 - max(dot(N0, V0), 0.0), 1.55);
                    float rim = pow(1.0 - max(dot(N0, V0), 0.0), 3.2) * 0.55;
                    vec3 fluo = vec3(1.0, 0.22, 0.14) * (0.42 + 0.95 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                // 兜底：若 uniform 未成功设置（全 0），仍保持灰白材质
                vec3 albedo = (dot(uMeshAlbedo, uMeshAlbedo) > 1e-6) ? uMeshAlbedo : vec3(0.78, 0.79, 0.80);
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
            void main() {
                float nlen0 = length(vWorldNormal);
                vec3 N0 = (nlen0 > 1e-6) ? (vWorldNormal / nlen0) : vec3(0.0, 0.0, 1.0);
                vec3 Vdir0 = (uCameraPos - vWorldPos);
                float vlen0 = max(length(Vdir0), 1e-6);
                vec3 V0 = Vdir0 / vlen0;
                if (uDrawMode > 1.5) {
                    float edge = pow(1.0 - max(dot(N0, V0), 0.0), 1.55);
                    float rim = pow(1.0 - max(dot(N0, V0), 0.0), 3.2) * 0.55;
                    vec3 fluo = vec3(0.22, 1.0, 0.48) * (0.38 + 0.90 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                if (uDrawMode > 0.5) {
                    float edge = pow(1.0 - max(dot(N0, V0), 0.0), 1.55);
                    float rim = pow(1.0 - max(dot(N0, V0), 0.0), 3.2) * 0.55;
                    vec3 fluo = vec3(1.0, 0.22, 0.14) * (0.42 + 0.95 * edge + rim);
                    outColor = vec4(fluo, 1.0);
                    return;
                }
                // 兜底：若 uniform 未成功设置（全 0），仍保持灰白材质
                vec3 albedo = (dot(uMeshAlbedo, uMeshAlbedo) > 1e-6) ? uMeshAlbedo : vec3(0.78, 0.79, 0.80);
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
        rotationGizmoProgram.removeAllShaders();

        // Rotation gizmo shader (控制球)
        const QByteArray gizmoVertSrc = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec3 aColor;
            uniform mat4 uMVP;
            out vec3 vColor;
            void main() {
                vColor = aColor;
                gl_Position = uMVP * vec4(aPos, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec3 aColor;
            uniform mat4 uMVP;
            out vec3 vColor;
            void main() {
                vColor = aColor;
                gl_Position = uMVP * vec4(aPos, 1.0);
            }
        )");

        const QByteArray gizmoFragSrc = ctx->isOpenGLES() ? QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec3 vColor;
            out vec4 outColor;
            void main() {
                outColor = vec4(vColor, 1.0);
            }
        )") : QByteArray(R"(
            #version 330 core
            in vec3 vColor;
            out vec4 outColor;
            void main() {
                outColor = vec4(vColor, 1.0);
            }
        )");

        if (!rotationGizmoProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, gizmoVertSrc)) {
            qWarning() << "[Gizmo] Vertex shader FAILED:" << rotationGizmoProgram.log();
        }
        if (!rotationGizmoProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, gizmoFragSrc)) {
            qWarning() << "[Gizmo] Fragment shader FAILED:" << rotationGizmoProgram.log();
        }
        rotationGizmoProgram.bindAttributeLocation("aPos", 0);
        rotationGizmoProgram.bindAttributeLocation("aColor", 1);
        if (!rotationGizmoProgram.link()) {
            qWarning() << "[Gizmo] Program link FAILED:" << rotationGizmoProgram.log();
        }

        setupRotationGizmoGeometry();

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
        bool mLinkOk = meshProgram.link();
        if (!mLinkOk) {
            qWarning() << "[ViewportRenderer] Mesh program link FAILED:" << meshProgram.log();
            return;
        }
        qInfo() << "[ViewportRenderer] Mesh program OK attrib pos=0 normal=1";

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
            const QString path = QStringLiteral(u"D:/编辑图-4.png");
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

        // 原点处沿 X、Y 的边线（左下角坐标系）
        QVector3D edgeColor(0.52f, 0.55f, 0.60f);
        vertices << 0.0f << 0.0f << zEdge << edgeColor.x() << edgeColor.y() << edgeColor.z();
        vertices << L << 0.0f << zEdge << edgeColor.x() << edgeColor.y() << edgeColor.z();
        vertices << 0.0f << 0.0f << zEdge << edgeColor.x() << edgeColor.y() << edgeColor.z();
        vertices << 0.0f << L << zEdge << edgeColor.x() << edgeColor.y() << edgeColor.z();

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

    void setupRotationGizmoGeometry() {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLFunctions *f = ctx->functions();
        if (!f) {
            qWarning() << "[RotationGizmo] No GL functions";
            return;
        }

        qDebug() << "[RotationGizmo] Creating rotation gizmo vertices...";

        // 生成球体网格（用于显示控制球）
        QVector<float> vertices;
        auto push = [&](float x, float y, float z, float r, float g, float b) {
            vertices << x << y << z << r << g << b;
        };

        const float radius = 0.08f;  // 控制球半径
        const int stacks = 16;        // 纬度分段
        const int slices = 24;        // 经度分段

        // 球体网格
        for (int i = 0; i < stacks; ++i) {
            float phi1 = M_PI * i / stacks;
            float phi2 = M_PI * (i + 1) / stacks;
            for (int j = 0; j < slices; ++j) {
                float theta1 = 2.0f * M_PI * j / slices;
                float theta2 = 2.0f * M_PI * (j + 1) / slices;

                float x1 = radius * sinf(phi1) * cosf(theta1);
                float y1 = radius * sinf(phi1) * sinf(theta1);
                float z1 = radius * cosf(phi1);
                float x2 = radius * sinf(phi1) * cosf(theta2);
                float y2 = radius * sinf(phi1) * sinf(theta2);
                float z2 = radius * cosf(phi1);
                float x3 = radius * sinf(phi2) * cosf(theta2);
                float y3 = radius * sinf(phi2) * sinf(theta2);
                float z3 = radius * cosf(phi2);
                float x4 = radius * sinf(phi2) * cosf(theta1);
                float y4 = radius * sinf(phi2) * sinf(theta1);
                float z4 = radius * cosf(phi2);

                // 半透明灰色的球体
                float gray = 0.6f;
                push(x1, y1, z1, gray, gray, gray);
                push(x2, y2, z2, gray, gray, gray);
                push(x3, y3, z3, gray, gray, gray);
                push(x1, y1, z1, gray, gray, gray);
                push(x3, y3, z3, gray, gray, gray);
                push(x4, y4, z4, gray, gray, gray);
            }
        }

        // 三条坐标轴（X红、Y绿、Z蓝）
        float axisLen = radius * 1.4f;
        float axisW = 0.006f;
        // X轴 - 红色
        push(0, 0, 0, 0.9f, 0.2f, 0.2f);
        push(axisLen, 0, 0, 0.9f, 0.2f, 0.2f);
        // Y轴 - 绿色
        push(0, 0, 0, 0.2f, 0.9f, 0.2f);
        push(0, axisLen, 0, 0.2f, 0.9f, 0.2f);
        // Z轴 - 蓝色
        push(0, 0, 0, 0.2f, 0.45f, 0.95f);
        push(0, 0, axisLen, 0.2f, 0.45f, 0.95f);

        // 绘制旋转向量的圆环（三个互相垂直的环）
        const int ringSegments = 48;
        const float ringRadius = radius * 1.1f;
        const float ringThickness = 0.004f;

        // XY平面环（白色）
        for (int i = 0; i < ringSegments; ++i) {
            float a1 = 2.0f * M_PI * i / ringSegments;
            float a2 = 2.0f * M_PI * (i + 1) / ringSegments;
            float r = ringRadius;
            push(r * cosf(a1), r * sinf(a1), 0, 0.9f, 0.9f, 0.9f);
            push(r * cosf(a2), r * sinf(a2), 0, 0.9f, 0.9f, 0.9f);
            push(r * cosf(a1) + ringThickness, r * sinf(a1) + ringThickness, 0, 0.9f, 0.9f, 0.9f);
        }

        // XZ平面环（淡蓝色）
        for (int i = 0; i < ringSegments; ++i) {
            float a1 = 2.0f * M_PI * i / ringSegments;
            float a2 = 2.0f * M_PI * (i + 1) / ringSegments;
            float r = ringRadius;
            push(r * cosf(a1), 0, r * sinf(a1), 0.6f, 0.8f, 1.0f);
            push(r * cosf(a2), 0, r * sinf(a2), 0.6f, 0.8f, 1.0f);
            push(r * cosf(a1) + ringThickness, 0, r * sinf(a1) + ringThickness, 0.6f, 0.8f, 1.0f);
        }

        // YZ平面环（淡绿色）
        for (int i = 0; i < ringSegments; ++i) {
            float a1 = 2.0f * M_PI * i / ringSegments;
            float a2 = 2.0f * M_PI * (i + 1) / ringSegments;
            float r = ringRadius;
            push(0, r * cosf(a1), r * sinf(a1), 0.6f, 1.0f, 0.6f);
            push(0, r * cosf(a2), r * sinf(a2), 0.6f, 1.0f, 0.6f);
            push(0, r * cosf(a1) + ringThickness, r * sinf(a1) + ringThickness, 0.6f, 1.0f, 0.6f);
        }

        rotationGizmoVertexCount = vertices.size() / 6;

        vboRotationGizmo.create();
        vboRotationGizmo.bind();
        vboRotationGizmo.allocate(vertices.constData(), vertices.size() * sizeof(float));

        vaoRotationGizmo.create();
        vaoRotationGizmo.bind();
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        vaoRotationGizmo.release();
        vboRotationGizmo.release();

        qDebug() << "[RotationGizmo] SUCCESS - vertices:" << rotationGizmoVertexCount;
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
            layout(location = 1) in vec2 aUv;
            layout(location = 2) in float aFace;
            uniform mat4 uMvp;
            uniform mat3 uRot;
            uniform vec3 uEyeWorld;
            out vec2 vUv;
            flat out float vFace;
            int faceId(float x) { return int(x + 0.5); }
            vec3 faceCenter(int fi) {
                if (fi == 0) return vec3(0.0, 0.0, 0.5);
                if (fi == 1) return vec3(0.0, 0.0, -0.5);
                if (fi == 2) return vec3(0.0, 0.5, 0.0);
                if (fi == 3) return vec3(0.0, -0.5, 0.0);
                if (fi == 4) return vec3(0.5, 0.0, 0.0);
                return vec3(-0.5, 0.0, 0.0);
            }
            vec3 faceNormalLocal(int fi) {
                if (fi == 0) return vec3(0.0, 0.0, 1.0);
                if (fi == 1) return vec3(0.0, 0.0, -1.0);
                if (fi == 2) return vec3(0.0, 1.0, 0.0);
                if (fi == 3) return vec3(0.0, -1.0, 0.0);
                if (fi == 4) return vec3(1.0, 0.0, 0.0);
                return vec3(-1.0, 0.0, 0.0);
            }
            void main() {
                int fi = faceId(aFace);
                vFace = aFace;
                vec3 cLoc = faceCenter(fi);
                vec3 nLoc = faceNormalLocal(fi);
                vec3 dLoc = aPos - cLoc;
                mat3 R = uRot;
                vec3 nW = normalize(R * nLoc);
                vec3 pW = R * aPos;
                vec3 wv = normalize(uEyeWorld - pW);
                vec3 worldUp = vec3(0.0, 1.0, 0.0);
                vec3 camRight = cross(worldUp, wv);
                if (dot(camRight, camRight) < 1e-8)
                    camRight = cross(vec3(1.0, 0.0, 0.0), wv);
                camRight = normalize(camRight);
                vec3 camUp = normalize(cross(wv, camRight));
                vec3 uAxis = camRight - nW * dot(camRight, nW);
                vec3 vAxis = camUp - nW * dot(camUp, nW);
                if (dot(uAxis, uAxis) < 1e-8) {
                    uAxis = normalize(cross(nW, worldUp));
                    vAxis = normalize(cross(nW, uAxis));
                } else {
                    uAxis = normalize(uAxis);
                    vAxis = normalize(vAxis);
                    if (dot(cross(uAxis, vAxis), nW) < 0.0)
                        vAxis = -vAxis;
                }
                vec3 dW = R * dLoc;
                float uT = dot(dW, uAxis) + 0.5;
                float vT = dot(dW, vAxis) + 0.5;
                vUv = vec2(uT, vT);
                gl_Position = uMvp * vec4(aPos, 1.0);
            }
        )");
        const QByteArray vcVertGl = QByteArray(R"(
            #version 330 core
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec2 aUv;
            layout(location = 2) in float aFace;
            uniform mat4 uMvp;
            uniform mat3 uRot;
            uniform vec3 uEyeWorld;
            out vec2 vUv;
            flat out float vFace;
            int faceId(float x) { return int(x + 0.5); }
            vec3 faceCenter(int fi) {
                if (fi == 0) return vec3(0.0, 0.0, 0.5);
                if (fi == 1) return vec3(0.0, 0.0, -0.5);
                if (fi == 2) return vec3(0.0, 0.5, 0.0);
                if (fi == 3) return vec3(0.0, -0.5, 0.0);
                if (fi == 4) return vec3(0.5, 0.0, 0.0);
                return vec3(-0.5, 0.0, 0.0);
            }
            vec3 faceNormalLocal(int fi) {
                if (fi == 0) return vec3(0.0, 0.0, 1.0);
                if (fi == 1) return vec3(0.0, 0.0, -1.0);
                if (fi == 2) return vec3(0.0, 1.0, 0.0);
                if (fi == 3) return vec3(0.0, -1.0, 0.0);
                if (fi == 4) return vec3(1.0, 0.0, 0.0);
                return vec3(-1.0, 0.0, 0.0);
            }
            void main() {
                int fi = faceId(aFace);
                vFace = aFace;
                vec3 cLoc = faceCenter(fi);
                vec3 nLoc = faceNormalLocal(fi);
                vec3 dLoc = aPos - cLoc;
                mat3 R = uRot;
                vec3 nW = normalize(R * nLoc);
                vec3 pW = R * aPos;
                vec3 wv = normalize(uEyeWorld - pW);
                vec3 worldUp = vec3(0.0, 1.0, 0.0);
                vec3 camRight = cross(worldUp, wv);
                if (dot(camRight, camRight) < 1e-8)
                    camRight = cross(vec3(1.0, 0.0, 0.0), wv);
                camRight = normalize(camRight);
                vec3 camUp = normalize(cross(wv, camRight));
                vec3 uAxis = camRight - nW * dot(camRight, nW);
                vec3 vAxis = camUp - nW * dot(camUp, nW);
                if (dot(uAxis, uAxis) < 1e-8) {
                    uAxis = normalize(cross(nW, worldUp));
                    vAxis = normalize(cross(nW, uAxis));
                } else {
                    uAxis = normalize(uAxis);
                    vAxis = normalize(vAxis);
                    if (dot(cross(uAxis, vAxis), nW) < 0.0)
                        vAxis = -vAxis;
                }
                vec3 dW = R * dLoc;
                float uT = dot(dW, uAxis) + 0.5;
                float vT = dot(dW, vAxis) + 0.5;
                vUv = vec2(uT, vT);
                gl_Position = uMvp * vec4(aPos, 1.0);
            }
        )");
        const QByteArray vcFragEs = QByteArray(R"(
            #version 300 es
            precision highp float;
            in vec2 vUv;
            flat in float vFace;
            uniform sampler2D uAtlas;
            uniform int uHoverFace;
            out highp vec4 outColor;
            void main() {
                int fi = int(vFace + 0.5);
                float uu = clamp(vUv.x, 0.001, 0.999);
                float vv = clamp(1.0 - vUv.y, 0.001, 0.999);
                vec2 tuv = vec2((float(fi) + uu) / 6.0, vv);
                vec3 rgb = texture(uAtlas, tuv).rgb;
                if (fi == uHoverFace)
                    rgb = mix(rgb, vec3(0.30, 0.78, 1.0), 0.52);
                outColor = vec4(rgb, 1.0);
            }
        )");
        const QByteArray vcFragGl = QByteArray(R"(
            #version 330 core
            in vec2 vUv;
            flat in float vFace;
            uniform sampler2D uAtlas;
            uniform int uHoverFace;
            out vec4 outColor;
            void main() {
                int fi = int(vFace + 0.5);
                float uu = clamp(vUv.x, 0.001, 0.999);
                float vv = clamp(1.0 - vUv.y, 0.001, 0.999);
                vec2 tuv = vec2((float(fi) + uu) / 6.0, vv);
                vec3 rgb = texture(uAtlas, tuv).rgb;
                if (fi == uHoverFace)
                    rgb = mix(rgb, vec3(0.30, 0.78, 1.0), 0.52);
                outColor = vec4(rgb, 1.0);
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
        viewCubeProgram.bindAttributeLocation("aUv", 1);
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

        // 立方体 6 面：统一外侧 CCW 绕序（用于背面剔除，保证“实心”）
        // 0 前面 +Z
        addQuad(0,
                -0.5f, -0.5f, +0.5f, 0, 0,
                +0.5f, -0.5f, +0.5f, 1, 0,
                +0.5f, +0.5f, +0.5f, 1, 1,
                -0.5f, +0.5f, +0.5f, 0, 1);
        // 1 后面 -Z
        addQuad(1,
                -0.5f, -0.5f, -0.5f, 0, 0,
                +0.5f, -0.5f, -0.5f, 1, 0,
                +0.5f, +0.5f, -0.5f, 1, 1,
                -0.5f, +0.5f, -0.5f, 0, 1);
        // 2 顶部 +Y
        addQuad(2,
                -0.5f, +0.5f, -0.5f, 0, 0,
                -0.5f, +0.5f, +0.5f, 1, 0,
                +0.5f, +0.5f, +0.5f, 1, 1,
                +0.5f, +0.5f, -0.5f, 0, 1);
        // 3 底部 -Y
        addQuad(3,
                -0.5f, -0.5f, -0.5f, 0, 0,
                +0.5f, -0.5f, -0.5f, 1, 0,
                +0.5f, -0.5f, +0.5f, 1, 1,
                -0.5f, -0.5f, +0.5f, 0, 1);
        // 4 右边 +X
        addQuad(4,
                +0.5f, -0.5f, -0.5f, 0, 0,
                +0.5f, +0.5f, -0.5f, 1, 0,
                +0.5f, +0.5f, +0.5f, 1, 1,
                +0.5f, -0.5f, +0.5f, 0, 1);
        // 5 左边 -X
        addQuad(5,
                -0.5f, -0.5f, -0.5f, 0, 0,
                -0.5f, -0.5f, +0.5f, 1, 0,
                -0.5f, +0.5f, +0.5f, 1, 1,
                -0.5f, +0.5f, -0.5f, 0, 1);

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

        // 视图立方体配套坐标轴（position(3)+color(3)）
        QVector<float> axisVerts;
        auto pushAxis = [&](float x, float y, float z, float r, float g, float b) {
            axisVerts << x << y << z << r << g << b;
        };
        // 小轴长度/箭头（单位立方体空间）
        const float L = 0.85f;
        const float a = 0.14f;
        const float w = 0.06f;
        // 轴线
        pushAxis(0, 0, 0, 0.90f, 0.20f, 0.20f); pushAxis(L, 0, 0, 0.90f, 0.20f, 0.20f); // X
        pushAxis(0, 0, 0, 0.20f, 0.90f, 0.20f); pushAxis(0, L, 0, 0.20f, 0.90f, 0.20f); // Y
        pushAxis(0, 0, 0, 0.20f, 0.45f, 0.95f); pushAxis(0, 0, L, 0.20f, 0.45f, 0.95f); // Z
        viewCubeAxisLineVertexCount = 6;
        // 箭头（三角形，扁平）
        // X arrow in XY
        pushAxis(L, 0, 0, 0.90f, 0.20f, 0.20f);
        pushAxis(L - a, +w, 0, 0.90f, 0.20f, 0.20f);
        pushAxis(L - a, -w, 0, 0.90f, 0.20f, 0.20f);
        // Y arrow in XY
        pushAxis(0, L, 0, 0.20f, 0.90f, 0.20f);
        pushAxis(+w, L - a, 0, 0.20f, 0.90f, 0.20f);
        pushAxis(-w, L - a, 0, 0.20f, 0.90f, 0.20f);
        // Z arrow in XZ
        pushAxis(0, 0, L, 0.20f, 0.45f, 0.95f);
        pushAxis(+w, 0, L - a, 0.20f, 0.45f, 0.95f);
        pushAxis(-w, 0, L - a, 0.20f, 0.45f, 0.95f);
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

        QImage atlas = buildViewCubeLabelAtlas();
        viewCubeAtlas = new QOpenGLTexture(atlas);
        viewCubeAtlas->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        viewCubeAtlas->setWrapMode(QOpenGLTexture::ClampToEdge);
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
        view.lookAt(eye, orbitCenter, QVector3D(0.0f, 0.0f, 1.0f));

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
                        const QMatrix4x4 chunkMvp = projection * view * chunkModel;
                        meshProgram.setUniformValue("model", chunkModel);
                        meshProgram.setUniformValue("mvp", chunkMvp);
                        meshProgram.setUniformValue("uNormalMat", chunkModel.normalMatrix());
                        f->glDrawArrays(GL_TRIANGLES, ch.firstVertex, ch.vertexCount);
                    }
                } else {
                    f->glDrawArrays(GL_TRIANGLES, 0, meshDrawVertexCount);
                }

                bool anySel = false;
                bool anyAct = false;
                for (const MeshDrawChunk &ch : m_syncedDrawChunks) {
                    if (ch.vertexCount <= 0 || !ch.sceneVisible)
                        continue;
                    if (ch.selected)
                        anySel = true;
                    if (ch.active)
                        anyAct = true;
                }
                if (anySel || anyAct) {
                    f->glEnable(GL_BLEND);
                    f->glBlendFunc(GL_ONE, GL_ONE);
                    f->glBlendEquation(GL_FUNC_ADD);
                    // 叠加高亮会重复绘制同一几何，深度值通常与首遍相等；
                    // 默认 GL_LESS 会导致第二遍全部被深度测试丢弃，从而看不到红/绿高亮。
                    f->glDepthFunc(GL_LEQUAL);
                    f->glDepthMask(GL_FALSE);
                    if (anySel) {
                        meshProgram.setUniformValue("uDrawMode", 1.0f);
                        for (const MeshDrawChunk &ch : m_syncedDrawChunks) {
                            if (!ch.selected || ch.vertexCount <= 0 || !ch.sceneVisible)
                                continue;
                            QMatrix4x4 chunkModel = model;
                            chunkModel.translate(ch.positionOffset);
                            chunkModel.rotate(ch.rotation);
                            const QMatrix4x4 chunkMvp = projection * view * chunkModel;
                            meshProgram.setUniformValue("model", chunkModel);
                            meshProgram.setUniformValue("mvp", chunkMvp);
                            meshProgram.setUniformValue("uNormalMat", chunkModel.normalMatrix());
                            f->glDrawArrays(GL_TRIANGLES, ch.firstVertex, ch.vertexCount);
                        }
                    }
                    if (anyAct) {
                        meshProgram.setUniformValue("uDrawMode", 2.0f);
                        for (const MeshDrawChunk &ch : m_syncedDrawChunks) {
                            if (!ch.active || ch.vertexCount <= 0 || !ch.sceneVisible)
                                continue;
                            QMatrix4x4 chunkModel = model;
                            chunkModel.translate(ch.positionOffset);
                            chunkModel.rotate(ch.rotation);
                            const QMatrix4x4 chunkMvp = projection * view * chunkModel;
                            meshProgram.setUniformValue("model", chunkModel);
                            meshProgram.setUniformValue("mvp", chunkMvp);
                            meshProgram.setUniformValue("uNormalMat", chunkModel.normalMatrix());
                            f->glDrawArrays(GL_TRIANGLES, ch.firstVertex, ch.vertexCount);
                        }
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

        // 旋转控制球（当处于模型旋转模式时显示）
        if (m_syncedRotatingModel && m_syncedRotateModelIndex >= 0 && vaoRotationGizmo.isCreated() && rotationGizmoProgram.bind()) {
            // 构建控制球模型矩阵：位置在模型几何中心 + 旋转
            QMatrix4x4 gizmoModel;
            gizmoModel.translate(m_syncedModelCenter);
            gizmoModel.rotate(m_syncedModelRotation);
            const QMatrix4x4 gizmoMvp = projection * view * gizmoModel;

            rotationGizmoProgram.setUniformValue("uMVP", gizmoMvp);

            // 绘制控制球（半透明）
            f->glEnable(GL_BLEND);
            f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            f->glDepthMask(GL_FALSE);

            vaoRotationGizmo.bind();
            // 球体部分（灰色半透明）
            const int sphereVertCount = 16 * 24 * 6;  // stacks * slices * 6
            f->glDrawArrays(GL_TRIANGLES, 0, sphereVertCount);
            // 坐标轴（实线）
            f->glDisable(GL_BLEND);
            f->glDepthMask(GL_TRUE);
            f->glLineWidth(2.0f);
            const int axisLineStart = sphereVertCount;
            const int axisLineCount = 6;  // 3条轴 * 2个顶点
            f->glDrawArrays(GL_LINES, axisLineStart, axisLineCount);
            // 旋转向量圆环
            const int ringStart = axisLineStart + axisLineCount;
            const int ringCount = rotationGizmoVertexCount - ringStart;
            f->glDrawArrays(GL_TRIANGLES, ringStart, ringCount);
            f->glLineWidth(1.0f);
            vaoRotationGizmo.release();
            rotationGizmoProgram.release();
        }

        // 右上角视图立方体（与平面姿态同步；悬停高亮在 shader）
        if (viewCubeOk && vaoViewCube.isCreated() && viewCubeAtlas && viewCubeProgram.bind()) {
            GLint vpPrev[4] = {};
            f->glGetIntegerv(GL_VIEWPORT, vpPrev);
            const int gw = kGizmoSize;
            const int gh = kGizmoSize;
            const int vx = fbSize.width() - kGizmoMargin - gw;
            const int vy = fbSize.height() - kGizmoMargin - gh;
            f->glViewport(vx, vy, gw, gh);
            f->glDisable(GL_DEPTH_TEST);
            f->glDisable(GL_BLEND);

            const QMatrix4x4 P = viewCubeProj(gw, gh);
            const QMatrix4x4 V = viewCubeView();
            const QMatrix4x4 M = viewCubeModel(m_planeOrientation);
            const QMatrix4x4 mvpGizmo = P * V * M;
            viewCubeProgram.setUniformValue("uMvp", mvpGizmo);
            viewCubeProgram.setUniformValue("uRot", M.normalMatrix());
            viewCubeProgram.setUniformValue("uEyeWorld", QVector3D(0.0f, 0.0f, 2.85f));
            viewCubeProgram.setUniformValue("uHoverFace", m_hoverViewCubeFace);
            viewCubeAtlas->bind(0);
            viewCubeProgram.setUniformValue("uAtlas", 0);
            // 实心立方体：剔除背面（前提：6 个面绕序已统一为外侧 CCW）
            f->glEnable(GL_CULL_FACE);
            f->glCullFace(GL_BACK);
            vaoViewCube.bind();
            f->glDrawArrays(GL_TRIANGLES, 0, viewCubeVertexCount);
            vaoViewCube.release();
            viewCubeProgram.release();
            viewCubeAtlas->release();

            // 立方体坐标轴（与立方体同一旋转）
            if (vaoViewCubeAxis.isCreated() && axisProgram.bind()) {
                axisProgram.setUniformValue("mvp", mvpGizmo);
                vaoViewCubeAxis.bind();
                f->glLineWidth(2.0f);
                f->glDrawArrays(GL_LINES, 0, viewCubeAxisLineVertexCount);
                f->glLineWidth(1.0f);
                f->glDrawArrays(GL_TRIANGLES, viewCubeAxisLineVertexCount,
                                viewCubeAxisVertexCount - viewCubeAxisLineVertexCount);
                vaoViewCubeAxis.release();
                axisProgram.release();
            }

            f->glDisable(GL_CULL_FACE);
            f->glViewport(vpPrev[0], vpPrev[1], vpPrev[2], vpPrev[3]);
            f->glEnable(GL_DEPTH_TEST);
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

        // 同步旋转控制球状态
        m_syncedRotatingModel = vp->m_rotatingModel;
        m_syncedRotateModelIndex = vp->m_rotateModelIndex;
        if (m_syncedRotatingModel && m_syncedRotateModelIndex >= 0 && m_syncedRotateModelIndex < vp->m_meshChunks.size()) {
            m_syncedModelCenter = vp->m_meshChunks[m_syncedRotateModelIndex].positionOffset;
            m_syncedModelRotation = vp->m_meshChunks[m_syncedRotateModelIndex].rotation;
        }

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
        const int activeIdx = vp->m_activeModelIndex;
        for (int i = 0; i < vp->m_meshChunks.size(); ++i) {
            const auto &c = vp->m_meshChunks[i];
            MeshDrawChunk d;
            d.firstVertex = c.firstVertex;
            d.vertexCount = c.vertexCount;
            d.selected = c.selected;
            d.active = (activeIdx >= 0 && i == activeIdx);
            d.sceneVisible = c.sceneVisible;
            d.positionOffset = c.positionOffset;
            d.rotation = c.rotation;
            m_syncedDrawChunks.push_back(d);
        }

        m_syncedPreviewMode = vp->m_previewMode;
        m_syncedTrajNonEmpty = !vp->m_trajSegments.isEmpty();

        const quint64 pv = vp->m_trajPathVersion;
        const qreal pr = vp->m_trajProgress;
        const int dl = vp->m_trajDisplayLayer;
        const bool st = vp->m_previewShowTravel;
        if (pv != m_lastTrajPathVersion || qAbs(pr - m_lastTrajProgress) > 1e-7 || dl != m_lastTrajDisplayLayer
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
    QOpenGLShaderProgram rotationGizmoProgram;
    QOpenGLBuffer vboPlane, vboGrid, vboAxis, vboAxisLabel, vboViewCube, vboMesh, vboTraj;
    QOpenGLVertexArrayObject vaoPlane, vaoGrid, vaoAxis, vaoAxisLabel, vaoViewCube, vaoMesh, vaoTraj;
    QOpenGLBuffer vboViewCubeAxis;
    QOpenGLVertexArrayObject vaoViewCubeAxis;
    QOpenGLTexture *viewCubeAtlas = nullptr;
    QOpenGLTexture *bgTexture = nullptr;
    // 旋转控制球
    QOpenGLBuffer vboRotationGizmo;
    QOpenGLVertexArrayObject vaoRotationGizmo;
    int rotationGizmoVertexCount = 0;
    // Logo
    QOpenGLShaderProgram logoProgram;
    QOpenGLBuffer vboLogo;
    QOpenGLVertexArrayObject vaoLogo;
    QOpenGLTexture *logoTexture = nullptr;
    int logoVertexCount = 0;
    int viewCubeVertexCount = 0;
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
    int m_hoverViewCubeFace = -1;

    quint64 m_syncedMeshVersion = 0;
    quint64 m_meshDiagVersion = 0;
    QVector<MeshDrawChunk> m_syncedDrawChunks;
    std::shared_ptr<QVector<float>> m_meshRenderBuf;
    bool m_meshGpuDirty = false;
    int meshDrawVertexCount = 0;
    // 旋转控制球状态
    bool m_syncedRotatingModel = false;
    int m_syncedRotateModelIndex = -1;
    QVector3D m_syncedModelCenter = QVector3D(0, 0, 0);
    QQuaternion m_syncedModelRotation = QQuaternion();

    bool m_syncedPreviewMode = false;
    bool m_syncedTrajNonEmpty = false;
    QVector<float> m_trajInterleaved;
    bool m_trajDirty = true;
    quint64 m_lastTrajPathVersion = std::numeric_limits<quint64>::max();
    qreal m_lastTrajProgress = -1.0;
    int m_lastTrajDisplayLayer = std::numeric_limits<int>::min();
    bool m_lastTrajShowTravel = true;
    int trajVertexCount = 0;
};

OpenGLViewport::OpenGLViewport(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    qDebug() << "[OpenGLViewport] Constructor called";
    setMirrorVertically(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
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

void OpenGLViewport::resetView()
{
    m_yawDeg = kDefaultYawDeg;
    m_pitchDeg = kDefaultPitchDeg;
    m_distance = kDefaultDistance;
    m_planeOrientation = QQuaternion();
    m_viewPanWorld = QVector3D(0.0f, 0.0f, 0.0f);
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setPreviewMode(bool on)
{
    if (m_previewMode == on)
        return;
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
    m_previewShowTravel = on;
    emit previewShowTravelChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setProgress(qreal p)
{
    const qreal np = qBound(0.0, p, 1.0);
    if (qFuzzyCompare(np, m_trajProgress))
        return;
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
    const int n = m_trajSegments.size();
    if (n <= 0)
        return;
    m_trajCumTimeSec.resize(n + 1);
    m_trajCumTimeSec[0] = 0.0;
    for (int i = 0; i < n; ++i) {
        m_trajTotalTimeSec += qMax(1e-9, double(m_trajSegments[i].durationSec));
        m_trajCumTimeSec[i + 1] = m_trajTotalTimeSec;
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
        return;
    }
    const int n = m_trajSegments.size();
    int visEnd = visibleSegmentCountForProgress(m_trajProgress);
    visEnd = qBound(0, visEnd, n);
    if (visEnd <= 0) {
        if (m_trajPlaybackLine != 1) {
            m_trajPlaybackLine = 1;
            emit playbackLineChanged();
        }
        pushFeed(0.f);
        tipChanged(0.f, 0.f, 0.f);
        return;
    }
    float zCut = 1.0e6f;
    if (!m_trajLayerZs.isEmpty()) {
        const int li = qBound(0, m_trajDisplayLayer, m_trajLayerZs.size() - 1);
        zCut = m_trajLayerZs[li] + 1e-4f;
    }
    int line = 1;
    float nx = 0.f, ny = 0.f, nz = 0.f;
    bool any = false;
    float lastFeed = 0.f;
    for (int i = 0; i < visEnd; ++i) {
        const auto &s = m_trajSegments[i];
        if (qMax(s.az, s.bz) > zCut)
            continue;
        line = s.sourceLine;
        nx = s.bx;
        ny = s.by;
        nz = s.bz;
        any = true;
        lastFeed = s.feedMmMin;
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
    m_yawDeg = kDefaultYawDeg;
    m_pitchDeg = kDefaultPitchDeg;
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
        m.insert(QStringLiteral("selected"), c.selected);
        m.insert(QStringLiteral("active"), c.active);
        m.insert(QStringLiteral("sceneVisible"), c.sceneVisible);
        list.append(m);
    }
    return list;
}

bool OpenGLViewport::allModelsSelected() const
{
    if (m_meshChunks.isEmpty())
        return false;
    for (const ImportedMeshChunk &c : m_meshChunks) {
        if (!c.selected)
            return false;
    }
    return true;
}

void OpenGLViewport::setAllModelsSelected(bool all)
{
    if (m_meshChunks.isEmpty())
        return;
    for (ImportedMeshChunk &c : m_meshChunks)
        c.selected = all;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::ensureOneActiveModel()
{
    // 如果智能选择关闭，则不自动激活任何模型
    if (!m_smartSelectEnabled) {
        m_activeModelIndex = -1;
        return;
    }

    int found = -1;
    for (int i = 0; i < m_meshChunks.size(); ++i) {
        if (m_meshChunks[i].active) {
            if (found >= 0)
                m_meshChunks[i].active = false;
            else
                found = i;
        }
    }
    if (found < 0 && !m_meshChunks.isEmpty()) {
        m_meshChunks[0].active = true;
        found = 0;
    }
    m_activeModelIndex = found;
}

void OpenGLViewport::setSmartSelectEnabled(bool enabled)
{
    if (m_smartSelectEnabled == enabled)
        return;

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

void OpenGLViewport::setModelSelected(int index, bool selected)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;
    m_meshChunks[index].selected = selected;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setModelActive(int index)
{
    // 允许传入-1来取消激活
    if (index < 0 || index >= m_meshChunks.size()) {
        // 取消所有激活状态
        if (m_activeModelIndex >= 0 && m_activeModelIndex < m_meshChunks.size()) {
            m_meshChunks[m_activeModelIndex].active = false;
        }
        m_activeModelIndex = -1;
        emit meshModelsChanged();
        update();
        if (auto *w = window())
            w->update();
        return;
    }
    m_activeModelIndex = index;
    for (int i = 0; i < m_meshChunks.size(); ++i)
        m_meshChunks[i].active = (i == index);
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::setModelSceneVisible(int index, bool visible)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;
    m_meshChunks[index].sceneVisible = visible;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::selectAllModels()
{
    setAllModelsSelected(true);
}

void OpenGLViewport::deselectAllModels()
{
    setAllModelsSelected(false);
}

void OpenGLViewport::deleteModelAt(int index)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;

    if (m_meshChunks.size() == 1) {
        // 只有一个模型，直接清空
        clearAllModels();
        return;
    }

    // 保存要删除的模型的顶点信息
    const auto &deletedChunk = m_meshChunks[index];
    const int deletedFirstVertex = deletedChunk.firstVertex;
    const int deletedVertexCount = deletedChunk.vertexCount;

    // 从列表中移除该模型
    m_meshChunks.removeAt(index);

    // 更新后续模型的 firstVertex
    for (int i = index; i < m_meshChunks.size(); ++i) {
        m_meshChunks[i].firstVertex -= deletedVertexCount;
    }

    // 创建新的顶点缓冲区
    const int totalVerts = m_importedMeshVertexCount - deletedVertexCount;
    auto newBuf = std::make_shared<QVector<float>>();
    newBuf->reserve(totalVerts * 6);

    // 复制删除区域之前的数据
    for (int v = 0; v < deletedFirstVertex; ++v) {
        newBuf->append((*m_importedMeshBuf)[v * 6 + 0]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 1]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 2]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 3]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 4]);
        newBuf->append((*m_importedMeshBuf)[v * 6 + 5]);
    }

    // 复制删除区域之后的数据
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

    // 更新激活状态
    if (m_activeModelIndex == index) {
        // 删除的是激活模型，重置激活状态
        if (!m_meshChunks.isEmpty() && m_smartSelectEnabled) {
            m_activeModelIndex = 0;
            m_meshChunks[0].active = true;
        } else {
            m_activeModelIndex = -1;
        }
    } else if (m_activeModelIndex > index) {
        // 删除的模型在激活模型之前，需要调整索引
        m_activeModelIndex--;
    }

    ++m_meshDataVersion;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::clearAllModels()
{
    m_meshChunks.clear();
    m_importedMeshBuf.reset();
    m_importedMeshVertexCount = 0;
    m_activeModelIndex = -1;
    ++m_meshDataVersion;
    emit meshModelsChanged();
    update();
    if (auto *w = window())
        w->update();
}

void OpenGLViewport::startRotateModel(int index)
{
    if (index < 0 || index >= m_meshChunks.size())
        return;

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

        const int hitMeshIndex = pickMeshAt(m_pressPos);
        if (hitMeshIndex >= 0 && !(m_previewMode && !m_trajSegments.isEmpty())) {
            setModelActive(hitMeshIndex);
            m_draggingMesh = true;
            m_dragMeshIndex = hitMeshIndex;
            m_pressMeshPos = m_meshChunks[hitMeshIndex].positionOffset;
            m_dragging = false;
            setCursor(QCursor(Qt::SizeAllCursor));
            intersectBedPlaneWorld(m_pressPos, m_pressBedHitWorld);
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
            setModelActive(m_pressMeshIndexForMenu);
        }

        m_dragging = true;
        m_draggingMesh = false;
        m_dragMeshIndex = -1;
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
        view.lookAt(eye, orbitCenter, QVector3D(0.0f, 0.0f, 1.0f));
        const QMatrix4x4 invView = view.inverted();
        QVector3D camRight(invView(0, 0), invView(1, 0), invView(2, 0));
        QVector3D camUp(invView(0, 1), invView(1, 1), invView(2, 1));
        camRight.normalize();
        camUp.normalize();
        m_viewPanWorld += camRight * float(-d.x()) * panScale + camUp * (float(d.y()) * panScale);
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
    if (m_rotatingModel && m_rotateModelIndex >= 0 && m_rotateModelIndex < m_meshChunks.size()) {
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
        view.lookAt(eye, orbitCenter, QVector3D(0.0f, 0.0f, 1.0f));
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
        m_meshChunks[m_rotateModelIndex].rotation = newRotation.normalized();
        m_rotateStartQuaternion = m_meshChunks[m_rotateModelIndex].rotation;

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
    view.lookAt(eye, orbitCenter, QVector3D(0.0f, 0.0f, 1.0f));
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
        if (m_draggingMesh) {
            m_draggingMesh = false;
            m_dragMeshIndex = -1;
            unsetCursor();
            event->accept();
            return;
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
                emit contextMenuRequested(m_pressMeshIndexForMenu);
            }
        }
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
    return pickViewCubeFaceImpl(itemPos, iw, ih, fbW, fbH, m_planeOrientation);
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
    view.lookAt(eye, orbitCenter, QVector3D(0.0f, 0.0f, 1.0f));

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
    view.lookAt(eye, orbitCenter, QVector3D(0.0f, 0.0f, 1.0f));

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
    // 点击视图立方体：让“模型/热床”旋转到该面（而不是改相机）
    // Face id: 0 前(+Z) 1 后(-Z) 2 顶(+Y) 3 底(-Y) 4 右(+X) 5 左(-X)
    if (faceIndex < 0 || faceIndex > 5)
        return;

    QQuaternion q;
    switch (faceIndex) {
    case 0: // front
        q = QQuaternion(); // identity
        break;
    case 1: // back
        q = QQuaternion::fromAxisAndAngle(0.0f, 1.0f, 0.0f, 180.0f);
        break;
    case 2: // top (+Y -> +Z)
        q = QQuaternion::fromAxisAndAngle(1.0f, 0.0f, 0.0f, -90.0f);
        break;
    case 3: // bottom (-Y -> +Z)
        q = QQuaternion::fromAxisAndAngle(1.0f, 0.0f, 0.0f, 90.0f);
        break;
    case 4: // right (+X -> +Z)
        q = QQuaternion::fromAxisAndAngle(0.0f, 1.0f, 0.0f, -90.0f);
        break;
    case 5: // left (-X -> +Z)
        q = QQuaternion::fromAxisAndAngle(0.0f, 1.0f, 0.0f, 90.0f);
        break;
    default:
        return;
    }

    m_planeOrientation = q.normalized();
    update();
}

void OpenGLViewport::hoverMoveEvent(QHoverEvent *event)
{
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
            chunk.selected = false;
            chunk.active = false;
            chunk.sceneVisible = true;
            m_meshChunks.push_back(chunk);
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
