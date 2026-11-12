#include "builtin_scenes.hpp"
#include "cloth_controller.hpp"
#include "cloth_panel.hpp"
#include "geometry_controller.hpp"
#include "offline_panel.hpp"
#include "point_cloud_panel.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QComboBox>
#include <QStatusBar>
#include <QTreeWidget>

#include <algorithm>

namespace {

// 演示场景复用单个地面，碰撞形状与实际可编辑物体共用变换。
minirender::SceneDocument demo_scene(int kind) {
    using namespace minirender;
    const auto source = std::filesystem::path(builtin_scene_directory().toStdU16String());
    auto scene = load_scene(source / "studio.json");
    std::erase_if(scene.objects, [](const auto& object) { return object.id != "ground"; });
    std::erase_if(scene.meshes, [](const auto& mesh) { return mesh.id != "plane"; });
    std::erase_if(scene.materials, [](const auto& material) { return material.id != "floor"; });
    scene.objects[0].cloth_collider = ClothCollider{ClothColliderKind::plane};
    ClothDefinition definition;
    Transform transform;
    if (kind == 0) {
        definition.grid.origin = {-1, 0, 1};
        transform.translation = {0, 1.7F, 0};
        auto sphere = std::make_shared<const MeshData>(load_obj(source / "assets/demo-sphere.obj"));
        scene.meshes.push_back({"support", "assets/demo-sphere.obj", sphere});
        Material support;
        support.id = "support";
        support.base_color = {.6F, .65F, .7F};
        scene.materials.push_back(support);
        SceneObject object{"support", "碰撞球", "support", "support", {}};
        object.transform.translation = {0, .65F, 0};
        object.transform.scale = glm::vec3(.65F);
        object.cloth_collider = ClothCollider{};
        scene.objects.push_back(object);
    } else {
        definition.grid.origin = {-1, .85, 0};
        definition.grid.span_v = {0, -1.7, 0};
        definition.grid.pinned = {0, 24};
        definition.environment.wind_velocity = {0, 0, 1.5};
        transform.translation = {0, 1.3F, 0};
    }
    const auto component = make_cloth_component(definition);
    const auto preset = make_cloth_preset("silk");
    Material material;
    material.id = "fabric";
    material.kind = MaterialKind::cloth;
    material.base_color = glm::vec3(1);
    material.ior = static_cast<float>(preset.ior);
    material.cloth = preset.appearance;
    scene.materials.push_back(material);
    scene.meshes.push_back({"fabric", {}, component->mesh()});
    SceneObject object{"cloth", "布料", "fabric", "fabric", transform};
    object.cloth = component;
    scene.objects.push_back(object);
    scene.camera.position = {3, 2.4F, 3.8F};
    scene.camera.target = {0, kind == 0 ? .8F : 1.3F, 0};
    scene.validate();
    return scene;
}
} // namespace

ClothController* Workbench::cloth_controller() const noexcept {
    return cloth_controller_;
}

void Workbench::create_cloth_demo(int kind) {
    if (asset_loading_) {
        throw std::runtime_error("等待当前文件任务结束后再载入布料示例。");
    }
    auto scene = demo_scene(kind);
    cloth_controller_->pause();
    geometry_tasks_->cancel();
    ++document_generation_;
    ++document_revision_;
    viewport_->set_point_preview_data({});
    points_->clear_source();
    editor_.edit([&](auto& document) { document = std::move(scene); });
    scene_path_.clear();
    cloth_target_ = "cloth";
    selected_id_ = "cloth";
    refresh();
    show_cloth_preview();
    setWindowTitle("MiniRender — 布料示例");
    statusBar()->showMessage("布料示例已载入，可播放、编辑物体或冻结当前帧。", 8000);
}

void Workbench::show_cloth_preview() {
    viewport_->set_offline_visible(false);
    findChild<QComboBox*>("view_mode")->setCurrentIndex(0);
    viewport_->set_view_mode(0);
}

void Workbench::sync_cloth_context() {
    if (!cloth_controller_) {
        return;
    }
    const auto& scene = editor_.document();
    auto target = std::find_if(scene.objects.begin(), scene.objects.end(), [&](const auto& object) {
        return object.id == cloth_target_ && object.cloth;
    });
    if (target == scene.objects.end()) {
        target = std::find_if(scene.objects.begin(), scene.objects.end(),
                              [](const auto& object) { return bool(object.cloth); });
    }
    if (target == scene.objects.end()) {
        cloth_controller_->bind(std::nullopt);
    } else {
        cloth_target_ = target->id;
        cloth_controller_->bind(ClothRuntimeContext{
            target->id, document_generation_, target->cloth, target->transform,
            minirender::cloth_environment(*target->cloth->definition(), scene)});
    }
    cloth_panel_->set_document(scene, cloth_target_);
}

bool Workbench::accept_cloth_frame(const ClothRuntimeFrame& frame) {
    if (frame.source.document_generation != document_generation_) {
        return false;
    }
    if (!editor_.update_cloth_frame(frame.source.object_id, frame.source.component,
                                    frame.source.transform, frame.frame.component, frame.validated,
                                    frame.history)) {
        return false;
    }
    ++document_revision_;
    const auto mesh = frame.frame.component->mesh();
    viewport_->supply_raster_mesh(mesh, frame.raster);
    viewport_->set_scene(editor_.snapshot());
    mark_snapshot_changed();
    if (frame.history) {
        undo_->setEnabled(editor_.can_undo());
        redo_->setEnabled(editor_.can_redo());
        refresh_properties();
        cloth_panel_->set_document(editor_.document(), cloth_target_);
        for (int i = 0; i < assets_->topLevelItemCount(); ++i) {
            auto* item = assets_->topLevelItem(i);
            for (const auto& asset : editor_.document().meshes) {
                if (item->text(0).toStdString() == asset.id) {
                    item->setText(1, QString::number(asset.data->triangles.size()));
                }
            }
        }
    }
    return true;
}

void Workbench::apply_cloth_definition(
    std::shared_ptr<const minirender::ClothDefinition> definition, bool reset) {
    const auto environment = minirender::cloth_environment(*definition, editor_.document());
    cloth_controller_->configure(std::move(definition), environment, reset);
}

void Workbench::freeze_cloth() {
    cloth_controller_->pause();
    const auto id = cloth_target_;
    perform_edit([&](auto& scene) {
        for (auto& object : scene.objects) {
            if (object.id == id) {
                object.cloth.reset();
            }
        }
    });
}

std::shared_ptr<const minirender::SceneSnapshot> Workbench::offline_snapshot() {
    if (cloth_controller_) {
        cloth_controller_->pause();
    }
    return editor_.snapshot();
}
