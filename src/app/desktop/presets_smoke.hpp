#pragma once
#include <QString>
class Workbench;

// 通过快速导入动作覆盖场景、编辑并保存，再核对实际 CPU 任务及 GPU 显示。
void start_presets_smoke(Workbench& window, const QString& directory);
