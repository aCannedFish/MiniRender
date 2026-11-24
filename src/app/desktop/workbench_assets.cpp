// 模型和场景导入、内置模板、场景切换及同步保存。

#include "builtin_scenes.hpp"
#include "cloth_controller.hpp"
#include "environment_panel.hpp"
#include "lightfield_panel.hpp"
#include "offline_panel.hpp"
#include "point_cloud_panel.hpp"
#include "resource_catalog.hpp"
#include "workbench_internal.hpp"
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QStatusBar>
#include <algorithm>
#include <minirender/assets/scene_io.hpp>

using namespace workbench_detail;

LoadedScene workbench_detail::prepare_scene(const std::filesystem::path& source) {
    LoadedScene result;
    result.document = minirender::load_scene(source);
    result.document.validate(&result.validated);
    for (const auto& mesh : result.document.meshes) {
        const bool visible =
            std::any_of(result.document.objects.begin(), result.document.objects.end(),
                        [&](const auto& item) { return item.visible && item.mesh_id == mesh.id; });
        if (visible) {
            result.raster.emplace_back(mesh.data, minirender::prepare_raster_mesh(*mesh.data));
        }
    }
    return result;
}

void Workbench::import_object_file(const QString& path) {
    begin_asset_task("正在读取并校验网格…");
    const auto source     = native_path(path);
    const auto generation = document_generation_;
    run_background(
        this,
        [source] {
            LoadedMesh result;
            result.data =
                std::make_shared<const minirender::MeshData>(minirender::load_obj(source));
            result.validated.validate(result.data);
            result.raster = minirender::prepare_raster_mesh(*result.data);
            return result;
        },
        [this, source, path, generation](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            viewport_->supply_raster_mesh(result.value.data, result.value.raster);
            perform_edit(
                [&](minirender::SceneDocument& scene) {
                    if (scene.materials.empty()) {
                        scene.materials.push_back({"default", "默认材质"});
                    }
                    const auto mesh_id   = unique_id(scene.meshes, "mesh_");
                    const auto object_id = unique_id(scene.objects, "object_");
                    scene.meshes.push_back({mesh_id, source, result.value.data});
                    scene.objects.push_back({object_id,
                                             QFileInfo(path).baseName().toStdString(),
                                             mesh_id,
                                             scene.materials.front().id,
                                             {},
                                             true});
                    selected_id_ = object_id;
                },
                &result.value.validated);
        });
}

void Workbench::open_scene_file(const QString& path, std::function<void()> after_load) {
    begin_asset_task("正在读取场景与资产…");
    const auto source     = native_path(path);
    const auto generation = ++document_generation_;
    run_background(
        this, [source] { return prepare_scene(source); },
        [this, path, generation, after_load = std::move(after_load)](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            try {
                for (const auto& [mesh, raster] : result.value.raster) {
                    viewport_->supply_raster_mesh(mesh, raster);
                }
                install_scene(result.value.document, path, &result.value.validated);
                if (after_load) {
                    after_load();
                }
            } catch (const std::exception& error) {
                report_error(error);
            }
        });
}

void Workbench::import_builtin_scene(const QString& id) {
    const auto entries = catalog::read(builtin_scene_directory(), true);
    for (const auto& entry : entries) {
        if (entry.builtin && entry.id == id) {
            import_resource(entry);
            return;
        }
    }
    throw std::invalid_argument("Unknown built-in scene");
}

void Workbench::apply_builtin_settings(const BuiltinScene& preset) {
    scene_path_.clear();
    findChild<QDoubleSpinBox*>("exposure")->setValue(0);
    lightfield_->cancel();
    offline_->prepare_import(preset.render, preset.integrator);
    select_object(preset.selected_object);
    findChild<QComboBox*>("view_mode")->setCurrentIndex(VulkanViewport::base_color_preview_mode);
    if (!editor_.document().media.empty()) {
        panels_->setCurrentIndex(10);
    }
    for (const auto& material : editor_.document().materials) {
        if (material.kind == minirender::MaterialKind::subsurface) {
            panels_->setCurrentIndex(0);
            break;
        }
    }
    if (preset.id == "lightfield") {
        minirender::LightFieldCaptureSettings settings;
        settings.render          = preset.render;
        settings.reference_focus = 4;
        settings.aperture_radius = .2;
        lightfield_panel_->set_capture_settings(settings);
        panels_->setCurrentIndex(11);
    }
    setWindowTitle("MiniRender — " + preset.name + "（示例副本）");
    statusBar()->showMessage(preset.name + " 已覆盖导入，当前为原色预览；可编辑后开始离线渲染。",
                             12000);
}

