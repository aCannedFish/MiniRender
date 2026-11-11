#pragma once

#include <QString>
class Workbench;

// 离屏核对微圆柱 CPU/GPU 响应、UV 方向、变换、纹理和参数更新。
void start_cloth_smoke(Workbench& window, const QString& directory, bool enforce_response = false);
void verify_cloth_gpu(Workbench& window, const QString& directory, const QString& label);
