#pragma once

#include <QString>
class Workbench;

// 点云工具的离屏回归入口；先检查独立预览，再检查后台编辑与文件流程。
void start_point_cloud_smoke(Workbench& window, const QString& directory,
                             bool measure_responsiveness = false);
void verify_point_cloud_probe(Workbench& window, const QString& directory, const QString& label);
