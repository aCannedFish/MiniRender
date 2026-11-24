// 工具栏的直接导入与覆盖导入子菜单，数据与可搜索目录共用。
#pragma once

#include "resource_catalog.hpp"

class QMenu;

// 在菜单下建立两个导入入口。用户触发条目时，将其元数据与导入方式传给工作台。
void add_quick_import_actions(
    QMenu& menu, std::function<void(const catalog::Entry&, catalog::ImportAction)> on_import);
