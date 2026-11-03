#pragma once

#include <QString>

class Workbench;

// 通过真实控件提交参数，分别记录 UI 回调和下一帧的耗时。
// 场景由命令行指定，可对同一大网格比较修复前后的响应。
void start_parameter_smoke(Workbench& window, const QString& directory);
