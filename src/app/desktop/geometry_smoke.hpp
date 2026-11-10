#pragma once

#include <QString>
#include <string>
class Workbench;
class QImage;

// 共用的桌面验证：CPU BVH 对照实际 GPU 法线/深度，以及包含原生视口的 UI 截图。
QImage verify_geometry_gpu(Workbench& window, const QString& directory, const QString& label,
                           const std::string& target_id = "blue_cube");
void verify_geometry_shadows(Workbench& window, const QString& directory);
void verify_geometry_layout(Workbench& window);
void capture_workbench_ui(Workbench& window, const QString& path);

// 通过真实 Qt 控件验证后台几何、历史、文件任务和 Vulkan/CPU 渲染链路。
void start_geometry_smoke(Workbench& window, const QString& directory, bool measure_responsiveness);
