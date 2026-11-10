#include "geometry_controller.hpp"
#include "point_cloud_panel.hpp"
#include "scene_id.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>
#include <stdexcept>

namespace {
using minirender::desktop::unique_id;

// 所有读取都只查询对象与资产引用，原始顶点留在 worker 和不可变网格中。
const minirender::SceneObject* object_for(const minirender::SceneDocument& scene,
                                          const std::string& id) {
    const auto found = std::find_if(scene.objects.begin(), scene.objects.end(),
                                    [&](const auto& object) { return object.id == id; });
    return found == scene.objects.end() ? nullptr : &*found;
}
const minirender::MeshAsset* mesh_for(const minirender::SceneDocument& scene,
                                      const minirender::SceneObject* object) {
    if (!object) {
        return nullptr;
    }
    const auto found = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                    [&](const auto& mesh) { return mesh.id == object->mesh_id; });
    return found == scene.meshes.end() ? nullptr : &*found;
}
} // namespace

PointCloudPanel* Workbench::point_cloud_panel() const noexcept {
    return points_;
}

void Workbench::connect_point_panel() {
    const auto invoke = [this](const std::function<void()>& work) {
        try {
            work();
        } catch (const std::exception& error) {
            report_error(error);
        }
    };
    points_->on_import = [this, invoke] {
        const auto path =
            QFileDialog::getOpenFileName(this, "导入点云", {}, "PLY point clouds (*.ply)");
        if (!path.isEmpty()) {
            invoke([&] { import_point_file(path); });
        }
    };
    points_->on_sample = [this, invoke] { invoke([&] { sample_point_cloud(); }); };
    points_->on_prepare = [this, invoke] { invoke([&] { prepare_point_cloud_normals(); }); };
    points_->on_reconstruct = [this, invoke] { invoke([&] { reconstruct_point_cloud(); }); };
    points_->on_export = [this, invoke] {
        const auto path =
            QFileDialog::getSaveFileName(this, "导出当前点云", {}, "PLY point clouds (*.ply)");
        if (!path.isEmpty()) {
            invoke([&] { export_point_file(path); });
        }
    };
    points_->on_preview = [this, invoke](bool visible) {
        invoke([&] {
            if (visible) {
                viewport_->set_point_preview_data(points_->preview_data());
                viewport_->set_point_preview_settings(points_->display_parameters());
            }
            viewport_->set_point_preview_visible(visible);
        });
    };
    points_->on_locate = [this, invoke] {
        invoke([&] {
            viewport_->set_point_preview_data(points_->preview_data());
            viewport_->set_point_preview_visible(true);
            viewport_->reset_point_camera();
        });
    };
    points_->on_display = [this](const auto& settings) {
        viewport_->set_point_preview_settings(settings);
    };
    viewport_->on_point_preview_changed = [this](bool visible) {
        points_->set_preview_visible(visible);
        auto* hdr = findChild<QAction*>("export_hdr");
        hdr->setEnabled(!visible);
        hdr->setToolTip(visible ? "点云预览可截图，返回场景后可导出 EXR。"
                                : "导出当前线性 HDR 图像");
    };
}

void Workbench::update_point_selection() {
    const auto& document = editor_.document();
    const auto* selected = object_for(document, selected_id_);
    points_->set_selected_mesh(mesh_for(document, selected) != nullptr,
                               selected ? QString::fromStdString(selected->name) : QString{});
    const auto& source = points_->source();
    const auto* target = object_for(document, points_->target_id());
    if (source.object_id.empty() && !target) {
        points_->set_target_state(true, true);
    } else {
        const auto* mesh = mesh_for(document, target);
        points_->set_target_state(mesh && points_->allows_mesh(mesh->data), false);
    }
}

