// 隐藏窗口回归：ibl 的控件操作、渲染结果与资源生命周期检查。

#pragma once
#include <QString>
class Workbench;

// 真实环境编辑/后台控制与 Vulkan 回读，对照独立的 CPU 求交及 IBL 数据采样。
void start_ibl_smoke(Workbench& window, const QString& directory);
