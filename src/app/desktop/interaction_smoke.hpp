#pragma once
#include <QString>
class Workbench;

// 跨多个事件循环回合验证鼠标序列、UI heartbeat、布局与持续渲染。
void start_interaction_smoke(Workbench& window, const QString& directory);
