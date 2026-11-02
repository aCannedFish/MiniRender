#pragma once

#include <QString>
class Workbench;

// 自动化真实 UI 控件与 Vulkan 回读；异步推进步骤，保留每个关键状态的图像。
void start_scene_smoke(Workbench& window, const QString& directory);