void Workbench::load_scene_file(const QString& path) {
    install_scene(minirender::load_scene(native_path(path)), path);
}

void Workbench::install_scene(minirender::SceneDocument document, const QString& path,
                              const minirender::MeshValidationCache* validated) {
    if (document.directional_lights.size() > 8) {
        throw std::runtime_error("实时视口目前最多支持 8 个方向光。");
    }
    ++document_generation_;
    ++document_revision_;
    if (cloth_controller_) {
        cloth_controller_->pause();
    }
    cloth_target_.clear();
    geometry_tasks_->cancel();
    viewport_->set_point_preview_data({});
    points_->clear_source();
    if (validated) {
        editor_.replace_prevalidated(std::move(document), *validated);
    } else {
        editor_.replace(std::move(document));
    }
    scene_path_ = path;
#ifdef Q_OS_MACOS
    // 包内示例作为模板打开，首次保存选择用户路径，保持已签名资源不变。
    const auto resources =
        QDir(QCoreApplication::applicationDirPath() + "/../Resources").canonicalPath();
    if (!resources.isEmpty() && QFileInfo(path).canonicalFilePath().startsWith(resources + '/')) {
        scene_path_.clear();
    }
#endif
    selected_id_.clear();
    diagnostic_ = false;
    refresh();
    setWindowTitle("MiniRender — " + QFileInfo(path).fileName() +
                   (scene_path_.isEmpty() ? "（示例副本）" : ""));
}

void Workbench::save_scene_file(const QString& path) {
    minirender::save_scene(editor_.document(), native_path(path));
    scene_path_ = path;
    setWindowTitle("MiniRender — " + QFileInfo(path).fileName());
}

void Workbench::import_texture() {
    const auto path = QFileDialog::getOpenFileName(this, "导入底色纹理", {},
                                                   "Images (*.png *.jpg *.jpeg *.hdr *.exr)");
    if (path.isEmpty()) {
        return;
    }
    const auto suffix   = QFileInfo(path).suffix().toLower();
    const auto encoding = suffix == "hdr" || suffix == "exr" ? minirender::ColorEncoding::linear
                                                             : minirender::ColorEncoding::srgb;
    import_texture_file(path, encoding);
}

void Workbench::import_texture_file(const QString& path, minirender::ColorEncoding encoding) {
    begin_asset_task("正在读取纹理并生成 mipmap…");
    std::string material_id;
    for (const auto& object : editor_.document().objects) {
        if (object.id == selected_id_) {
            material_id = object.material_id;
        }
    }
    const auto generation = document_generation_;
    const auto source     = native_path(path);
    texture_loading_      = true;
    run_background(
        this,
        [source, encoding] {
            return std::make_shared<const minirender::Texture>(
                minirender::Image::read(source, encoding));
        },
        [this, generation, source, encoding, material_id](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            bool subsurface_target = false;
            perform_edit([&](minirender::SceneDocument& scene) {
                const auto id = unique_id(scene.textures, "texture_");
                scene.textures.push_back({id, source, encoding, result.value});
                for (auto& material : scene.materials) {
                    if (material.id == material_id) {
                        if (material.kind == minirender::MaterialKind::subsurface) {
                            subsurface_target = true;
                        } else {
                            material.base_color_texture = id;
                        }
                    }
                }
            });
            if (subsurface_target) {
                material_notice_->setText("纹理已导入资源；均匀 SSS 材质不使用底色纹理。");
                material_notice_->show();
            }
        });
}

bool Workbench::texture_loading() const noexcept {
    return texture_loading_;
}

IblController* Workbench::ibl_controller() const noexcept {
    return ibl_;
}

void Workbench::import_environment_file(const QString& path) {
    begin_asset_task("正在读取环境图…");
    const auto source     = native_path(path);
    const auto suffix     = QFileInfo(path).suffix().toLower();
    const auto encoding   = suffix == "hdr" || suffix == "exr" ? minirender::ColorEncoding::linear
                                                               : minirender::ColorEncoding::srgb;
    const auto generation = document_generation_;
    run_background(
        this,
        [source, encoding] {
            return std::make_shared<const minirender::Texture>(
                minirender::Image::read(source, encoding));
        },
        [this, source, encoding, generation](const auto& result) {
            finish_asset_task();
            if (generation != document_generation_) {
                return;
            }
            if (!result.error.empty()) {
                report_error(std::runtime_error(result.error));
                return;
            }
            perform_edit([&](auto& scene) {
                const auto id = unique_id(scene.textures, "environment_");
                scene.textures.push_back({id, source, encoding, result.value});
                scene.environment_map.texture_id = id;
            });
        });
}
