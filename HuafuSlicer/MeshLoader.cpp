#include "MeshLoader.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QDebug>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QtMath>
#include <QVector3D>

#define MESH_LOG qInfo() << "[MeshLoader]" << QThread::currentThreadId()

namespace {

static void appendTri(QVector<float> &out, const QVector3D &a, const QVector3D &b, const QVector3D &c,
                      const QVector3D &n)
{
    // 性能关键路径：避免大量 operator<< 造成的高开销 append
    const QVector3D nn = n.normalized();
    const int base = out.size();
    out.resize(base + 18);
    float *dst = out.data() + base;
    auto writeV = [&](const QVector3D &p) {
        *dst++ = p.x();
        *dst++ = p.y();
        *dst++ = p.z();
        *dst++ = nn.x();
        *dst++ = nn.y();
        *dst++ = nn.z();
    };
    writeV(a);
    writeV(b);
    writeV(c);
}

static QVector3D triNormal(const QVector3D &a, const QVector3D &b, const QVector3D &c)
{
    const QVector3D e1 = b - a;
    const QVector3D e2 = c - a;
    QVector3D n = QVector3D::crossProduct(e1, e2);
    if (n.lengthSquared() < 1e-20f)
        return QVector3D(0, 0, 1);
    return n.normalized();
}

/** OBJ 常见 Y-up：映射到场景 XY 热床、Z 向上 */
static QVector3D mapObjVertex(const QVector3D &v)
{
    return QVector3D(v.x(), v.z(), v.y());
}

static QVector3D mapObjNormal(const QVector3D &n)
{
    return QVector3D(n.x(), n.z(), n.y()).normalized();
}

/** 仅统一长度单位（如 mm→m），保留文件内顶点坐标与姿态。 */
static void applyUnitScaleToMeters(QVector<float> &io, float unitScale)
{
    if (io.isEmpty() || qFuzzyCompare(unitScale, 1.0f))
        return;

    const int n = io.size() / 6;
    for (int i = 0; i < n; ++i) {
        io[i * 6 + 0] *= unitScale;
        io[i * 6 + 1] *= unitScale;
        io[i * 6 + 2] *= unitScale;
    }

    if (n <= 0)
        return;
    QVector3D bmin(1e30f, 1e30f, 1e30f);
    QVector3D bmax(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < n; ++i) {
        bmin.setX(qMin(bmin.x(), io[i * 6 + 0]));
        bmin.setY(qMin(bmin.y(), io[i * 6 + 1]));
        bmin.setZ(qMin(bmin.z(), io[i * 6 + 2]));
        bmax.setX(qMax(bmax.x(), io[i * 6 + 0]));
        bmax.setY(qMax(bmax.y(), io[i * 6 + 1]));
        bmax.setZ(qMax(bmax.z(), io[i * 6 + 2]));
    }
    MESH_LOG << "applyUnitScale verts=" << n << "unitScale=" << unitScale << "bmin" << bmin << "bmax" << bmax;
}

static float guessUnitScaleToMeters(const QVector<float> &io, float bedExtent)
{
    if (io.isEmpty())
        return 1.0f;
    const int n = io.size() / 6;
    if (n <= 0)
        return 1.0f;

    QVector3D bmin(1e30f, 1e30f, 1e30f);
    QVector3D bmax(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < n; ++i) {
        const float x = io[i * 6 + 0];
        const float y = io[i * 6 + 1];
        const float z = io[i * 6 + 2];
        bmin.setX(qMin(bmin.x(), x));
        bmin.setY(qMin(bmin.y(), y));
        bmin.setZ(qMin(bmin.z(), z));
        bmax.setX(qMax(bmax.x(), x));
        bmax.setY(qMax(bmax.y(), y));
        bmax.setZ(qMax(bmax.z(), z));
    }
    const QVector3D extent = bmax - bmin;
    const float maxDim = qMax(qMax(extent.x(), extent.y()), extent.z());

    // STL/OBJ 通常以 mm 保存：一个 100mm 的零件 maxDim≈100。
    // 若直接按“m”解释会变成 100m，远超 1m 热床，因此用阈值启发式自动换算到米。
    if (maxDim > (bedExtent * 5.0f))
        return 0.001f; // mm -> m
    return 1.0f; // already meters (or user intentionally authored in meters)
}

} // namespace

