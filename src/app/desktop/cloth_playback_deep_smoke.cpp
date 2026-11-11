#include "cloth_playback_deep_smoke.hpp"
#include "cloth_controller.hpp"
#include "geometry_smoke.hpp"
#include "offline_panel.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/render/path_tracer.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
using namespace minirender;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* result = window.findChild<T*>(name);
    require(result, "Cloth playback control is missing");
    return result;
}
void click(Workbench& window, const char* name) {
    auto* button = control<QPushButton>(window, name);
    require(button->isEnabled(), "Cloth playback button is disabled");
    button->click();
}
const SceneObject& object(Workbench& window, const std::string& id = "cloth") {
    const auto& objects = window.document().objects;
    const auto found = std::find_if(objects.begin(), objects.end(),
                                    [&](const auto& item) { return item.id == id; });
    require(found != objects.end(), "Cloth test object is missing");
    return *found;
}
const std::shared_ptr<const ClothComponent>& component(Workbench& window) {
    const auto& result = object(window).cloth;
    require(result != nullptr, "Cloth test component is missing");
    return result;
}
void mouse(VulkanViewport& viewport, QEvent::Type type, glm::dvec2 point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    const QPointF local(point.x, point.y);
    QMouseEvent event(type, local, viewport.mapToGlobal(local), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&viewport, &event);
}

