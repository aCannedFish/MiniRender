#pragma once
#include <QString>
class Workbench;

// 实际窗口中的异步导入、绑定、历史记录和 CPU/GPU 过滤数值对照。
void start_texture_smoke(Workbench& window, const QString& directory);
