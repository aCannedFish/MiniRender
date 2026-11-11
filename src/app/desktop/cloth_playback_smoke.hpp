#pragma once

#include <QString>
class Workbench;

// 隐藏窗口中验证真实固定步播放、动态缓冲、状态文件和冻结历史。
void start_cloth_playback_smoke(Workbench& window, const QString& directory, bool enforce_response);
