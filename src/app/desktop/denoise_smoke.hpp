// 隐藏窗口验证帧序列、离线结果与实时 SSGI 的空间和时间滤波。
#pragma once
class Workbench;
class QString;
void start_denoise_smoke(Workbench& window, const QString& directory);
