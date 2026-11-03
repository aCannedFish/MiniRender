#pragma once

#include <QString>

class Workbench;

// 用前后两片几何验证可见边、隐藏边、HDR 独立性及窗口重建。
void start_wire_smoke(Workbench& window, const QString& directory);
