#pragma once

#include <QString>
class Workbench;

// 通过真实控件检查 CPU 任务状态、导出和 Vulkan 显示，并保留参考场景及图像供 CLI 对照。
void start_offline_smoke(Workbench& window, const QString& directory);