void Workbench::import_point_file(const QString& path) {
    const auto source = std::filesystem::path(path.toStdU16String());
    const auto name = QFileInfo(path).completeBaseName().toStdString();
    const auto generation = document_generation_;
    const auto revision = points_->source_revision();
    begin_asset_task("正在读取点云…");
    try {
        geometry_tasks_->import_points(source, [this, source, name, generation,
                                                revision](const auto& result) {
            if (!finish_geometry_task(result)) {
                return true;
            }
            if (generation != document_generation_ || revision != points_->source_revision()) {
                return false;
            }
            points_->set_source({result.point_cloud, source, name, {}, {}}, result.point_preview);
            viewport_->set_point_preview_data(result.point_preview);
            viewport_->set_point_preview_settings(points_->display_parameters());
            viewport_->reset_point_camera();
            viewport_->set_point_preview_visible(true);
            update_point_selection();
            return true;
        });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::sample_point_cloud() {
    const auto& document = editor_.document();
    const auto* object = object_for(document, selected_id_);
    const auto* mesh = mesh_for(document, object);
    if (!mesh) {
        throw std::invalid_argument("请先选择要采样的网格物体。");
    }
    const auto source_mesh = mesh->data;
    const auto target = object->id;
    const auto name = object->name;
    const auto generation = document_generation_;
    const auto revision = points_->source_revision();
    const auto count = points_->sample_count();
    const auto seed = points_->sample_seed();
    begin_asset_task("正在采样表面点云…");
    try {
        geometry_tasks_->sample_points(
            source_mesh, count, seed,
            [this, source_mesh, target, name, generation, revision](const auto& result) {
                if (!finish_geometry_task(result)) {
                    return true;
                }
                const auto& current = editor_.document();
                const auto* current_mesh = mesh_for(current, object_for(current, target));
                if (generation != document_generation_ || revision != points_->source_revision() ||
                    !current_mesh || current_mesh->data != source_mesh) {
                    return false;
                }
                points_->set_source({result.point_cloud, {}, name, target, source_mesh},
                                    result.point_preview);
                viewport_->set_point_preview_data(result.point_preview);
                viewport_->set_point_preview_settings(points_->display_parameters());
                viewport_->reset_point_camera();
                viewport_->set_point_preview_visible(true);
                update_point_selection();
                return true;
            });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::prepare_point_cloud_normals() {
    const auto source = points_->source().cloud;
    if (!source) {
        throw std::invalid_argument("请先导入或采样点云。");
    }
    const auto parameters = points_->normal_parameters();
    const auto generation = document_generation_;
    const auto revision = points_->source_revision();
    begin_asset_task("正在准备点云法线…");
    try {
        geometry_tasks_->prepare_points(
            source, parameters, [this, generation, revision, source](const auto& result) {
                if (!finish_geometry_task(result)) {
                    return true;
                }
                if (generation != document_generation_ || revision != points_->source_revision() ||
                    source != points_->source().cloud) {
                    return false;
                }
                points_->show_normals(result.point_normals, result.point_preview);
                viewport_->set_point_preview_data(result.point_preview);
                viewport_->set_point_preview_settings(points_->display_parameters());
                viewport_->set_point_preview_visible(true);
                return true;
            });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::reconstruct_point_cloud() {
    const auto source = points_->source();
    if (!source.cloud) {
        throw std::invalid_argument("请先导入或采样点云。");
    }
    auto input = points_->reconstruction_parameters();
    const auto generation = document_generation_;
    const auto revision = points_->source_revision();
    const auto fit = points_->fit_new_object();
    std::string expected_mesh_id;
    std::shared_ptr<const minirender::MeshData> expected_mesh;
    const auto candidate_id = points_->target_id();
    std::string target_id;
    if (!source.object_id.empty() || object_for(editor_.document(), candidate_id)) {
        const auto& document = editor_.document();
        const auto* mesh = mesh_for(document, object_for(document, candidate_id));
        if (!mesh || !points_->allows_mesh(mesh->data)) {
            throw std::invalid_argument("来源网格已改变，请重新采样。");
        }
        expected_mesh_id = mesh->id;
        expected_mesh = mesh->data;
        target_id = candidate_id;
    }
    begin_asset_task("正在重建点云网格…");
    try {
        geometry_tasks_->reconstruct_points(
            std::move(input), [this, source, generation, revision, fit, expected_mesh_id,
                               expected_mesh, target_id, candidate_id](const auto& result) {
                if (!finish_geometry_task(result)) {
                    return true;
                }
                if (generation != document_generation_ || revision != points_->source_revision() ||
                    source.cloud != points_->source().cloud) {
                    return false;
                }
                const auto& current = editor_.document();
                if (!target_id.empty()) {
                    const auto* mesh = mesh_for(current, object_for(current, target_id));
                    if (!mesh || mesh->id != expected_mesh_id || mesh->data != expected_mesh) {
                        return false;
                    }
                } else if (!candidate_id.empty() && object_for(current, candidate_id)) {
                    return false;
                }
                std::string created;
                editor_.edit_prevalidated(
                    [&](auto& scene) {
                        const auto mesh_id = unique_id(scene.meshes, "point_mesh_");
                        scene.meshes.push_back({mesh_id, {}, result.mesh});
                        if (!target_id.empty()) {
                            for (auto& object : scene.objects) {
                                if (object.id == target_id) {
                                    object.mesh_id = mesh_id;
                                }
                            }
                        } else {
                            const auto material = unique_id(scene.materials, "point_material_");
                            minirender::Material appearance;
                            appearance.id = material;
                            appearance.name = "点云材质";
                            appearance.base_color = {.32F, .58F, .72F};
                            scene.materials.push_back(appearance);
                            minirender::Transform transform;
                            if (fit) {
                                const auto low = result.raster->minimum;
                                const auto extent = result.raster->maximum - low;
                                const float scale = 2 / std::max({extent.x, extent.y, extent.z});
                                const auto center = low + extent * .5F;
                                transform.scale = glm::vec3(scale);
                                transform.translation = {-center.x * scale, .03F - low.y * scale,
                                                         -center.z * scale};
                            }
                            created = unique_id(scene.objects, "point_object_");
                            scene.objects.push_back({created, source.name + " 重建", mesh_id,
                                                     material, transform, true});
                        }
                    },
                    result.validated_meshes);
                ++document_revision_;
                points_->remember_generated(result.mesh, created.empty() ? target_id : created);
                if (!created.empty()) {
                    selected_id_ = created;
                }
                viewport_->supply_raster_mesh(result.mesh, result.raster);
                viewport_->set_point_preview_visible(false);
                viewport_->set_offline_visible(false);
                refresh();
                points_->show_reconstruction(result);
                viewport_->set_point_preview_data(result.point_preview);
                return true;
            });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}

void Workbench::export_point_file(const QString& path) {
    const auto cloud = points_->display_cloud();
    if (!cloud) {
        throw std::invalid_argument("请先导入或采样点云。");
    }
    const auto destination = std::filesystem::path(path.toStdU16String());
    begin_asset_task("正在导出点云…");
    try {
        geometry_tasks_->export_points(editor_.document(), cloud, points_->source().file,
                                       destination, [this](const auto& result) {
                                           finish_geometry_task(result);
                                           return true;
                                       });
    } catch (...) {
        finish_asset_task();
        throw;
    }
}