// 根据实际屏幕布局选取手柄，拖动通过真实鼠标事件进入 Workbench 的事务流程。
struct Gesture {
    Transform original;
    glm::dvec2 start{0}, end{0};
};
Gesture begin_gesture(Workbench& window, GizmoMode mode) {
    auto& viewport = *window.viewport();
    window.select_object("cloth");
    viewport.set_gizmo_mode(mode);
    const auto& layout = viewport.gizmo_layout();
    const auto original = object(window).transform;
    const auto forward = glm::normalize(glm::dvec3(viewport.camera().target) -
                                        glm::dvec3(viewport.camera().position));
    for (const auto& segment : layout.segments) {
        const auto start = segment.a * .35 + segment.b * .65;
        if (segment.handle == GizmoHandle::none || layout.hit(start).handle != segment.handle) {
            continue;
        }
        glm::dvec2 end;
        if (mode == GizmoMode::translate) {
            if (segment.handle != GizmoHandle::x || glm::length(segment.b - segment.a) < 20) {
                continue;
            }
            end = start + glm::normalize(segment.b - segment.a) * 35.0;
        } else {
            const glm::dvec3 axis = segment.handle == GizmoHandle::x   ? glm::dvec3(1, 0, 0)
                                    : segment.handle == GizmoHandle::y ? glm::dvec3(0, 1, 0)
                                                                       : glm::dvec3(0, 0, 1);
            if (std::abs(glm::dot(axis, forward)) < .5) {
                continue;
            }
            const auto radial = segment.world_a * .35 + segment.world_b * .65 - layout.pivot;
            const auto rotated =
                glm::dvec3(glm::rotate(glm::dmat4(1), .25, axis) * glm::dvec4(radial, 0));
            const GizmoView view{viewport.camera(), {viewport.width(), viewport.height()}};
            const auto projected = view.project(layout.pivot + rotated);
            require(projected.has_value(), "Rotation endpoint is outside the viewport");
            end = *projected;
        }
        mouse(viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        require(viewport.gizmo_dragging(), "Cloth handle did not begin dragging");
        require(!window.cloth_controller()->status().playing,
                "Dragging did not pause the simulation");
        return {original, start, end};
    }
    throw std::runtime_error("No cloth gizmo handle is selectable");
}
void finish_gesture(Workbench& window, const Gesture& gesture, GizmoMode mode) {
    auto& viewport = *window.viewport();
    mouse(viewport, QEvent::MouseMove, gesture.end, Qt::NoButton, Qt::LeftButton);
    mouse(viewport, QEvent::MouseButtonRelease, gesture.end, Qt::LeftButton, Qt::NoButton);
    require(!viewport.gizmo_dragging(), "Cloth gesture did not finish");
    const auto& actual = object(window).transform;
    require(mode == GizmoMode::translate
                ? glm::length(actual.translation - gesture.original.translation) > .01F
                : glm::length(actual.rotation_degrees - gesture.original.rotation_degrees) > 1,
            "Cloth gesture did not change its transform");
}

void verify_layout(Workbench& window, const QString& directory) {
    auto* tabs = control<QTabWidget>(window, "inspector_tabs");
    tabs->setCurrentIndex(9);
    auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
    auto* panel = control<QWidget>(window, "cloth_panel");
    require(area, "Cloth panel has no scroll area");
    for (const auto size : {QSize(1200, 800), QSize(960, 700)}) {
        window.resize(size);
        window.layout()->activate();
        if (auto* layout = area->layout()) {
            layout->activate();
        }
        panel->layout()->activate();
        area->verticalScrollBar()->setValue(0);
        capture_workbench_ui(window,
                             directory + QString("/simulation-layout-%1.png").arg(size.width()));
        auto* form = qobject_cast<QFormLayout*>(panel->layout());
        require(form, "Cloth form layout is missing");
        std::vector<QWidget*> fields;
        QWidget* bottom = nullptr;
        for (int i = 0; i < form->count(); ++i) {
            auto* field = form->itemAt(i)->widget();
            if (!field || !field->isVisibleTo(panel) || field->height() == 0) {
                continue;
            }
            const auto rect = field->geometry();
            require(rect.left() >= 0 && rect.right() < panel->width(),
                    "Cloth field exceeds panel bounds");
            for (const auto* other : fields) {
                const auto overlap = rect.intersected(other->geometry());
                require(overlap.width() <= 1 || overlap.height() <= 1,
                        "Cloth panel fields overlap");
            }
            fields.push_back(field);
            if (!bottom || rect.bottom() > bottom->geometry().bottom()) {
                bottom = field;
            }
        }
        require(fields.size() >= 35 && bottom, "Cloth layout omitted controls");
        area->ensureWidgetVisible(bottom);
        require(area->viewport()->rect().contains(
                    bottom->mapTo(area->viewport(), bottom->rect().center())),
                "Cloth bottom controls are inaccessible");
        capture_workbench_ui(window,
                             directory + QString("/simulation-bottom-%1.png").arg(size.width()));
        qInfo() << "Simulation layout" << size << fields.size();
    }
}

enum class Stage {
    setup,
    edit_while_playing,
    translate_begin,
    translate_move,
    translate_finish,
    rotate_begin,
    rotate_move,
    rotate_finish,
    offline_play,
    offline_start,
    offline_done,
    offline_stale,
    configure,
    configured,
    config_step,
    collider_setup,
    collider_step,
    contacts,
    shared_setup,
    shared_step,
    shared_reuse,
    heavy_configure,
    heavy_ready,
    cancel,
    cancelled,
    delete_busy,
    deleted,
    replace_busy,
    replaced,
    shutdown_ready,
    shutdown
};
struct State {
    Stage stage = Stage::setup;
    int ticks = 0, gpu_frame = 0;
    std::uint64_t frame = 0;
    Gesture gesture;
    std::shared_ptr<const ClothComponent> expected, preserved;
    std::shared_ptr<const SceneSnapshot> snapshot;
    std::shared_ptr<const MeshData> static_mesh;
    std::string copy_id;
    SceneUploadStatistics uploads;
    QElapsedTimer cancellation;
    UiResponseProbe* probe = nullptr;
};
void expect_step(Workbench& window, State& state) {
    state.probe->verify([&] {
        const auto& source = object(window);
        auto simulation = make_cloth_simulation(
            *source.cloth, source.transform,
            cloth_environment(*source.cloth->definition(), window.document()));
        simulation->advance();
        state.expected =
            prepare_cloth_frame(*simulation, source.cloth->definition(), source.transform)
                .component;
    });
}
void verify_step(Workbench& window, State& state) {
    const auto& actual = component(window)->state();
    const auto& expected = state.expected->state();
    require(actual.frame == expected.frame && actual.positions == expected.positions &&
                actual.velocities == expected.velocities,
            "Edited simulation differs from independently restored world-space step");
}
void heavy_parameters(Workbench& window) {
    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(9);
    control<QSpinBox>(window, "cloth_columns")->setValue(128);
    control<QSpinBox>(window, "cloth_rows")->setValue(128);
    control<QSpinBox>(window, "cloth_substeps")->setValue(4096);
    click(window, "cloth_apply");
}
bool advancing(const ClothRuntimeStatus& status) {
    return status.busy && status.stage == "推进子步" && status.completed > 0 &&
           status.completed < status.total;
}
} // namespace

