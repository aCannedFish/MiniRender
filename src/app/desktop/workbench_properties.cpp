// 物体、灯光和相机属性面板的构建、绑定及状态刷新。

#include "cloth_collider_panel.hpp"
#include "cloth_material_panel.hpp"
#include "comparison_controls.hpp"
#include "editor_theme.hpp"
#include "geometry_panel.hpp"
#include "offline_panel.hpp"
#include "subsurface_material_panel.hpp"
#include "workbench_internal.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <algorithm>
#include <minirender/render/rectangle_light.hpp>

using namespace workbench_detail;

void Workbench::build_object_panel() {
    object_panel_ = new QWidget;
    auto* layout  = new QFormLayout(object_panel_);
    configure_form(layout);
    object_name_ = new QLineEdit;
    object_name_->setObjectName("object_name");
    translation_   = new VectorField(object_panel_, "translation");
    rotation_      = new VectorField(object_panel_, "rotation", -360, 360);
    scale_         = new VectorField(object_panel_, "scale", -100, 100);
    base_color_    = new VectorField(object_panel_, "base_color", 0, 1);
    emission_      = new VectorField(object_panel_, "emission", 0, 1000000);
    material_kind_ = new QComboBox;
    material_kind_->setObjectName("material_kind");
    material_kind_->addItems({"漫反射", "金属", "镜面（预览近似）", "玻璃（预览近似）",
                              "布料（微圆柱）", "次表面散射（SSS）"});
    texture_selector_ = new QComboBox;
    texture_selector_->setObjectName("base_texture");
    texture_filter_ = new QComboBox;
    texture_filter_->setObjectName("texture_filter");
    texture_filter_->addItems({"最近邻", "双线性", "三线性 mipmap"});
    double_sided_ = new QCheckBox("双面渲染");
    double_sided_->setObjectName("material_double_sided");
    double_sided_->setToolTip("关闭时只显示几何正面；镜像缩放保留原外侧。");
    roughness_ = spin(object_panel_, "roughness", 0, 1, 0.05);
    ior_       = spin(object_panel_, "ior", 0.1, 5, 0.05);
    layout->addRow("名称", object_name_);
    layout->addRow(editor::section("变换", object_panel_));
    layout->addRow("位置 / m", translation_);
    layout->addRow("旋转 / °", rotation_);
    layout->addRow("缩放", scale_);
    cloth_collider_ = new ClothColliderPanel(object_panel_);
    layout->addRow(cloth_collider_);
    cloth_collider_->on_changed = [this](const minirender::ClothCollider& collider) {
        edit_object_property(
            [&](minirender::SceneObject& object) { object.cloth_collider = collider; });
    };
    layout->addRow(editor::section("表面材质", object_panel_));
    layout->addRow("材质", material_kind_);
    material_notice_ = new QLabel(object_panel_);
    material_notice_->setObjectName("material_notice");
    material_notice_->setWordWrap(true);
    material_notice_->setProperty("role", "muted");
    layout->addRow(material_notice_);
    layout->addRow(double_sided_);
    layout->addRow("底色", base_color_);
    layout->addRow("底色纹理", texture_selector_);
    layout->addRow("纹理过滤", texture_filter_);
    layout->addRow("自发光", emission_);
    layout->addRow("粗糙度", roughness_);
    energy_compensation_ = new QCheckBox("补偿多次微面散射");
    energy_compensation_->setObjectName("energy_compensation");
    layout->addRow(energy_compensation_);
    connect(energy_compensation_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!refreshing_) {
            edit_material_property(
                [enabled](auto& material) { material.compensate_energy = enabled; });
        }
    });
    layout->addRow("折射率", ior_);
    auto* hint = new QLabel("同一材质的参数会同时影响引用它的物体。");
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    layout->addRow(hint);
    cloth_material_ = new ClothMaterialPanel(object_panel_);
    layout->addRow(cloth_material_);
    cloth_material_->on_changed =
        [this](std::shared_ptr<const minirender::ClothAppearance> appearance,
               std::optional<double>                              ior) {
            edit_material_property([&](minirender::Material& material) {
                material.cloth = appearance;
                if (ior) {
                    material.ior        = static_cast<float>(*ior);
                    material.base_color = glm::vec3(1);
                }
            });
        };
    subsurface_material_ = new SubsurfaceMaterialPanel(object_panel_);
    layout->addRow(subsurface_material_);
    subsurface_material_->on_changed = [this](auto parameters, std::optional<double> ior) {
        edit_material_property([&](minirender::Material& material) {
            material.subsurface = parameters;
            if (ior) {
                material.ior = static_cast<float>(*ior);
            }
        });
    };
    subsurface_material_->render_active = [this] {
        return offline_ && (!minirender::render_terminal(offline_->session().progress().state) ||
                            offline_->exporting());
    };
    subsurface_material_->on_render = [this](bool start) {
        if (start && minirender::render_terminal(offline_->session().progress().state) &&
            !offline_->exporting()) {
            offline_->set_integrator_kind(minirender::IntegratorKind::path_tracing);
            offline_->start();
        }
        panels_->setCurrentIndex(3);
    };
    translation_->on_changed = [this] {
        edit_object_property([&](minirender::SceneObject& object) {
            object.transform.translation = translation_->value();
        });
    };
    rotation_->on_changed = [this] {
        edit_object_property([&](minirender::SceneObject& object) {
            object.transform.rotation_degrees = rotation_->value();
        });
    };
    scale_->on_changed = [this] {
        edit_object_property(
            [&](minirender::SceneObject& object) { object.transform.scale = scale_->value(); });
    };
    base_color_->on_changed = [this] {
        edit_material_property(
            [&](minirender::Material& material) { material.base_color = base_color_->value(); });
    };
    emission_->on_changed = [this] {
        edit_material_property(
            [&](minirender::Material& material) { material.emission = emission_->value(); });
    };
    connect(object_name_, &QLineEdit::editingFinished, this, [this] {
        if (!object_name_->isModified()) {
            return;
        }
        object_name_->setModified(false);
        edit_object_property([&](minirender::SceneObject& object) {
            object.name = object_name_->text().toStdString();
        });
    });
    connect(roughness_, &QDoubleSpinBox::valueChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.roughness = static_cast<float>(roughness_->value());
        });
    });
    connect(ior_, &QDoubleSpinBox::valueChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.ior = static_cast<float>(ior_->value());
        });
    });
    connect(material_kind_, &QComboBox::currentIndexChanged, this, [this] {
        if (refreshing_) {
            return;
        }
        const auto kind = static_cast<minirender::MaterialKind>(material_kind_->currentIndex());
        if (kind == minirender::MaterialKind::subsurface) {
            for (const auto& object : editor_.document().objects) {
                if (object.id != selected_id_) {
                    continue;
                }
                for (const auto& material : editor_.document().materials) {
                    if (material.id == object.material_id && !material.base_color_texture.empty()) {
                        refresh_properties();
                        material_notice_->setText("请先移除底色纹理，再使用均匀 SSS 材质。");
                        material_notice_->show();
                        return;
                    }
                }
            }
        }
        edit_material_property([&](minirender::Material& material) {
            material.kind = kind;
            if (material.kind == minirender::MaterialKind::cloth) {
                material.ior = std::clamp(material.ior, 1.0F, 3.0F);
                if (!material.cloth) {
                    const auto preset   = minirender::make_cloth_preset("linen");
                    material.cloth      = preset.appearance;
                    material.ior        = static_cast<float>(preset.ior);
                    material.base_color = glm::vec3(1);
                }
            }
            if (kind == minirender::MaterialKind::subsurface) {
                material.ior = std::clamp(material.ior, 1.0F, 3.0F);
                if (!material.subsurface) {
                    const auto preset   = minirender::make_subsurface_preset("marble");
                    material.subsurface = preset.parameters;
                    material.ior        = static_cast<float>(preset.ior);
                }
            }
        });
    });
    connect(texture_selector_, &QComboBox::currentIndexChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.base_color_texture = texture_selector_->currentData().toString().toStdString();
        });
    });
    connect(texture_filter_, &QComboBox::currentIndexChanged, this, [this] {
        edit_material_property([&](minirender::Material& material) {
            material.texture_filter =
                static_cast<minirender::TextureFilter>(texture_filter_->currentIndex());
        });
    });
    connect(double_sided_, &QCheckBox::toggled, this, [this](bool checked) {
        edit_material_property(
            [&](minirender::Material& material) { material.double_sided = checked; });
    });
    object_panel_->setEnabled(false);
}

