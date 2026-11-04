#pragma once

#include <QString>
class Workbench;

// 通过真实 Qt 控件验证后台几何、历史、文件任务和 Vulkan/CPU 渲染链路。
void start_geometry_smoke(Workbench& window, const QString& directory, bool measure_responsiveness);