bool MeshLoader::loadTriangleMesh(const QString &path, QVector<float> &outInterleaved, QString *errorMessage)
{
    QElapsedTimer total;
    total.start();
    outInterleaved.clear();
    const QFileInfo fi(path);
    if (!fi.exists()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("文件不存在：%1").arg(path);
        MESH_LOG << "loadTriangleMesh FAIL not exist" << path;
        return false;
    }

    const qint64 fsz = fi.size();
    MESH_LOG << "loadTriangleMesh START path=" << path << "sizeBytes=" << fsz;

    const QString suf = fi.suffix().toLower();
    bool ok = false;
    if (suf == QLatin1String("stl"))
        ok = loadStl(path, outInterleaved, errorMessage);
    else if (suf == QLatin1String("obj"))
        ok = loadObj(path, outInterleaved, errorMessage);
    else {
        if (errorMessage)
            *errorMessage = QStringLiteral("不支持的格式（请使用 .stl 或 .obj）：%1").arg(suf);
        MESH_LOG << "loadTriangleMesh FAIL bad suffix" << suf;
        return false;
    }

    MESH_LOG << "loadTriangleMesh END ok=" << ok << "elapsedMs=" << total.elapsed()
             << "floats=" << outInterleaved.size() << "verts=" << (outInterleaved.size() / 6)
             << "tris=" << (outInterleaved.size() / 18);
    return ok;
}

bool MeshLoader::loadStl(const QString &path, QVector<float> &out, QString *errorMessage)
{
    QElapsedTimer t;
    t.start();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开文件");
        MESH_LOG << "loadStl open fail" << path;
        return false;
    }

    const qint64 fileSize = f.size();
    quint32 nTriBin = 0;
    bool binary = false;
    if (fileSize >= 84) {
        f.seek(80);
        QDataStream ds(&f);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds >> nTriBin;
        const qint64 expected = 80 + 4 + qint64(nTriBin) * 50;
        if (ds.status() == QDataStream::Ok && nTriBin > 0u && nTriBin < 200000000u && expected == fileSize)
            binary = true;
    }
    f.seek(0);

    // 头部长度与计数略不一致时，仍按「84 字节后每三角 50 字节」识别二进制，避免整文件当 ASCII 正则卡死
    if (!binary && fileSize >= 134) {
        const qint64 payload = fileSize - 84;
        if (payload > 0 && (payload % 50) == 0) {
            const quint32 nGuess = quint32(payload / 50);
            if (nGuess > 0u && nGuess < 200000000u) {
                nTriBin = nGuess;
                binary = true;
                MESH_LOG << "loadStl relaxed BINARY nTri=" << nTriBin << "fileSize=" << fileSize;
            }
        }
    }
    f.seek(0);

    if (!binary) {
        constexpr qint64 kMaxAsciiStlBytes = 80ll * 1024 * 1024;
        if (fileSize > kMaxAsciiStlBytes) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "ASCII STL 过大（>%1 MB），请改用二进制 STL 或缩小文件。")
                                  .arg(kMaxAsciiStlBytes / (1024 * 1024));
            }
            MESH_LOG << "loadStl ASCII too large bytes=" << fileSize;
            return false;
        }
        // ASCII STL 用逐行解析，避免整文件读入+正则扫描在大文件上非常慢
        MESH_LOG << "loadStl ASCII parse (stream) begin bytes=" << fileSize;
        out.clear();
        QTextStream ts(&f);
        QVector3D fn;
        QVector3D v[3];
        int gotV = 0;
        qint64 facets = 0;
        while (!ts.atEnd()) {
            const QString line = ts.readLine().trimmed();
            if (line.isEmpty())
                continue;
            if (line.startsWith(QLatin1String("facet normal "))) {
                const QStringList p = line.split(u' ', Qt::SkipEmptyParts);
                if (p.size() >= 5) {
                    fn = QVector3D(p[2].toFloat(), p[3].toFloat(), p[4].toFloat());
                    gotV = 0;
                }
                continue;
            }
            if (line.startsWith(QLatin1String("vertex "))) {
                const QStringList p = line.split(u' ', Qt::SkipEmptyParts);
                if (p.size() >= 4 && gotV < 3) {
                    v[gotV] = QVector3D(p[1].toFloat(), p[2].toFloat(), p[3].toFloat());
                    ++gotV;
                    if (gotV == 3) {
                        QVector3D n = fn;
                        if (n.lengthSquared() < 1e-12f)
                            n = triNormal(v[0], v[1], v[2]);
                        appendTri(out, v[0], v[1], v[2], n);
                        ++facets;
                    }
                }
                continue;
            }
        }
        MESH_LOG << "loadStl ASCII tris=" << facets << "ms=" << t.elapsed();
    } else {
        const quint32 nTri = nTriBin;
        if (nTri == 0u || nTri > 100000000u) {
            if (errorMessage)
                *errorMessage = QStringLiteral("无效的 STL 二进制文件");
            MESH_LOG << "loadStl binary bad nTri" << nTri;
            return false;
        }
        f.seek(84);
        out.clear();
        out.reserve(int(nTri) * 18);
        MESH_LOG << "loadStl BINARY fast read nTri=" << nTri;

        // 一次性读入主体，减少 per-triangle read 系统调用
        const qint64 payloadBytes = qint64(nTri) * 50;
        const QByteArray payload = f.read(payloadBytes);
        if (payload.size() != payloadBytes) {
            if (errorMessage)
                *errorMessage = QStringLiteral("STL 数据不完整");
            out.clear();
            return false;
        }
        const char *p = payload.constData();
        for (quint32 i = 0; i < nTri; ++i) {
            const float *buf = reinterpret_cast<const float *>(p);
            QVector3D n(buf[0], buf[1], buf[2]);
            QVector3D v0(buf[3], buf[4], buf[5]);
            QVector3D v1(buf[6], buf[7], buf[8]);
            QVector3D v2(buf[9], buf[10], buf[11]);
            if (n.lengthSquared() < 1e-12f)
                n = triNormal(v0, v1, v2);
            appendTri(out, v0, v1, v2, n);
            p += 50; // 12 floats (48 bytes) + 2 bytes attribute
        }
        MESH_LOG << "loadStl BINARY done tris=" << nTri << "ms=" << t.elapsed();
    }

    if (out.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("STL 中未解析到三角形");
        MESH_LOG << "loadStl empty result";
        return false;
    }

    const float unitScale = guessUnitScaleToMeters(out, kBedPlaneExtentMeters);
    QElapsedTimer placeT;
    placeT.start();
    applyUnitScaleToMeters(out, unitScale);
    MESH_LOG << "loadStl unitScale ms=" << placeT.elapsed() << "unitScale=" << unitScale << "totalMs=" << t.elapsed();
    return true;
}

