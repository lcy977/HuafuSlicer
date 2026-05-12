#include "GCodeParser.h"

#include <algorithm>

#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QtMath>

namespace GCodeParser {

/** 全角分号/冒号、BOM 等会导致「行尾 feature」整段被忽略 */
static QString normalizeLineSeparators(QString s)
{
    if (!s.isEmpty() && s.at(0) == QChar(0xFEFF))
        s.remove(0, 1);
    s.replace(QChar(0xFF1B), QLatin1Char(';')); // ；
    s.replace(QChar(0xFF1A), QLatin1Char(':')); // ：
    return s;
}

static float parseAxisValue(const QString &line, QChar axis, bool *has)
{
    *has = false;
    const int idx = line.indexOf(axis, 0, Qt::CaseInsensitive);
    if (idx < 0)
        return 0.0f;
    int i = idx + 1;
    if (i < line.size() && line.at(i) == QLatin1Char(' '))
        ++i;
    QString num;
    while (i < line.size()) {
        const QChar c = line.at(i);
        if (c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('+')
            || c == QLatin1Char('e') || c == QLatin1Char('E'))
            num.append(c);
        else
            break;
        ++i;
    }
    bool ok = false;
    const float v = num.toFloat(&ok);
    if (ok)
        *has = true;
    return v;
}

/** 统一特征名：大小写、空格/下划线 → 连字符，便于匹配 Bambu「Outer wall」等 */
static QString normalizedFeatureName(QString raw)
{
    QString s = raw.toUpper();
    s.replace(QLatin1Char(' '), QLatin1Char('-'));
    s.replace(QLatin1Char('_'), QLatin1Char('-'));
    return s;
}

static MoveKind kindFromCuraType(const QString &raw)
{
    const QString t = raw.trimmed();
    // 部分切片在 TYPE/FEATURE 的值或备注里直接写中文（与参考图一致）
    if (t.contains(QStringLiteral("界面支撑")) || t.contains(QStringLiteral("支撑界面"))
        || t.contains(QStringLiteral("接触面支撑")))
        return MoveKind::ExtrudeSupportInterface;
    if (t.contains(QStringLiteral("实心")) && (t.contains(QStringLiteral("填充")) || t.contains(QStringLiteral("填"))))
        return MoveKind::ExtrudeInfillSolid;
    if (t.contains(QStringLiteral("稀疏")) && (t.contains(QStringLiteral("填充")) || t.contains(QStringLiteral("填"))))
        return MoveKind::ExtrudeInfillSparse;
    if (t.contains(QStringLiteral("内壁")) || t.contains(QStringLiteral("内墙")))
        return MoveKind::ExtrudeInnerWall;
    if (t.contains(QStringLiteral("外壁")) || t.contains(QStringLiteral("外墙")))
        return MoveKind::ExtrudeOuterWall;
    if (t.contains(QStringLiteral("表皮")) || t.contains(QStringLiteral("顶底")))
        return MoveKind::ExtrudeSkin;
    if (t.contains(QStringLiteral("裙边")) || t.contains(QStringLiteral("触边"))
        || t.contains(QLatin1String("brim"), Qt::CaseInsensitive)
        || t.contains(QLatin1String("skirt"), Qt::CaseInsensitive))
        return MoveKind::ExtrudeSkirtBrim;
    if (t.contains(QStringLiteral("支撑")))
        return MoveKind::ExtrudeSupport;
    if (t.contains(QStringLiteral("纤维")))
        return MoveKind::ExtrudeFiber;
    if (t.contains(QStringLiteral("填充")))
        return MoveKind::ExtrudeInfill;

    const QString u = normalizedFeatureName(raw);

    // 裙边/边缘（勿放在含 SKIN 的判定之后，以免误伤）
    if (u.contains(QLatin1String("SKIRT")) || u.contains(QLatin1String("BRIM"))
        || u.contains(QLatin1String("PRIME-TOWER")) || u.contains(QLatin1String("PRIMETOWER")))
        return MoveKind::ExtrudeSkirtBrim;

    // 内壁：先判，避免与「OUTER…」类字符串交叉误判
    if (u.contains(QLatin1String("WALL-INNER")) || u.contains(QLatin1String("INNER-WALL"))
        || u.contains(QLatin1String("INNERWALL"))
        || (u.contains(QLatin1String("INNER")) && u.contains(QLatin1String("WALL"))
            && !u.contains(QLatin1String("OUTER")))
        || (u.contains(QLatin1String("INNER")) && u.contains(QLatin1String("PERIMETER")))
        || u.contains(QLatin1String("INTERNAL-PERIMETER")) || u.contains(QLatin1String("INTERNALPERIMETER")))
        return MoveKind::ExtrudeInnerWall;

    if (u.contains(QLatin1String("WALL-OUTER")) || u.contains(QLatin1String("OUTER-WALL"))
        || u.contains(QLatin1String("OUTERWALL"))
        || (u.contains(QLatin1String("OUTER")) && u.contains(QLatin1String("WALL")))
        || u.contains(QLatin1String("EXTERNAL-PERIMETER")) || u.contains(QLatin1String("EXTERNALPERIMETER")))
        return MoveKind::ExtrudeOuterWall;

    // PrusaSlicer 等单独使用「Perimeter」表示外圈
    if (u.contains(QLatin1String("PERIMETER")) && !u.contains(QLatin1String("INTERNAL"))
        && !(u.contains(QLatin1String("INNER")) && u.contains(QLatin1String("PERIMETER"))))
        return MoveKind::ExtrudeOuterWall;

    if (u.contains(QLatin1String("SKIN")) || u.contains(QLatin1String("TOP/BOTTOM"))
        || u.contains(QLatin1String("IRONING")))
        return MoveKind::ExtrudeSkin;

    // 实心 / 稀疏填充（先于泛型 FILL/INFILL）
    if ((u.contains(QLatin1String("SOLID"))
         && (u.contains(QLatin1String("INFILL")) || u.contains(QLatin1String("FILL"))))
        || u.contains(QLatin1String("INTERNAL-SOLID-INFILL"))
        || u.contains(QLatin1String("INTERNALSOLIDINFILL")))
        return MoveKind::ExtrudeInfillSolid;
    if ((u.contains(QLatin1String("SPARSE")) && u.contains(QLatin1String("INFILL")))
        || u.contains(QLatin1String("SPARSE-INFILL")) || u.contains(QLatin1String("SPARSEINFILL")))
        return MoveKind::ExtrudeInfillSparse;

    if (u.contains(QLatin1String("FILL")) || u.contains(QLatin1String("INFILL")))
        return MoveKind::ExtrudeInfill;

    // 界面支撑须先于普通 SUPPORT
    if ((u.contains(QLatin1String("INTERFACE")) && u.contains(QLatin1String("SUPPORT")))
        || u.contains(QLatin1String("SUPPORT-INTERFACE")) || u.contains(QLatin1String("SUPPORTINTERFACE"))
        || u.contains(QLatin1String("INTERFACE-SUPPORT")) || u.contains(QLatin1String("INTERFACESUPPORT")))
        return MoveKind::ExtrudeSupportInterface;
    if (u.contains(QLatin1String("SUPPORT")))
        return MoveKind::ExtrudeSupport;
    if (u.contains(QLatin1String("FIBER")))
        return MoveKind::ExtrudeFiber;
    return MoveKind::ExtrudeOther;
}

/** 行尾/多段注释里的 TYPE、FEATURE 等（不要求 key 在分块开头） */
static void applyFeatureHintsFromCommentSuffix(const QString &suffixIn, MoveKind &curExtrudeKind)
{
    if (suffixIn.isEmpty())
        return;
    QString suffix = normalizeLineSeparators(suffixIn);
    const QString su = suffix.toUpper();
    static const QLatin1String kTags[] = {
        QLatin1String("TYPE:"),
        QLatin1String("FEATURE:"),
        QLatin1String("PRINTING_FEATURE:"),
        QLatin1String("LAYER_TYPE:"),
    };
    for (const QLatin1String &tag : kTags) {
        int from = 0;
        while (true) {
            const int p = su.indexOf(tag, from);
            if (p < 0)
                break;
            const int valStart = p + tag.size();
            int end = suffix.indexOf(QLatin1Char(';'), valStart);
            if (end < 0)
                end = suffix.size();
            QString val = suffix.mid(valStart, end - valStart).trimmed();
            if (!val.isEmpty())
                curExtrudeKind = kindFromCuraType(val);
            from = valStart + 1;
        }
    }
}

QColor colorForKind(MoveKind k)
{
    // 与 PreviewSimulation.qml「特征颜色」图例一致（轨迹顶点色）
    switch (k) {
    case MoveKind::Travel:
        return QColor(QStringLiteral("#ffffff"));
    case MoveKind::ExtrudeOuterWall:
        return QColor(QStringLiteral("#1e6eff"));
    case MoveKind::ExtrudeInnerWall:
        return QColor(QStringLiteral("#32c85a"));
    case MoveKind::ExtrudeInfill:
    case MoveKind::ExtrudeInfillSolid:
    case MoveKind::ExtrudeInfillSparse:
        return QColor(QStringLiteral("#ff963c"));
    case MoveKind::ExtrudeSkin:
        return QColor(QStringLiteral("#ffd250"));
    case MoveKind::ExtrudeSupport:
        return QColor(QStringLiteral("#82878c"));
    case MoveKind::ExtrudeSupportInterface:
        return QColor(QStringLiteral("#22d3ee"));
    case MoveKind::ExtrudeSkirtBrim:
        return QColor(QStringLiteral("#38bdf8"));
    case MoveKind::ExtrudeFiber:
        return QColor(QStringLiteral("#6a6a72"));
    case MoveKind::ExtrudeOther:
    default:
        return QColor(QStringLiteral("#ff785a"));
    }
}

ParseResult parseFile(const QString &filePath, int maxSegments, const ProgressCallback &progress)
{
    ParseResult out;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        out.errorMessage = QStringLiteral("无法打开文件");
        return out;
    }

