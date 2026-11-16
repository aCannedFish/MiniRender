// 隐藏窗口回归：texture 的控件操作、渲染结果与资源生命周期检查。

#pragma once
#include <QString>
class Workbench;

// 实际窗口中的异步导入、绑定、历史记录和 CPU/GPU 过滤数值对照。
void start_texture_smoke(Workbench& window, const QString& directory);