bool MeshLoader::loadObj(const QString &path, QVector<float> &out, QString *errorMessage)
{
    QElapsedTimer t;
    t.start();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开文件");
        MESH_LOG << "loadObj open fail" << path;
        return false;
    }

    QVector<QVector3D> vp(1); // 1-based
    QVector<QVector3D> vn(1);

    QTextStream ts(&f);
    int lineNo = 0;
    while (!ts.atEnd()) {
        ++lineNo;
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        if (line.startsWith(QLatin1String("v "))) {
            const QStringList p = line.split(u' ', Qt::SkipEmptyParts);
            if (p.size() >= 4) {
                QVector3D v(p[1].toFloat(), p[2].toFloat(), p[3].toFloat());
                vp.append(mapObjVertex(v));
            }
            continue;
        }
        if (line.startsWith(QLatin1String("vn "))) {
            const QStringList p = line.split(u' ', Qt::SkipEmptyParts);
            if (p.size() >= 4) {
                QVector3D n(p[1].toFloat(), p[2].toFloat(), p[3].toFloat());
                vn.append(mapObjNormal(n));
            }
            continue;
        }

        if (!line.startsWith(QLatin1String("f ")))
            continue;

        const QStringList parts = line.split(u' ', Qt::SkipEmptyParts);
        QVector<int> idxV;
        QVector<int> idxN;
        idxV.reserve(parts.size() - 1);
        idxN.reserve(parts.size() - 1);
        for (int i = 1; i < parts.size(); ++i) {
            const QStringList tok = parts[i].split(u'/', Qt::KeepEmptyParts);
            if (tok.isEmpty())
                continue;
            const int vi = tok[0].toInt();
            int ni = 0;
            if (tok.size() >= 3 && !tok[2].isEmpty())
                ni = tok[2].toInt();

            int avi = vi;
            if (vi < 0)
                avi = vp.size() + vi;
            if (avi < 1 || avi >= vp.size())
                continue;

            idxV.append(avi);
            if (ni < 0)
                ni = vn.size() + ni;
            idxN.append(ni);
        }

        if (idxV.size() < 3)
            continue;

        const QVector3D &p0 = vp[idxV[0]];
        QVector3D n0, n1, n2;
        if (idxN.size() == idxV.size() && idxN[0] > 0 && idxN[0] < vn.size()) {
            n0 = vn[idxN[0]];
        } else {
            n0 = QVector3D();
        }

        for (int k = 1; k + 1 < idxV.size(); ++k) {
            const QVector3D &p1 = vp[idxV[k]];
            const QVector3D &p2 = vp[idxV[k + 1]];
            QVector3D nn = triNormal(p0, p1, p2);
            if (idxN.size() == idxV.size() && idxN[0] > 0 && idxN[0] < vn.size()
                && idxN[k] > 0 && idxN[k] < vn.size() && idxN[k + 1] > 0 && idxN[k + 1] < vn.size()) {
                nn = ((vn[idxN[0]] + vn[idxN[k]] + vn[idxN[k + 1]]) / 3.0f).normalized();
            }
            appendTri(out, p0, p1, p2, nn);
        }
    }

    if (out.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("OBJ 中未解析到面");
        MESH_LOG << "loadObj empty lines=" << lineNo << "ms=" << t.elapsed();
        return false;
    }

    const float unitScale = guessUnitScaleToMeters(out, kBedPlaneExtentMeters);
    QElapsedTimer placeT;
    placeT.start();
    applyUnitScaleToMeters(out, unitScale);
    MESH_LOG << "loadObj verts=" << vp.size() - 1 << "lines=" << lineNo << "floats=" << out.size()
             << "unitScaleMs=" << placeT.elapsed() << "unitScale=" << unitScale << "totalMs=" << t.elapsed();
    return true;
}
