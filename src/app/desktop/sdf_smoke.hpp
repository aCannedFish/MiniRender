#pragma once

#include <QString>
class Workbench;

// 隐藏窗口中核对解析球、CPU 图像、HDR 读回及独立视角恢复。
void start_sdf_smoke(Workbench& window, const QString& directory, bool enforce_response = false);
void verify_sdf_gpu(Workbench& window, const QString& directory, const QString& label);
