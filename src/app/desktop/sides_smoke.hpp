#pragma once

#include <QString>
class Workbench;

// 从地面上下移动相机，核对单/双面、镜像、线框、拾取及 CPU/GPU 图像。
void start_sides_smoke(Workbench& window, const QString& directory);
