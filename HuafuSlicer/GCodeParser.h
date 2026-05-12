#pragma once

#include <functional>

#include <QColor>
#include <QString>
#include <QVector>

namespace GCodeParser {

enum class MoveKind : quint8 {
    Travel = 0,
    ExtrudeOuterWall,
    ExtrudeInnerWall,
    /** 未细分的填充（旧 G 代码或无法识别时） */
    ExtrudeInfill,
    ExtrudeSkin,
    ExtrudeSupport,
    ExtrudeFiber,
    ExtrudeOther,
    ExtrudeInfillSolid,
    ExtrudeInfillSparse,
    ExtrudeSupportInterface,
    ExtrudeSkirtBrim,
};

struct Segment {
    float ax = 0, ay = 0, az = 0;
    float bx = 0, by = 0, bz = 0;
    int sourceLine = 0;
    MoveKind kind = MoveKind::Travel;
    /** 本段使用的进给（mm/min），用于预览状态栏 */
    float feedMmMin = 0;
    /** 本段耗时（秒），由长度与进给推算 */
    float durationSec = 0;
};

struct ParseResult {
    QVector<Segment> segments;
    QVector<float> layerZs;
    QStringList sourceLines;
    QString rawText;
    int lineCount = 0;
    QString errorMessage;
    bool ok = false;
};

/** 解析进度：linesDone ∈ [0,lineTotal]，主线程回调由调用方保证 */
using ProgressCallback = std::function<void(int linesDone, int lineTotal)>;

ParseResult parseFile(const QString &filePath, int maxSegments = 2'000'000,
                      const ProgressCallback &progress = {});

QColor colorForKind(MoveKind k);

} // namespace GCodeParser
