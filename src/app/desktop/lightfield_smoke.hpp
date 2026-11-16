// 隐藏窗口回归：lightfield 的控件操作、渲染结果与资源生命周期检查。

#pragma once
#include <QString>
class Workbench;
// 隐藏窗口验证光场采集、重建、文件和多个后台图像来源之间的切换。
void start_lightfield_smoke(Workbench&, const QString& directory, bool enforce_response);
