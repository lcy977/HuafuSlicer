#pragma once

#include <QObject>

class WindowHelper : public QObject
{
    Q_OBJECT

public:
    explicit WindowHelper(QObject *parent = nullptr);

    /// 使用系统拖动（推荐），避免无边框窗口反复 setPosition 导致的闪烁 / 抖动。
    Q_INVOKABLE void startSystemMove(QObject *windowObject) const;
};
