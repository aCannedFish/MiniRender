// 在隐藏窗口测量不同高度、光源半角与多方向光下的半影。
#pragma once

#include <QString>

class Workbench;

// 使用实时控件切换阴影并读回 GPU 帧，测量两块不同高度挡板的半影宽度。
void start_shadow_smoke(Workbench& window, const QString& directory);
