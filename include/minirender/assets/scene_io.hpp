#pragma once

#include <minirender/scene/scene.hpp>

#include <functional>

namespace minirender {

// 文件入口统一使用 filesystem::path；Windows 非 ASCII 文件名不经窄字符串 fopen。
// OBJ 仅导入几何与面角属性，材质由场景 JSON 指定，不读取 MTL 中的渲染模型。
MeshData load_obj(const std::filesystem::path& path);
SceneDocument load_scene(const std::filesystem::path& path);

using MeshWriteProgress = std::function<void(std::size_t completed, std::size_t total)>;

// 共享位置写为 v，面角分别写为 vt/vn；法线导出为单位方向，与读取器一致。
// 使用最大 float 往返精度和经典数字格式。
// 在目标旁写临时文件，全部写出且未取消后原子替换目标；失败保留原文件。
// 进度单位为位置/UV/法线/面记录，不包含最后一次不可取消的文件名提交。
void save_obj(const MeshData& mesh, const std::filesystem::path& path,
              CancellationToken cancellation = {}, MeshWriteProgress progress = {});

enum class SceneSaveStage { validation, mesh, hash, compare, copy, metadata, commit };
struct SceneSaveProgress {
    SceneSaveStage stage = SceneSaveStage::validation;
    std::size_t asset_index = 0;
    std::size_t asset_count = 0;
    std::size_t completed = 0;
    std::size_t total = 1;
};
using SceneSaveCallback = std::function<void(const SceneSaveProgress&)>;

// 另存时重算已有资产路径。source 为空的生成网格写入内容命名的新 OBJ，不覆盖输入。
// JSON 在最后原子替换；失败/取消保留原 JSON，可能留下尚未引用的新资产。
// 不修改原 document。回调在调用线程触发；commit 0/1 为最后取消点，返回表示提交完成。
void save_scene(const SceneDocument& document, const std::filesystem::path& path,
                CancellationToken cancellation = {}, SceneSaveCallback progress = {});

} // namespace minirender
