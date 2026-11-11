#include "cloth_playback_smoke.hpp"
#include "cloth_controller.hpp"
#include "cloth_playback_deep_smoke.hpp"
#include "geometry_smoke.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* value = window.findChild<T*>(name);
    require(value != nullptr, "Simulation control is missing");
    return value;
}
void click(Workbench& window, const char* name) {
    control<QPushButton>(window, name)->click();
}
const minirender::SceneObject& target(Workbench& window) {
    const auto& objects = window.document().objects;
    const auto found = std::find_if(objects.begin(), objects.end(),
                                    [](const auto& object) { return object.id == "cloth"; });
    require(found != objects.end(), "Simulation target is missing");
    return *found;
}
struct State {
    int stage = 0, frame = 0, ticks = 0, paused_ticks = 0;
    std::uint64_t paused_frame = 0;
    std::shared_ptr<const minirender::ClothComponent> initial;
    std::shared_ptr<const minirender::ClothComponent> expected;
    SceneUploadStatistics uploads;
    UiResponseProbe* probe = nullptr;
};
} // namespace

void start_cloth_playback_smoke(Workbench& window, const QString& directory,
                                bool enforce_response) {
    auto state = std::make_shared<State>();
    state->probe = new UiResponseProbe(&window, "Simulation");
    auto* timer = new QTimer(&window);
    timer->setInterval(50);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            try {
                auto* viewport = window.viewport();
                auto* controller = window.cloth_controller();
                require(++state->ticks < 2500 && viewport->failure().isEmpty(),
                        "Simulation smoke timed out");
                const auto status = controller->status();
                require(!status.failed, status.message.toStdString().c_str());
                if (viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto advance = [&] {
                    ++state->stage;
                    state->frame = viewport->rendered_frames();
                };
                switch (state->stage) {
                case 0:
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(9);
                    click(window, "cloth_demo");
                    require(target(window).cloth != nullptr, "Demo has no simulation component");
                    state->initial = target(window).cloth;
                    state->probe->verify([&] {
                        auto simulation = minirender::make_cloth_simulation(
                            *state->initial, target(window).transform,
                            minirender::cloth_environment(*state->initial->definition(),
                                                          window.document()));
                        simulation->advance();
                        state->expected =
                            minirender::prepare_cloth_frame(
                                *simulation, state->initial->definition(), target(window).transform)
                                .component;
                    });
                    advance();
                    return;
                case 1:
                    require(status.bound && !status.busy, "Demo controller did not bind");
                    state->uploads = viewport->upload_statistics();
                    click(window, "cloth_step");
                    advance();
                    return;
                case 2:
                    if (status.busy) {
                        return;
                    }
                    require(target(window).cloth->state().frame == 1,
                            "Single step did not advance one frame");
                    require(target(window).cloth->state().positions ==
                                state->expected->state().positions,
                            "UI step differs from independent core step");
                    require(target(window).cloth->state().velocities ==
                                state->expected->state().velocities,
                            "UI step velocities differ from core");
                    require(viewport->upload_statistics().mesh_allocations ==
                                    state->uploads.mesh_allocations &&
                                viewport->upload_statistics().index_uploads ==
                                    state->uploads.index_uploads,
                            "Single frame allocated mesh or uploaded indices");
                    require(viewport->upload_statistics().vertex_buffer_reuses >
                                state->uploads.vertex_buffer_reuses,
                            "Dynamic frame did not reuse its vertex buffer");
                    click(window, "cloth_play");
                    advance();
                    return;
                case 3:
                    if (target(window).cloth->state().frame < 15) {
                        return;
                    }
                    click(window, "cloth_pause");
                    advance();
                    return;
                case 4:
                    if (status.busy) {
                        return;
                    }
                    if (state->paused_ticks++ == 0) {
                        state->paused_frame = target(window).cloth->state().frame;
                    }
                    require(target(window).cloth->state().frame == state->paused_frame,
                            "Paused simulation advanced");
                    if (state->paused_ticks < 5) {
                        return;
                    }
                    require(viewport->upload_statistics().mesh_allocations ==
                                    state->uploads.mesh_allocations &&
                                viewport->upload_statistics().index_uploads ==
                                    state->uploads.index_uploads,
                            "Playback repeatedly allocated buffers or uploaded indices");
                    state->probe->verify([&] {
                        verify_geometry_gpu(window, directory, "simulation-paused", "cloth");
                    });
                    click(window, "cloth_reset");
                    advance();
                    return;
                case 5:
                    if (status.busy) {
                        return;
                    }
                    require(target(window).cloth->state().frame == 0,
                            "Reset did not restore frame zero");
                    require(target(window).cloth->state().positions ==
                                state->initial->state().positions,
                            "Reset did not restore rest positions");
                    control<QAction>(window, "undo")->trigger();
                    require(target(window).cloth->state().frame == state->paused_frame,
                            "Reset undo did not restore its pose");
                    control<QAction>(window, "redo")->trigger();
                    require(target(window).cloth->state().frame == 0, "Reset redo failed");
                    window.select_object("support");
                    control<QDoubleSpinBox>(window, "translation.x")->setValue(.25);
                    window.select_object("cloth");
                    control<QDoubleSpinBox>(window, "rotation.y")->setValue(15);
                    click(window, "cloth_step");
                    advance();
                    return;
                case 6:
                    if (status.busy) {
                        return;
                    }
                    require(target(window).cloth->state().frame == 1,
                            "Transformed cloth did not resume");
                    state->probe->verify([&] {
                        verify_geometry_gpu(window, directory, "simulation-transformed", "cloth");
                    });
                    window.save_scene_async(directory + "/布料 播放.json");
                    advance();
                    return;
                case 7:
                    if (window.asset_loading()) {
                        return;
                    }
                    window.open_scene_file(directory + "/布料 播放.json");
                    advance();
                    return;
                case 8:
                    if (window.asset_loading()) {
                        return;
                    }
                    window.select_object("cloth");
                    require(target(window).cloth->state().frame == 1,
                            "File load lost simulation state");
                    click(window, "cloth_step");
                    advance();
                    return;
                case 9:
                    if (status.busy) {
                        return;
                    }
                    require(target(window).cloth->state().frame == 2,
                            "Reopened state did not resume");
                    click(window, "cloth_freeze");
                    require(target(window).cloth == nullptr, "Freeze kept the live component");
                    control<QAction>(window, "undo")->trigger();
                    require(target(window).cloth && target(window).cloth->state().frame == 2,
                            "Freeze undo lost current state");
                    control<QAction>(window, "redo")->trigger();
                    require(target(window).cloth == nullptr, "Freeze redo failed");
                    state->probe->report(enforce_response);
                    timer->stop();
                    qInfo() << "Simulation basic PASS: exact step, play/pause/reset, buffer reuse, "
                               "transforms, files and freeze";
                    start_cloth_playback_deep_smoke(window, directory, enforce_response);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Simulation smoke FAIL stage" << state->stage << ':' << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
