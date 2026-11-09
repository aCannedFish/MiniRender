#pragma once
#include <QString>
class Workbench;

// 自适应几何任务的完整编辑/渲染回归，测试使用离屏视口。
void start_adaptive_smoke(Workbench& window, const QString& directory,
                          bool measure_responsiveness = false);
