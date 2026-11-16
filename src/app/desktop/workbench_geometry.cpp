// 几何操作的目标校验、后台提交、结果应用及网格导出。

#include "geometry_panel.hpp"
#include "workbench_internal.hpp"
#include <QFileInfo>
#include <algorithm>
#include <minirender/assets/scene_io.hpp>

using namespace workbench_detail;

bool Workbench::finish_geometry_task(const GeometryTaskResult& result) {
    finish_asset_task();
    geometry_notice_.restart();
    return result.error.empty() && !result.cancelled;
}

void Workbench::apply_geometry() {
    const auto& document = editor_.document();
    const auto  object   = std::find_if(document.objects.begin(), document.objects.end(),
                                        [&](const auto& item) { return item.id == selected_id_; });
    if (object == document.objects.end()) {
        throw std::invalid_argument("Select an object before generating geometry");
    }
    if (object->cloth) {
        throw std::invalid_argument("请先冻结布料，再进行网格处理。");
    }
    const auto mesh = std::find_if(document.meshes.begin(), document.meshes.end(),
                                   [&](const auto& item) { return item.id == object->mesh_id; });
    if (mesh == document.meshes.end()) {
        throw std::invalid_argument("Selected mesh is missing");
    }
    const auto        params = geometry_->parameters();
    GeometryTaskInput input;
    input.mesh            = mesh->data;
    input.loop            = params.loop;
    input.height_settings = params.displacement;
    input.simplify        = params.simplify;
    input.remesh          = params.remesh;
    input.adaptive        = params.adaptive;
    if (input.adaptive && input.adaptive->metric == minirender::AdaptiveMetric::screen_pixels) {
        const auto extent = viewport_->swapChainImageSize();
        if (extent.isEmpty()) {
            throw std::runtime_error("Wait for the viewport before using pixel error");
        }
        const auto& camera         = viewport_->camera();
        input.adaptive->projection = minirender::AdaptiveProjection{
            glm::dmat4(camera.projection_matrix(static_cast<float>(extent.width()) /
                                                static_cast<float>(extent.height()))) *
                glm::dmat4(camera.view_matrix()) * glm::dmat4(object->transform.matrix()),
            {static_cast<double>(extent.width()), static_cast<double>(extent.height())}};
    }
    input.target_name = object->name;
    if (params.displacement && params.displacement->scale != 0) {
        const auto height = std::find_if(
            document.textures.begin(), document.textures.end(), [&](const auto& texture) {
                return texture.id == params.height_id &&
                       texture.encoding == minirender::ColorEncoding::linear;
            });
        if (height == document.textures.end()) {
            throw std::invalid_argument("Select a linear height texture before displacement");
        }
        input.height = height->data;
    }
    const auto generation       = document_generation_;
    const auto target           = object->id;
    const auto expected_mesh_id = mesh->id;
    const auto source           = mesh->data;
    begin_asset_task("正在生成网格…");
    try {
        geometry_tasks_->build_mesh(std::move(input), [this, generation, target, expected_mesh_id,
                                                       source](const auto& result) {
            if (!finish_geometry_task(result)) {
                return true;
            }
            const auto& current = editor_.document();
            const auto  current_object =
                std::find_if(current.objects.begin(), current.objects.end(),
                             [&](const auto& item) { return item.id == target; });
            const auto current_mesh =
                std::find_if(current.meshes.begin(), current.meshes.end(),
                             [&](const auto& item) { return item.id == expected_mesh_id; });
            if (generation != document_generation_ || current_object == current.objects.end() ||
                current_object->mesh_id != expected_mesh_id ||
                current_mesh == current.meshes.end() || current_mesh->data != source) {
                return false;
            }
            if (result.mesh != source) {
                editor_.edit_prevalidated(
                    [&](auto& scene) {
                        const auto id = unique_id(scene.meshes, "geometry_");
                        scene.meshes.push_back({id, {}, result.mesh});
                        for (auto& item : scene.objects) {
                            if (item.id == target) {
                                item.mesh_id = id;
                            }
                        }
                    },
                    result.validated_meshes);
                ++document_revision_;
                viewport_->supply_raster_mesh(result.mesh, result.raster);
                refresh();
            }
            if (selected_id_ == target) {
                geometry_->show_result(result);
            }
            return true;
        });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::import_height_file(const QString& path) {
    const auto generation = document_generation_;
    begin_asset_task("正在读取线性高度图…");
    try {
        geometry_tasks_->import_height(native_path(path), [this, generation](const auto& result) {
            if (!finish_geometry_task(result)) {
                return true;
            }
            if (generation != document_generation_) {
                return false;
            }
            std::string id;
            editor_.edit([&](auto& scene) {
                id = unique_id(scene.textures, "height_");
                scene.textures.push_back(
                    {id, result.saved_path, minirender::ColorEncoding::linear, result.height});
            });
            ++document_revision_;
            refresh();
            geometry_->select_height(id);
            return true;
        });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::export_geometry_file(const QString& path) {
    const auto& document = editor_.document();
    const auto  object   = std::find_if(document.objects.begin(), document.objects.end(),
                                        [&](const auto& item) { return item.id == selected_id_; });
    if (object == document.objects.end()) {
        throw std::invalid_argument("Select an object before exporting geometry");
    }
    const auto mesh = std::find_if(document.meshes.begin(), document.meshes.end(),
                                   [&](const auto& item) { return item.id == object->mesh_id; });
    if (mesh == document.meshes.end()) {
        throw std::invalid_argument("Selected mesh is missing");
    }
    begin_asset_task("正在导出 OBJ…");
    try {
        geometry_tasks_->export_obj(document, mesh->data, native_path(path),
                                    [this](const auto& result) {
                                        finish_geometry_task(result);
                                        return true;
                                    });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::save_scene_async(const QString& path) {
    const auto generation = document_generation_;
    const auto revision   = document_revision_;
    begin_asset_task("正在保存场景快照…");
    try {
        geometry_tasks_->save_scene(editor_.document(), native_path(path),
                                    [this, generation, revision, path](const auto& result) {
                                        if (!finish_geometry_task(result)) {
                                            return true;
                                        }
                                        if (generation == document_generation_) {
                                            scene_path_        = path;
                                            const bool changed = revision != document_revision_;
                                            setWindowTitle("MiniRender — " +
                                                           QFileInfo(path).fileName() +
                                                           (changed ? " *" : ""));
                                            if (changed) {
                                                geometry_notice_extra_ = " | 当前还有未保存修改";
                                            }
                                        }
                                        return true;
                                    });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}