void start_cloth_playback_deep_smoke(Workbench& window, const QString& directory,
                                     bool enforce_response) {
    auto state = std::make_shared<State>();
    state->probe = new UiResponseProbe(&window, "Simulation editing");
    auto* timer = new QTimer(&window);
    timer->setInterval(50);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            try {
                auto* controller = window.cloth_controller();
                auto* viewport = window.viewport();
                const auto status = controller->status();
                require(++state->ticks < 3500 && viewport->failure().isEmpty(),
                        "Cloth editing regression timed out");
                require(!status.failed, status.message.toStdString().c_str());
                if (viewport->rendered_frames() <= state->gpu_frame + 2) {
                    return;
                }
                const auto next = [&] {
                    state->stage = static_cast<Stage>(static_cast<int>(state->stage) + 1);
                    state->gpu_frame = viewport->rendered_frames();
                    qInfo() << "Simulation stage" << static_cast<int>(state->stage);
                };
                switch (state->stage) {
                case Stage::setup:
                    window.create_cloth_demo(1);
                    controller->play();
                    next();
                    return;
                case Stage::edit_while_playing:
                    if (component(window)->state().frame < 4) {
                        return;
                    }
                    window.select_object("cloth");
                    control<QDoubleSpinBox>(window, "base_color.x")->setValue(.35);
                    control<QDoubleSpinBox>(window, "fov")->setValue(48);
                    require(controller->status().playing, "Material/camera edit paused physics");
                    state->frame = component(window)->state().frame;
                    next();
                    return;
                case Stage::translate_begin:
                    if (component(window)->state().frame < state->frame + 3) {
                        return;
                    }
                    state->gesture = begin_gesture(window, GizmoMode::translate);
                    state->preserved = component(window);
                    next();
                    return;
                case Stage::translate_move:
                    mouse(*viewport, QEvent::MouseMove,
                          (state->gesture.start + state->gesture.end) * .5, Qt::NoButton,
                          Qt::LeftButton);
                    next();
                    return;
                case Stage::translate_finish:
                    finish_gesture(window, state->gesture, GizmoMode::translate);
                    require(component(window) == state->preserved,
                            "Dragging published an old simulation frame");
                    expect_step(window, *state);
                    controller->step();
                    next();
                    return;
                case Stage::rotate_begin:
                    if (status.busy) {
                        return;
                    }
                    verify_step(window, *state);
                    state->gesture = begin_gesture(window, GizmoMode::rotate);
                    next();
                    return;
                case Stage::rotate_move:
                    mouse(*viewport, QEvent::MouseMove,
                          (state->gesture.start + state->gesture.end) * .5, Qt::NoButton,
                          Qt::LeftButton);
                    next();
                    return;
                case Stage::rotate_finish:
                    finish_gesture(window, state->gesture, GizmoMode::rotate);
                    expect_step(window, *state);
                    controller->step();
                    next();
                    return;
                case Stage::offline_play:
                    if (status.busy) {
                        return;
                    }
                    verify_step(window, *state);
                    // 操纵器覆盖层不参与 CPU 场景求交；成像对照前收起手柄。
                    viewport->set_gizmo_mode(GizmoMode::select);
                    state->probe->verify([&] {
                        verify_geometry_gpu(window, directory, "simulation-gizmos", "cloth");
                    });
                    state->frame = component(window)->state().frame;
                    controller->play();
                    next();
                    return;
                case Stage::offline_start:
                    if (component(window)->state().frame < state->frame + 3) {
                        return;
                    }
                    state->preserved = component(window);
                    state->probe->verify(
                        [&] { state->snapshot = make_snapshot(window.document()); });
                    window.offline_panel()->set_settings({16, 16, 8, 101}, {.maximum_depth = 4},
                                                         {2, 8, 2});
                    window.offline_panel()->start();
                    require(!controller->status().playing, "Offline start did not pause cloth");
                    next();
                    return;
                case Stage::offline_done: {
                    const auto progress = window.offline_panel()->session().progress();
                    require(progress.state != RenderState::failed, "Cloth offline task failed");
                    if (status.busy || progress.state != RenderState::completed) {
                        return;
                    }
                    require(component(window) == state->preserved,
                            "Offline snapshot was followed by a late cloth frame");
                    state->probe->verify([&] {
                        const auto reference = PathTracer(state->snapshot, {.maximum_depth = 4})
                                                   .render({16, 16, 8, 101});
                        const auto actual = window.offline_panel()->session().result();
                        require(actual != nullptr, "Cloth offline result is missing");
                        for (std::size_t y = 0; y < 16; ++y) {
                            for (std::size_t x = 0; x < 16; ++x) {
                                require(
                                    glm::length(actual->at(x, y) - reference.at(x, y)) < 1e-6F,
                                    "Cloth offline image differs from fixed snapshot reference");
                            }
                        }
                    });
                    state->frame = component(window)->state().frame;
                    click(window, "cloth_play");
                    require(!viewport->offline_visible(),
                            "Play button did not return to the scene preview");
                    next();
                    return;
                }
                case Stage::offline_stale:
                    if (component(window)->state().frame == state->frame) {
                        return;
                    }
                    require(window.offline_panel()->stale(),
                            "Playback did not stale offline image");
                    controller->pause();
                    next();
                    return;
                case Stage::configure:
                    if (status.busy) {
                        return;
                    }
                    state->preserved = component(window);
                    control<QSpinBox>(window, "cloth_columns")->setValue(17);
                    control<QSpinBox>(window, "cloth_rows")->setValue(13);
                    control<QDoubleSpinBox>(window, "cloth_density")->setValue(.25);
                    control<QDoubleSpinBox>(window, "cloth_wind.2")->setValue(.8);
                    control<QComboBox>(window, "cloth_pins")->setCurrentIndex(2);
                    click(window, "cloth_apply");
                    next();
                    return;
                case Stage::configured:
                    if (status.busy) {
                        return;
                    }
                    require(component(window)->state().frame == 0 &&
                                component(window)->state().positions.size() == 17 * 13 &&
                                component(window)->definition()->grid.pinned.size() == 17 &&
                                component(window)->definition()->parameters.surface_density ==
                                    .25 &&
                                component(window)->definition()->environment.wind_velocity.z == .8,
                            "Cloth parameter apply lost grid, pins or forces");
                    control<QAction>(window, "undo")->trigger();
                    require(component(window) == state->preserved, "Parameter undo lost old pose");
                    control<QAction>(window, "redo")->trigger();
                    require(component(window)->state().frame == 0, "Parameter redo lost rest pose");
                    expect_step(window, *state);
                    controller->step();
                    next();
                    return;
                case Stage::config_step:
                    if (status.busy) {
                        return;
                    }
                    verify_step(window, *state);
                    state->probe->verify([&] { verify_layout(window, directory); });
                    window.create_cloth_demo(0);
                    require(!viewport->offline_visible(),
                            "Cloth demo kept the previous offline display active");
                    next();
                    return;
                case Stage::collider_setup:
                    window.select_object("support");
                    control<QDoubleSpinBox>(window, "cloth_collider_friction")->setValue(.7);
                    control<QCheckBox>(window, "cloth_collider_enabled")->setChecked(false);
                    require(
                        object(window, "support").cloth_collider->friction == .7 &&
                            cloth_environment(*component(window)->definition(), window.document())
                                .spheres.empty(),
                        "Collider controls did not edit physics");
                    expect_step(window, *state);
                    controller->step();
                    next();
                    return;
                case Stage::collider_step:
                    if (status.busy) {
                        return;
                    }
                    verify_step(window, *state);
                    control<QCheckBox>(window, "cloth_collider_enabled")->setChecked(true);
                    control<QDoubleSpinBox>(window, "translation.x")->setValue(.25);
                    controller->play();
                    next();
                    return;
                case Stage::contacts:
                    if (component(window)->state().frame < 90) {
                        return;
                    }
                    controller->pause();
                    next();
                    return;
                case Stage::shared_setup: {
                    if (status.busy) {
                        return;
                    }
                    require(status.last_steps.contacts > 0,
                            "Moved sphere produced no cloth contact");
                    const auto environment =
                        cloth_environment(*component(window)->definition(), window.document());
                    const auto& sphere = environment.spheres.at(0);
                    const glm::dmat4 matrix(object(window).transform.matrix());
                    for (const auto position : component(window)->state().positions) {
                        const auto world = glm::dvec3(matrix * glm::dvec4(position, 1));
                        require(glm::length(world - sphere.center) >= sphere.radius - 1e-6,
                                "Cloth penetrates the moved scene sphere");
                    }
                    window.select_object("cloth");
                    control<QAction>(window, "duplicate")->trigger();
                    state->copy_id = window.document().objects.back().id;
                    control<QDoubleSpinBox>(window, "translation.x")->setValue(2);
                    auto* targets = control<QComboBox>(window, "cloth_target");
                    targets->setCurrentIndex(
                        targets->findData(QString::fromStdString(state->copy_id)));
                    require(controller->status().frame == component(window)->state().frame,
                            "Switching the simulation target lost its state");
                    click(window, "cloth_freeze");
                    require(!object(window, state->copy_id).cloth,
                            "Target selection froze the wrong component");
                    state->static_mesh = component(window)->mesh();
                    state->uploads = viewport->upload_statistics();
                    // 先等待 duplicate/freeze 的场景上传，再记录实际分配计数。
                    next();
                    return;
                }
                case Stage::shared_step:
                    state->uploads = viewport->upload_statistics();
                    controller->step();
                    next();
                    return;
                case Stage::shared_reuse:
                    if (status.busy) {
                        return;
                    }
                    if (viewport->upload_statistics().mesh_uploads == state->uploads.mesh_uploads) {
                        return;
                    }
                    qInfo() << "Simulation shared allocations"
                            << viewport->upload_statistics().mesh_allocations -
                                   state->uploads.mesh_allocations;
                    require(viewport->upload_statistics().mesh_allocations ==
                                state->uploads.mesh_allocations + 1,
                            "Shared visible owner was reused by a dynamic frame");
                    require(object(window, state->copy_id).mesh_id != object(window).mesh_id,
                            "Duplicate shares mutable mesh asset identity");
                    state->probe->verify([&] {
                        verify_geometry_gpu(window, directory, "simulation-shared", "cloth");
                    });
                    state->uploads = viewport->upload_statistics();
                    controller->step();
                    next();
                    return;
                case Stage::heavy_configure:
                    if (status.busy) {
                        return;
                    }
                    if (viewport->upload_statistics().mesh_uploads == state->uploads.mesh_uploads) {
                        return;
                    }
                    require(viewport->upload_statistics().mesh_allocations ==
                                    state->uploads.mesh_allocations &&
                                viewport->upload_statistics().index_uploads ==
                                    state->uploads.index_uploads,
                            "Unshared dynamic frame failed to reuse its buffers");
                    require(std::any_of(window.document().meshes.begin(),
                                        window.document().meshes.end(),
                                        [&](const auto& asset) {
                                            return asset.data == state->static_mesh;
                                        }),
                            "Static duplicate mesh was discarded");
                    heavy_parameters(window);
                    next();
                    return;
                case Stage::heavy_ready:
                    if (status.busy) {
                        return;
                    }
                    require(component(window)->state().positions.size() == 128 * 128,
                            "Heavy simulation configuration was not applied");
                    state->preserved = component(window);
                    controller->step();
                    next();
                    return;
                case Stage::cancel:
                    if (!advancing(status)) {
                        return;
                    }
                    require(control<QProgressBar>(window, "cloth_progress")
                                ->isVisibleTo(control<QWidget>(window, "cloth_panel")),
                            "Long simulation has no progress indicator");
                    state->cancellation.start();
                    click(window, "cloth_pause");
                    next();
                    return;
                case Stage::cancelled:
                    if (status.busy) {
                        return;
                    }
                    require(component(window) == state->preserved,
                            "Cancelled substeps changed the visible component");
                    qInfo() << "Simulation cancellation ms" << state->cancellation.elapsed();
                    require(state->cancellation.elapsed() < 2000,
                            "Cloth cancellation was too slow");
                    controller->step();
                    next();
                    return;
                case Stage::delete_busy:
                    if (!advancing(status)) {
                        return;
                    }
                    window.select_object("cloth");
                    control<QAction>(window, "delete")->trigger();
                    next();
                    return;
                case Stage::deleted:
                    if (status.busy) {
                        return;
                    }
                    require(!status.bound, "Deleted cloth remains bound to a worker");
                    require(std::none_of(window.document().objects.begin(),
                                         window.document().objects.end(),
                                         [](const auto& item) { return item.id == "cloth"; }),
                            "A late simulation result restored the deleted object");
                    control<QAction>(window, "undo")->trigger();
                    require(component(window) == state->preserved,
                            "Delete undo lost the heavy pose");
                    controller->step();
                    next();
                    return;
                case Stage::replace_busy:
                    if (!advancing(status)) {
                        return;
                    }
                    window.create_cloth_demo(1);
                    next();
                    return;
                case Stage::replaced:
                    if (status.busy) {
                        return;
                    }
                    require(component(window)->state().frame == 0 &&
                                component(window)->state().positions.size() == 625 &&
                                !status.playing,
                            "Old worker changed the replacement scene");
                    heavy_parameters(window);
                    next();
                    return;
                case Stage::shutdown_ready:
                    if (status.busy) {
                        return;
                    }
                    controller->step();
                    next();
                    return;
                case Stage::shutdown:
                    if (!advancing(status)) {
                        return;
                    }
                    state->probe->report(enforce_response);
                    timer->stop();
                    qInfo()
                        << "Simulation smoke PASS: playback, edits, gizmos, offline snapshot, "
                           "parameters, colliders, shared buffers, layout, cancellation and exit";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Simulation editing FAIL stage" << static_cast<int>(state->stage)
                            << ':' << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
