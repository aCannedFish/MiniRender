#pragma once

#include <QString>
#include <minirender/scene/scene.hpp>
class Workbench;

// 通过真实材质控件验证曲线事务、历史、保存、布局和连续参数编辑。
void start_cloth_material_editor_smoke(Workbench& window, const QString& directory,
                                       minirender::SceneDocument scene, bool enforce_response);
