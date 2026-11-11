#pragma once

#include <QString>
class Workbench;

// 隐藏窗口中验证 SPPM 设置、任务边界、快照、焦散模板及显示。
void start_sppm_smoke(Workbench& window, const QString& directory, bool enforce_response);
