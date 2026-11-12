#pragma once

#include <QString>
class Workbench;

// 隐藏窗口验证 SSS 草稿、参数、实时近似、不可变离线任务及图像显示。
void start_subsurface_smoke(Workbench& window, const QString& directory, bool enforce_response);
