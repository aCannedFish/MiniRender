#include "presets_smoke.hpp"
#include "builtin_scenes.hpp"
#include "offline_panel.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
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
    std::shared_ptr<const minirender::SceneSnapshot> snapshot;
};

// 独立串行路径积分核对会话输出，线程调度和预览上传不改变固定种子结果。
void verify_render(Workbench& window, const QString& directory, const BuiltinScene& preset,
                   const std::shared_ptr<const minirender::SceneSnapshot>& snapshot) {
    auto* panel = window.offline_panel();
    require(panel->session().progress().state == minirender::RenderState::completed,
            "Preset render did not complete");
    const minirender::PathTracer reference(snapshot, {.maximum_depth = 8});
    const auto expected = reference.render({24, 24, 16, 101});
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
                require(window.document().objects.size() == 8,
                        "Preset did not replace the previous scene");
                require(std::none_of(window.document().objects.begin(),
                                     window.document().objects.end(),
                                     [](const auto& o) { return o.id == "blue_cube"; }),
                        "Old scene object survived replacement");
                require(!viewport->offline_visible(), "Old offline image hid the imported scene");
                require(window.findChild<QSpinBox*>("offline_spp")->value() ==
                            static_cast<int>(preset.render.samples_per_pixel),
                        "Preset recommendations missing");
                window.select_object(preset.selected_object);
                auto* x = window.findChild<QDoubleSpinBox*>("translation.x");
                const auto before = x->value();
                x->setValue(before + 0.07);
                require(x->value() != before, "Imported object cannot be edited");
                window.findChild<QAction*>("undo")->trigger();
                require(std::abs(x->value() - before) < 1e-5, "Imported object undo failed");
                window.findChild<QAction*>("redo")->trigger();
                window.findChild<QDoubleSpinBox*>("roughness")->setValue(0.37);
                window.select_object("lamp");
                auto* emission = window.findChild<QDoubleSpinBox*>("emission.x");
                const double new_emission = emission->value() + 1;
                emission->setValue(new_emission);
                auto* camera_x = window.findChild<QDoubleSpinBox*>("camera_position.x");
                const double new_camera_x = camera_x->value() + 0.05;
                camera_x->setValue(new_camera_x);
                const auto saved = directory + "/编辑 " + preset.id + ".json";
                window.save_scene_file(saved);
                window.load_scene_file(saved);
                const auto object =
                    std::find_if(window.document().objects.begin(), window.document().objects.end(),
                                 [&](const auto& o) { return o.id == preset.selected_object; });
                require(object != window.document().objects.end() &&
                            std::abs(object->transform.translation.x - before - 0.07) < 1e-5,
                        "Preset edits did not survive scene IO");
                const auto& materials = window.document().materials;
                const auto edited_material =
                    std::find_if(materials.begin(), materials.end(), [&](const auto& m) {
                        return m.id == "mat_" + preset.selected_object;
                    });
                const auto lamp_material =
                    std::find_if(materials.begin(), materials.end(),
                                 [](const auto& m) { return m.id == "mat_lamp"; });
                require(edited_material != materials.end() &&
                            std::abs(edited_material->roughness - .37F) < 1e-5F,
                        "Imported material edit was lost");
                require(lamp_material != materials.end() &&
                            std::abs(lamp_material->emission.x - new_emission) < 1e-5,
                        "Area light edit was lost");
                require(std::abs(window.document().camera.position.x - new_camera_x) < 1e-5,
                        "Camera edit was lost");
                window.offline_panel()->set_settings(
                    {24, 24, 16, 101}, {.maximum_depth = 8},
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
