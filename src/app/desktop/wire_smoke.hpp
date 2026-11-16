// 隐藏窗口回归：wire 的控件操作、渲染结果与资源生命周期检查。

#pragma once

#include <QString>

class Workbench;

// 用前后两片几何验证可见边、隐藏边、HDR 独立性及窗口重建。
void start_wire_smoke(Workbench& window, const QString& directory);
