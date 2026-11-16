// 隐藏窗口回归：outline 的控件操作、渲染结果与资源生命周期检查。

#pragma once

#include <QString>
class Workbench;

// 对同色前后表面、折角和斜平面作独立几何对照，并检查模式隔离、强度、缩放与拖拽。
void start_outline_smoke(Workbench& window, const QString& directory, bool measure_response);
