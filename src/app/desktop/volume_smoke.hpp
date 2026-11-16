// 隐藏窗口回归：volume 的控件操作、渲染结果与资源生命周期检查。

#pragma once

#include <QString>
class Workbench;

// 隐藏窗口中验证区域草稿、历史、文件、后台体积任务和 Vulkan 显示。
void start_volume_smoke(Workbench& window, const QString& directory, bool enforce_response);
