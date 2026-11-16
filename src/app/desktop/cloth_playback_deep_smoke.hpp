// 隐藏窗口回归：cloth playback deep 的控件操作、渲染结果与资源生命周期检查。

#pragma once

#include <QString>
class Workbench;

// 接续基础播放回归，检查编辑、离线快照、共享资源和运行中取消。
void start_cloth_playback_deep_smoke(Workbench& window, const QString& directory,
                                     bool enforce_response);