void Workbench::build_light_panel() {
    light_panel_ = new QWidget;
    auto* layout = new QFormLayout(light_panel_);
    configure_form(layout);
    light_selector_  = new QComboBox;
    light_direction_ = new VectorField(light_panel_, "light_direction", -100, 100);
    light_color_     = new VectorField(light_panel_, "light_color", 0, 100);
    light_intensity_ = spin(light_panel_, "light_intensity", 0, 1000, 0.1);
    layout->addRow("方向光", light_selector_);
    layout->addRow("传播方向", light_direction_);
    layout->addRow("颜色", light_color_);
    layout->addRow("强度", light_intensity_);
    auto* rectangle = new QPushButton("添加矩形面积光");
    rectangle->setObjectName("add_rectangle");
    layout->addRow(rectangle);
    connect(rectangle, &QPushButton::clicked, this, [this] {
        perform_edit([this](auto& scene) {
            const auto           mesh_id     = unique_id(scene.meshes, "rectangle_");
            const auto           material_id = unique_id(scene.materials, "emitter_");
            minirender::Material material{material_id, "面积光", minirender::MaterialKind::diffuse,
                                          glm::vec3(0)};
            material.emission     = glm::vec3(8);
            material.double_sided = false;
            scene.materials.push_back(std::move(material));
            scene.meshes.push_back({mesh_id, {}, minirender::make_emitter_quad()});
            const auto distance =
                std::max(.01F, glm::length(scene.camera.position - scene.camera.target));
            minirender::Transform transform;
            transform.translation        = scene.camera.target + glm::vec3(0, .35F * distance, 0);
            transform.rotation_degrees.x = 180;
            transform.scale              = {.2F * distance, 1, .2F * distance};
            selected_id_                 = unique_id(scene.objects, "light_");
            scene.objects.push_back(
                {selected_id_, "矩形面积光", mesh_id, material_id, transform, true});
        });
        panels_->setCurrentIndex(0);
    });
    connect(light_selector_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_) {
            refresh_properties();
        }
    });
    const auto update = [this] {
        if (refreshing_ || light_selector_->currentIndex() < 0) {
            return;
        }
        perform_edit([&](minirender::SceneDocument& scene) {
            auto& light = scene.directional_lights.at(
                static_cast<std::size_t>(light_selector_->currentIndex()));
            light.direction = light_direction_->value();
            light.color     = light_color_->value();
            light.intensity = static_cast<float>(light_intensity_->value());
        });
    };
    light_direction_->on_changed = light_color_->on_changed = update;
    connect(light_intensity_, &QDoubleSpinBox::valueChanged, this, update);
}

