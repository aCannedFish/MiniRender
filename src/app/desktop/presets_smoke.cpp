#include "presets_smoke.hpp"
#include "builtin_scenes.hpp"
#include "offline_panel.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <minirender/assets/scene_io.hpp>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct State {
    std::size_t preset = 0;
    int stage = 0;
    int ticks = 0;
    QByteArray original;
    minirender::SceneDocument original_scene;
    std::shared_ptr<const minirender::SceneSnapshot> snapshot;
};

const minirender::SceneObject& object(const minirender::SceneDocument& scene,
                                      const std::string& id) {
    const auto found = std::find_if(scene.objects.begin(), scene.objects.end(),
                                    [&](const auto& value) { return value.id == id; });
    require(found != scene.objects.end(), "Preset object is missing");
    return *found;
}

// 按源模板的完整对象集合和变换验证覆盖导入，适用于物体数量不同的场景。
void verify_import(const minirender::SceneDocument& actual,
                   const minirender::SceneDocument& expected) {
    require(actual.objects.size() == expected.objects.size() &&
                actual.meshes.size() == expected.meshes.size() &&
                actual.materials.size() == expected.materials.size() &&
                actual.media == expected.media,
            "Preset did not replace the complete scene");
    for (const auto& source : expected.objects) {
        const auto& loaded = object(actual, source.id);
        require(loaded.mesh_id == source.mesh_id && loaded.material_id == source.material_id &&
                    loaded.visible == source.visible &&
                    loaded.transform.translation == source.transform.translation &&
                    loaded.transform.rotation_degrees == source.transform.rotation_degrees &&
                    loaded.transform.scale == source.transform.scale,
                "Imported object differs from the template");
    }
}

minirender::IntegratorSettings test_integrator(const BuiltinScene& preset) {
    auto result = preset.integrator;
    result.maximum_depth = 8;
    result.sppm.photons_per_iteration = 2000;
    return result;
}

// 按模板积分器运行独立串行参考，覆盖 PT 与 SPPM 的真实结果。
void verify_render(Workbench& window, const QString& directory, const BuiltinScene& preset,
                   const std::shared_ptr<const minirender::SceneSnapshot>& snapshot) {
    auto* panel = window.offline_panel();
    require(panel->session().progress().state == minirender::RenderState::completed,
            "Preset render did not complete");
    const auto integrator = test_integrator(preset);
    const auto expected = [&] {
        if (integrator.kind == minirender::IntegratorKind::path_tracing) {
            return minirender::PathTracer(snapshot, integrator).render({24, 24, 16, 101});
        }
        minirender::SppmIntegrator reference(snapshot, {24, 24, 16, 101}, integrator);
        for (int iteration = 0; iteration < 16; ++iteration) {
            reference.advance();
        }
        require(reference.statistics().specular_light_contributions > 0,
                "Caustic preset has no specular light contributions");
        return reference.image();
    }();
    require(panel->session().progress().integrator == integrator.kind,
            "Preset selected the wrong integrator");
    const auto actual = panel->session().preview().image;
    require(actual && actual->width() == 24, "Preset preview dimensions differ");
    for (std::size_t y = 0; y < 24; ++y) {
        for (std::size_t x = 0; x < 24; ++x) {
            require(glm::length(actual->at(x, y) - expected.at(x, y)) < 1e-6F,
                    "Preset render differs from CPU reference");
        }
    }
    minirender::display_image(*actual).write_png(
        std::filesystem::path((directory + "/" + preset.id + ".png").toStdU16String()));
    require(window.viewport()->grab().save(directory + "/" + preset.id + "-gpu.png"),
            "Cannot capture preset GPU display");
}

} // namespace

