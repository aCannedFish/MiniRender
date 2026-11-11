#pragma once

#include <QString>
class Workbench;

// 使用实际控件检查 CSG、历史、文件、连续拖拽、滚动布局和事件响应。
void start_sdf_editor_smoke(Workbench& window, const QString& directory, bool enforce_response);
