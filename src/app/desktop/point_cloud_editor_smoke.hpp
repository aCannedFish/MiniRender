#pragma once
#include <QString>
class Workbench;

// 从预览检查继续，执行真实点云面板、后台任务、场景事务和文件回归。
void start_point_cloud_editor_smoke(Workbench& window, const QString& directory,
                                    bool measure_responsiveness);
