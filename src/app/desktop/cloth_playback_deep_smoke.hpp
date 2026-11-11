#pragma once

#include <QString>
class Workbench;

// 接续基础播放回归，检查编辑、离线快照、共享资源和运行中取消。
void start_cloth_playback_deep_smoke(Workbench& window, const QString& directory,
                                     bool enforce_response);
