#include "GCodeParser.h"

#include <algorithm>

#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QtMath>

namespace GCodeParser {

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

static MoveKind kindFromCuraType(const QString &upper)
{
    if (upper.contains(QLatin1String("WALL-OUTER")) || upper.contains(QLatin1String("OUTER-WALL")))
        return MoveKind::ExtrudeOuterWall;
    if (upper.contains(QLatin1String("WALL-INNER")) || upper.contains(QLatin1String("INNER-WALL")))
        return MoveKind::ExtrudeInnerWall;
    if (upper.contains(QLatin1String("SKIN")) || upper.contains(QLatin1String("TOP/BOTTOM")))
        return MoveKind::ExtrudeSkin;
    if (upper.contains(QLatin1String("FILL")) || upper.contains(QLatin1String("INFILL")))
        return MoveKind::ExtrudeInfill;
    if (upper.contains(QLatin1String("SUPPORT")))
        return MoveKind::ExtrudeSupport;
    if (upper.contains(QLatin1String("FIBER")))
        return MoveKind::ExtrudeFiber;
    return MoveKind::ExtrudeOther;
}

QColor colorForKind(MoveKind k)
{
    switch (k) {
    case MoveKind::Travel:
        return QColor(255, 255, 255);
    case MoveKind::ExtrudeOuterWall:
        return QColor(30, 110, 255);
    case MoveKind::ExtrudeInnerWall:
        return QColor(50, 200, 90);
    case MoveKind::ExtrudeInfill:
        return QColor(255, 150, 60);
    case MoveKind::ExtrudeSkin:
        return QColor(255, 210, 80);
    case MoveKind::ExtrudeSupport:
        return QColor(130, 135, 140);
    case MoveKind::ExtrudeFiber:
        return QColor(40, 40, 48);
    case MoveKind::ExtrudeOther:
    default:
        return QColor(255, 120, 90);
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
        constexpr int kMaxPreviewChars = 4'000'000;
        QString raw = QString::fromUtf8(all);
        if (raw.size() > kMaxPreviewChars)
            raw = raw.left(kMaxPreviewChars) + QStringLiteral("\n; … (预览已截断)\n");
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
        QString line = out.sourceLines[lineNo].trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1Char(';'))) {
            const QString cmt = line.mid(1).trimmed().toUpper();
            if (cmt.startsWith(QLatin1String("TYPE:"))) {
                const QString t = cmt.mid(5).trimmed();
                curExtrudeKind = kindFromCuraType(t);
            }
            continue;
        }

        const int semi = line.indexOf(QLatin1Char(';'));
        if (semi >= 0)
            line = line.left(semi).trimmed();
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

        if (movedXY || hz) {
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