    {
        const QByteArray all = f.readAll();
        f.seek(0);
        QString raw = QString::fromUtf8(all);
        out.rawText = std::move(raw);
    }

    QTextStream ts(&f);
    float curX = 0, curY = 0, curZ = 0, curE = 0;
    bool absXYZ = true;
    bool absE = true;
    bool mmUnits = true;
    MoveKind curExtrudeKind = MoveKind::ExtrudeOther;
    // 进给 mm/min：在首条 F 出现前使用合理默认，避免时间为 0
    float curFeedMmMin = 4200.f;

    QHash<float, bool> zSet;
    zSet.reserve(4096);

    QStringList allLines;
    while (!ts.atEnd()) {
        QString raw = ts.readLine();
        allLines.append(raw);
    }
    out.sourceLines = std::move(allLines);
    out.lineCount = out.sourceLines.size();

    const float scale = 0.001f;

    const int totalLines = out.sourceLines.size();
    if (progress && totalLines > 0)
        progress(0, totalLines + 1);

    for (int lineNo = 0; lineNo < out.sourceLines.size(); ++lineNo) {
        QString line = normalizeLineSeparators(out.sourceLines[lineNo].trimmed());
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1Char(';'))) {
            const QString cmt = line.mid(1).trimmed();
            applyFeatureHintsFromCommentSuffix(cmt, curExtrudeKind);
            continue;
        }

        const int semi = line.indexOf(QLatin1Char(';'));
        if (semi >= 0) {
            applyFeatureHintsFromCommentSuffix(line.mid(semi + 1), curExtrudeKind);
            line = line.left(semi).trimmed();
        }
        if (line.isEmpty())
            continue;

        const QString u = line.toUpper();
        if (u.startsWith(QLatin1String("G90"))) {
            absXYZ = true;
            absE = true;
            continue;
        }
        if (u.startsWith(QLatin1String("G91"))) {
            absXYZ = false;
            absE = false;
            continue;
        }
        if (u.startsWith(QLatin1String("M82"))) {
            absE = true;
            continue;
        }
        if (u.startsWith(QLatin1String("M83"))) {
            absE = false;
            continue;
        }
        if (u.startsWith(QLatin1String("G21"))) {
            mmUnits = true;
            continue;
        }
        if (u.startsWith(QLatin1String("G20"))) {
            mmUnits = false;
            continue;
        }

        const QStringList tok = line.split(u' ', Qt::SkipEmptyParts);
        if (tok.isEmpty())
            continue;
        const QString cmd = tok[0].toUpper();
        const bool isG0 = (cmd == QLatin1String("G0") || cmd == QLatin1String("G00"));
        const bool isG1 = (cmd == QLatin1String("G1") || cmd == QLatin1String("G01"));
        if (!isG0 && !isG1)
            continue;

        bool hasF = false;
        const float parsedF = parseAxisValue(line, QLatin1Char('F'), &hasF);
        if (hasF && parsedF > 1.f)
            curFeedMmMin = parsedF;

        bool hx = false, hy = false, hz = false, he = false;
        float nx = parseAxisValue(line, QLatin1Char('X'), &hx);
        float ny = parseAxisValue(line, QLatin1Char('Y'), &hy);
        float nz = parseAxisValue(line, QLatin1Char('Z'), &hz);
        float ne = parseAxisValue(line, QLatin1Char('E'), &he);

        const float ax = curX;
        const float ay = curY;
        const float az = curZ;
        const float aE = curE;

        if (hx)
            curX = absXYZ ? nx : (curX + nx);
        if (hy)
            curY = absXYZ ? ny : (curY + ny);
        if (hz)
            curZ = absXYZ ? nz : (curZ + nz);
        if (he)
            curE = absE ? ne : (curE + ne);

        const float s = mmUnits ? scale : (scale * 25.4f);
        const float bx = curX * s;
        const float by = curY * s;
        const float bz = curZ * s;
        const float pax = ax * s;
        const float pay = ay * s;
        const float paz = az * s;

        const bool movedXY = hx || hy || hz;
        if (!movedXY && !he)
            continue;

        const float dE = curE - aE;
        MoveKind kind = MoveKind::Travel;
        if (isG0) {
            kind = MoveKind::Travel;
        } else {
            if (dE > 0.0005f)
                kind = curExtrudeKind;
            else
                kind = MoveKind::Travel;
        }

        // 含仅 E 挤出（无 XY/Z 位移）的段，否则统计/颜色会漏掉螺旋等模式
        if (movedXY || hz || (isG1 && dE > 0.0005f)) {
            if (out.segments.size() >= maxSegments) {
                out.errorMessage = QStringLiteral("轨迹段过多，已截断显示");
                break;
            }
            Segment seg;
            seg.ax = pax;
            seg.ay = pay;
            seg.az = paz;
            seg.bx = bx;
            seg.by = by;
            seg.bz = bz;
            seg.sourceLine = lineNo + 1;
            seg.kind = kind;
            {
                const double dxf = double(curX - ax);
                const double dyf = double(curY - ay);
                const double dzf = double(curZ - az);
                double lenMm = qSqrt(dxf * dxf + dyf * dyf + dzf * dzf);
                if (!mmUnits)
                    lenMm *= 25.4;
                float feedUse = curFeedMmMin;
                if (isG0) {
                    // 空走：通常比挤出快，不低于当前 F
                    feedUse = qMax(curFeedMmMin, 12000.f);
                }
                feedUse = qMax(feedUse, 30.f);
                seg.feedMmMin = feedUse;
                double dur = 0.0;
                if (lenMm > 1e-9)
                    dur = lenMm * 60.0 / double(feedUse);
                dur = qBound(1e-6, dur, 3600.0);
                seg.durationSec = float(dur);
            }
            out.segments.append(seg);
        }

        if (hz) {
            const float zKey = curZ * s;
            zSet.insert(zKey, true);
        }

        if (progress && totalLines > 0
            && ((lineNo & 0xFFF) == 0 || lineNo + 1 == totalLines)) {
            progress(lineNo + 1, totalLines + 1);
        }
    }

    if (progress && totalLines > 0)
        progress(totalLines, totalLines + 1);

    QVector<float> zs;
    zs.reserve(zSet.size());
    for (auto it = zSet.keyBegin(); it != zSet.keyEnd(); ++it)
        zs.append(*it);
    std::sort(zs.begin(), zs.end());
    out.layerZs = std::move(zs);

    out.ok = true;
    return out;
}

} // namespace GCodeParser
