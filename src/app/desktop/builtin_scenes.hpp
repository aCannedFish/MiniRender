#pragma once

#include <QString>
#include <minirender/render/path_tracer.hpp>

#include <array>

// 每个示例带独立场景文件和离线建议，导入后的数据仍由普通 SceneEditor 编辑。
struct BuiltinScene {
    QString id;
    QString name;
    QString filename;
    QString description;
    std::string selected_object;
    bool normal_preview;
    minirender::RenderSettings render;
    minirender::IntegratorSettings integrator;
};
const std::array<BuiltinScene, 14>& builtin_scenes();
QString builtin_scene_directory();
