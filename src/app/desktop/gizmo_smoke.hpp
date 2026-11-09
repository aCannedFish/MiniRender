#pragma once

#include <QString>
class Workbench;

// 使用真实 Qt 事件和 GPU 帧验证物体操纵、单次历史、取消、文件与离线显示。
void start_gizmo_smoke(Workbench& window, const QString& directory, bool measure_responsiveness);