void Workbench::build_camera_panel() {
    camera_panel_ = new QWidget;
    auto* layout  = new QFormLayout(camera_panel_);
    configure_form(layout);
    camera_position_ = new VectorField(camera_panel_, "camera_position");
    camera_target_   = new VectorField(camera_panel_, "camera_target");
    fov_             = spin(camera_panel_, "fov", 5, 150, 1);
    exposure_        = spin(camera_panel_, "exposure", -10, 10, 0.25);
    aperture_        = spin(camera_panel_, "aperture_radius", 0, 10, 0.005);
    focus_           = spin(camera_panel_, "focus_distance", 0.0001, 100000, 0.1);
    auto* mode       = new QComboBox;
    mode->setObjectName("view_mode");
    mode->addItems({"光照合成", "法线", "线性深度", "线框", "原始 AO", "滤波 AO", "原色预览",
                    "Blinn–Phong", "阴影深度", "遮挡物深度", "半影半径", "光源可见性"});
    mode->setItemData(VulkanViewport::base_color_preview_mode,
                      "均匀亮度显示材质和纹理，保留深度与接触遮蔽；不依赖场景光源。",
                      Qt::ToolTipRole);
    layout->addRow(editor::section("相机", camera_panel_));
    layout->addRow("位置", camera_position_);
    layout->addRow("目标", camera_target_);
    layout->addRow("垂直 FOV / °", fov_);
    layout->addRow("光圈半径 / m", aperture_);
    layout->addRow("对焦距离 / m", focus_);
    layout->addRow(editor::section("视口显示", camera_panel_));
    layout->addRow("曝光 / EV", exposure_);
    layout->addRow("显示（A）", mode);
    add_comparison_controls(*layout, *mode, *viewport_);
    auto* fxaa = new QCheckBox("启用 FXAA");
    fxaa->setObjectName("fxaa_enabled");
    fxaa->setChecked(true);
    layout->addRow(fxaa);
    connect(fxaa, &QCheckBox::toggled, this,
            [this](bool enabled) { viewport_->set_fxaa_enabled(enabled); });
    auto* shadows = new QCheckBox("启用方向光阴影");
    shadows->setObjectName("shadows_enabled");
    shadows->setChecked(true);
    layout->addRow(shadows);
    connect(shadows, &QCheckBox::toggled, this,
            [this](bool enabled) { viewport_->set_shadows_enabled(enabled); });
    auto* area_shadow = new QCheckBox("矩形面积光软阴影");
    area_shadow->setObjectName("area_shadows");
    area_shadow->setChecked(viewport_->area_shadows());
    layout->addRow(area_shadow);
    connect(area_shadow, &QCheckBox::toggled, this,
            [this](bool enabled) { viewport_->set_area_shadows(enabled); });
    auto* filter = new QComboBox;
    filter->setObjectName("shadow_filter");
    filter->addItems({"硬阴影", "PCF", "PCSS"});
    filter->setCurrentIndex(static_cast<int>(viewport_->shadow_filter()));
    auto* angle = spin(camera_panel_, "shadow_half_angle", 0, 15, .25);
    angle->setValue(viewport_->light_half_angle());
    auto* radius = spin(camera_panel_, "shadow_filter_radius", 0, 48, .5);
    radius->setValue(viewport_->shadow_filter_radius());
    auto* layer = new QSpinBox;
    layer->setObjectName("shadow_debug_light");
    layer->setRange(1, 8);
    layer->setToolTip("与场景方向光的顺序一致，只有阴影诊断视图使用此选择");
    layout->addRow("阴影过滤", filter);
    layout->addRow("光源半角 / °", angle);
    layout->addRow("PCF 半径 / 纹素", radius);
    layout->addRow("诊断光源", layer);
    const auto controls = [=] {
        filter->setEnabled(shadows->isChecked());
        angle->setEnabled(shadows->isChecked() && filter->currentIndex() == 2);
        radius->setEnabled(shadows->isChecked() && filter->currentIndex() == 1);
    };
    connect(shadows, &QCheckBox::toggled, camera_panel_, controls);
    connect(filter, &QComboBox::currentIndexChanged, camera_panel_, [this, controls](int i) {
        viewport_->set_shadow_filter(static_cast<ShadowFilter>(i));
        controls();
    });
    connect(angle, &QDoubleSpinBox::valueChanged, camera_panel_,
            [this](double v) { viewport_->set_light_half_angle(static_cast<float>(v)); });
    connect(radius, &QDoubleSpinBox::valueChanged, camera_panel_,
            [this](double v) { viewport_->set_shadow_filter_radius(static_cast<float>(v)); });
    connect(layer, &QSpinBox::valueChanged, camera_panel_,
            [this](int i) { viewport_->set_shadow_debug_light(i - 1); });
    controls();
    const auto update = [this] {
        if (refreshing_) {
            return;
        }
        perform_edit([&](minirender::SceneDocument& scene) {
            scene.camera.position             = camera_position_->value();
            scene.camera.target               = camera_target_->value();
            scene.camera.vertical_fov_degrees = static_cast<float>(fov_->value());
            scene.camera.aperture_radius      = static_cast<float>(aperture_->value());
            scene.camera.focus_distance       = static_cast<float>(focus_->value());
        });
    };
    camera_position_->on_changed = camera_target_->on_changed = update;
    connect(fov_, &QDoubleSpinBox::valueChanged, this, update);
    connect(aperture_, &QDoubleSpinBox::valueChanged, this, update);
    connect(focus_, &QDoubleSpinBox::valueChanged, this, update);
    connect(exposure_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) { viewport_->set_exposure(static_cast<float>(value)); });
    connect(mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        viewport_->set_view_mode(index);
        exposure_->setEnabled(index != VulkanViewport::base_color_preview_mode);
    });
    mode->setCurrentIndex(VulkanViewport::base_color_preview_mode);
}

