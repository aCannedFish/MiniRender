#pragma once

#include <minirender/scene/scene.hpp>

namespace minirender {

// 文件入口统一使用 filesystem::path；Windows 非 ASCII 文件名不经窄字符串 fopen。
// OBJ 仅导入几何与面角属性，材质由场景 JSON 指定，不读取 MTL 中的渲染模型。
MeshData load_obj(const std::filesystem::path& path);
SceneDocument load_scene(const std::filesystem::path& path);

// 另存位置变化时重算相对资产路径，保持对同一模型的引用；不修改原 document。
void save_scene(const SceneDocument& document, const std::filesystem::path& path);

} // namespace minirender