void start_presets_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create preset test directory");
    const auto state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(80);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            require(++state->ticks < 1500, "Preset test timed out");
            auto* viewport = window.viewport();
            require(viewport->failure().isEmpty(), "Preset Vulkan renderer failed");
            if (!viewport->isValid() || viewport->rendered_frames() < 5) {
                return;
            }
            const auto& preset = builtin_scenes()[state->preset];
            if (state->stage == 0) {
                QFile file(builtin_scene_directory() + "/" + preset.filename);
                require(file.open(QIODevice::ReadOnly), "Cannot read source preset");
                state->original = file.readAll();
                state->original_scene = minirender::load_scene(std::filesystem::path(
                    (builtin_scene_directory() + "/" + preset.filename).toStdU16String()));
                auto* action = window.findChild<QAction*>("builtin_" + preset.id);
                require(action && window.findChild<QToolButton*>("builtin_scenes")->isEnabled(),
                        "Quick import action missing");
                action->trigger();
                require(window.asset_loading(), "Quick import did not start in the background");
                state->stage = 1;
                return;
            }
            if (window.asset_loading() ||
                !minirender::render_terminal(window.offline_panel()->session().progress().state)) {
                return;
            }
            if (state->stage == 1) {
                if (window.findChild<QSpinBox*>("offline_spp")->value() !=
                    static_cast<int>(preset.render.samples_per_pixel)) {
                    return;
                }
                verify_import(window.document(), state->original_scene);
                require(!viewport->offline_visible(), "Old offline image hid the imported scene");
                require(window.findChild<QSpinBox*>("offline_spp")->value() ==
                            static_cast<int>(preset.render.samples_per_pixel),
                        "Preset recommendations missing");
                require(window.findChild<QComboBox*>("offline_backend")->currentIndex() ==
                            (preset.integrator.kind == minirender::IntegratorKind::sppm ? 1 : 0),
                        "Preset backend recommendation missing");
                require(window.findChild<QSpinBox*>("offline_photons")->value() ==
                            int(preset.integrator.sppm.photons_per_iteration),
                        "Preset photon recommendation missing");
                window.select_object(preset.selected_object);
                const bool subsurface =
                    std::any_of(state->original_scene.materials.begin(),
                                state->original_scene.materials.end(), [](const auto& material) {
                                    return material.kind == minirender::MaterialKind::subsurface;
                                });
                const double displacement = subsurface ? .0007 : .07;
                auto* x = window.findChild<QDoubleSpinBox*>("translation.x");
                const auto before = x->value();
                x->setValue(before + displacement);
                require(x->value() != before, "Imported object cannot be edited");
                window.findChild<QAction*>("undo")->trigger();
                require(std::abs(x->value() - before) < 1e-5, "Imported object undo failed");
                window.findChild<QAction*>("redo")->trigger();
                const auto edited_material_id =
                    object(window.document(), preset.selected_object).material_id;
                if (subsurface) {
                    window.findChild<QDoubleSpinBox*>("subsurface_density")->setValue(.73);
                    window.findChild<QPushButton*>("subsurface_apply")->click();
                } else {
                    window.findChild<QDoubleSpinBox*>("roughness")->setValue(0.37);
                }
                std::string emitter_id, emitter_material_id;
                for (const auto& source : state->original_scene.objects) {
                    const auto material =
                        std::find_if(state->original_scene.materials.begin(),
                                     state->original_scene.materials.end(), [&](const auto& value) {
                                         return value.id == source.material_id;
                                     });
                    if (material != state->original_scene.materials.end() &&
                        glm::length(material->emission) > 0) {
                        emitter_id = source.id;
                        emitter_material_id = source.material_id;
                        break;
                    }
                }
                double new_light = 0;
                if (!emitter_id.empty()) {
                    window.select_object(emitter_id);
                    auto* emission = window.findChild<QDoubleSpinBox*>("emission.x");
                    new_light = emission->value() + 1;
                    emission->setValue(new_light);
                } else {
                    require(!window.document().directional_lights.empty(),
                            "Preset lighting fixture has no editable light");
                    auto* intensity = window.findChild<QDoubleSpinBox*>("light_intensity");
                    new_light = intensity->value() + 1;
                    intensity->setValue(new_light);
                }
                auto* camera_x = window.findChild<QDoubleSpinBox*>("camera_position.x");
                const double new_camera_x = camera_x->value() + (subsurface ? .0005 : .05);
                camera_x->setValue(new_camera_x);
                const auto saved = directory + "/编辑 " + preset.id + ".json";
                window.save_scene_file(saved);
                window.load_scene_file(saved);
                const auto object =
                    std::find_if(window.document().objects.begin(), window.document().objects.end(),
                                 [&](const auto& o) { return o.id == preset.selected_object; });
                require(object != window.document().objects.end() &&
                            std::abs(object->transform.translation.x - before - displacement) <
                                1e-5,
                        "Preset edits did not survive scene IO");
                const auto& materials = window.document().materials;
                const auto edited_material =
                    std::find_if(materials.begin(), materials.end(),
                                 [&](const auto& m) { return m.id == edited_material_id; });
                require(edited_material != materials.end(), "Imported material is missing");
                if (subsurface) {
                    require(edited_material->subsurface &&
                                edited_material->subsurface->density == .73,
                            "Imported subsurface edit was lost");
                } else {
                    require(std::abs(edited_material->roughness - .37F) < 1e-5F,
                            "Imported material edit was lost");
                }
                if (!emitter_id.empty()) {
                    const auto lamp_material =
                        std::find_if(materials.begin(), materials.end(), [&](const auto& value) {
                            return value.id == emitter_material_id;
                        });
                    require(lamp_material != materials.end() &&
                                std::abs(lamp_material->emission.x - new_light) < 1e-5,
                            "Area light edit was lost");
                } else {
                    require(std::abs(window.document().directional_lights[0].intensity -
                                     new_light) < 1e-5,
                            "Directional light edit was lost");
                }
                require(window.document().media == state->original_scene.media,
                        "Preset media changed during ordinary edits or scene IO");
                require(std::abs(window.document().camera.position.x - new_camera_x) < 1e-5,
                        "Camera edit was lost");
                window.offline_panel()->set_settings(
                    {24, 24, 16, 101}, test_integrator(preset),
                    {.threads = 2, .tile_size = 8, .batch_samples = 4});
                state->snapshot = minirender::make_snapshot(window.document());
                window.offline_panel()->start();
                state->stage = 2;
                return;
            }
            if (state->stage == 2) {
                verify_render(window, directory, preset, state->snapshot);
                QFile file(builtin_scene_directory() + "/" + preset.filename);
                require(file.open(QIODevice::ReadOnly) && file.readAll() == state->original,
                        "Editing changed the built-in template");
                if (state->preset + 1 == builtin_scenes().size()) {
                    timer->stop();
                    qInfo() << "Presets smoke PASS: replacement, editable objects, history, file "
                               "IO, recommendations and CPU/GPU offline results";
                    QApplication::exit(0);
                    return;
                }
                // 在旧任务运行时选择下一示例，检查异步取消和建议参数延后应用。
                window.offline_panel()->set_settings(
                    {64, 64, 1000000, 101}, {.maximum_depth = 8},
                    {.threads = 2, .tile_size = 8, .batch_samples = 4});
                window.offline_panel()->start();
                ++state->preset;
                state->stage = 0;
            }
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Presets smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
