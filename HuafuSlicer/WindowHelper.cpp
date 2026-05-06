#include "WindowHelper.h"

#include <QQuickWindow>

WindowHelper::WindowHelper(QObject *parent)
    : QObject(parent)
{
}

void WindowHelper::startSystemMove(QObject *windowObject) const
{
    if (auto *w = qobject_cast<QQuickWindow *>(windowObject))
        w->startSystemMove();
}
