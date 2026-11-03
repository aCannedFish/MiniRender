#pragma once
#include <QString>
class Workbench;

// GPU 接触/滤波/合成检查，CPU 遮挡射线独立验证测试场景的近远关系。
void start_ssao_smoke(Workbench& window, const QString& directory);