void Workbench::refresh_properties() {
    refreshing_          = true;
    const auto& document = editor_.document();
    geometry_->set_context(document, selected_id_);
    const auto object = std::find_if(
        document.objects.begin(), document.objects.end(),
        [&](const minirender::SceneObject& value) { return value.id == selected_id_; });
    texture_selector_->clear();
    texture_selector_->addItem("无", QString());
    for (const auto& texture : document.textures) {
        texture_selector_->addItem(QString::fromStdString(texture.id),
                                   QString::fromStdString(texture.id));
    }
    object_panel_->setEnabled(object != document.objects.end());
    cloth_collider_->set_collider(object != document.objects.end() ? object->cloth_collider
                                                                   : std::nullopt);
    if (object == document.objects.end()) {
        cloth_material_->set_material(nullptr, document_generation_);
        subsurface_material_->set_material(nullptr, document_generation_);
    }
    material_notice_->clear();
    material_notice_->hide();
    viewport_->set_gizmo_target(
        object != document.objects.end() && object->visible ? object->id : "",
        object != document.objects.end() && object->visible ? std::optional(object->transform)
                                                            : std::nullopt);
    if (object != document.objects.end()) {
        object_name_->setText(QString::fromStdString(object->name));
        translation_->set_value(object->transform.translation);
        rotation_->set_value(object->transform.rotation_degrees);
        scale_->set_value(object->transform.scale);
        for (const auto& material : document.materials) {
            if (material.id == object->material_id) {
                base_color_->set_value(material.base_color);
                emission_->set_value(material.emission);
                material_kind_->setCurrentIndex(static_cast<int>(material.kind));
                const bool cloth      = material.kind == minirender::MaterialKind::cloth;
                const bool subsurface = material.kind == minirender::MaterialKind::subsurface;
                energy_compensation_->setVisible(material.kind ==
                                                 minirender::MaterialKind::conductor);
                energy_compensation_->setChecked(material.compensate_energy);
                roughness_->setEnabled(!cloth && !subsurface);
                base_color_->setEnabled(!subsurface);
                texture_selector_->setEnabled(!subsurface);
                texture_filter_->setEnabled(!subsurface);
                ior_->setRange(cloth || subsurface ? 1 : .1, cloth || subsurface ? 3 : 5);
                cloth_material_->set_material(&material, document_generation_);
                subsurface_material_->set_material(&material, document_generation_);
                roughness_->setValue(material.roughness);
                ior_->setValue(material.ior);
                double_sided_->setChecked(material.double_sided);
                texture_selector_->setCurrentIndex(texture_selector_->findData(
                    QString::fromStdString(material.base_color_texture)));
                texture_filter_->setCurrentIndex(static_cast<int>(material.texture_filter));
            }
        }
    }
    const int index = light_selector_->currentIndex();
    light_direction_->setEnabled(index >= 0);
    light_color_->setEnabled(index >= 0);
    light_intensity_->setEnabled(index >= 0);
    if (index >= 0) {
        const auto& light = document.directional_lights.at(static_cast<std::size_t>(index));
        light_direction_->set_value(light.direction);
        light_color_->set_value(light.color);
        light_intensity_->setValue(light.intensity);
    }
    refreshing_ = false;
    update_point_selection();
}

void Workbench::refresh_camera() {
    camera_position_->set_value(editor_.document().camera.position);
    camera_target_->set_value(editor_.document().camera.target);
    const QSignalBlocker blocker(fov_);
    fov_->setValue(editor_.document().camera.vertical_fov_degrees);
    const QSignalBlocker aperture_blocker(aperture_);
    const QSignalBlocker focus_blocker(focus_);
    const QSignalBlocker exposure_blocker(exposure_);
    aperture_->setValue(editor_.document().camera.aperture_radius);
    focus_->setValue(editor_.document().camera.focus_distance);
    exposure_->setValue(viewport_->exposure());
}
