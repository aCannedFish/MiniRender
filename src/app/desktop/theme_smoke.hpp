// 隐藏窗口回归：theme 的控件操作、渲染结果与资源生命周期检查。

#pragma once

#include <QString>
class Workbench;

// 隐藏窗口下检查所有面板的导航、边界和数值步进，并保存含 Vulkan 视口的截图。
void start_theme_smoke(Workbench& window, const QString& directory);
