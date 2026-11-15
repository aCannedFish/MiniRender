#pragma once

#include <QString>
class Workbench;

// 核对原色、纹理、深度遮挡、接触暗化及光源独立性，保存实际 Vulkan 读回与工作台截图。
void start_preview_smoke(Workbench& window, const QString& directory);
