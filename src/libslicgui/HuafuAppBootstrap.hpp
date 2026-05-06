#pragma once

class QQmlApplicationEngine;

/// 供 main.cpp 调用：避免可执行文件翻译单元直接包含 libslic3r 深层头文件。
void huafuAttachSlicerAppToQmlEngine(QQmlApplicationEngine& engine);
