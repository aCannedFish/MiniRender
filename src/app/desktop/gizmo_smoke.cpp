#include "gizmo_smoke.hpp"
#include "geometry_smoke.hpp"
#include "ibl_controller.hpp"
#include "offline_panel.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {

using Mode = minirender::GizmoMode;
using Handle = minirender::GizmoHandle;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* value = window.findChild<T*>(name);
    require(value != nullptr, "Gizmo regression control is missing");
    return value;
}
minirender::Transform transform(const Workbench& window) {
    for (const auto& object : window.document().objects) {
        if (object.id == "blue_cube") {
            return object.transform;
        }
    }
    throw std::runtime_error("Gizmo fixture requires blue_cube");
}
bool equal(const minirender::Transform& a, const minirender::Transform& b) {
    return a.translation == b.translation && a.rotation_degrees == b.rotation_degrees &&
           a.scale == b.scale;
}
bool close(const glm::mat4& actual, const glm::dmat4& expected) {
    for (int column = 0; column < 4; ++column) {
        if (glm::length(glm::dvec4(actual[column]) - expected[column]) > 3e-5) {
            return false;
        }
    }
    return true;
}
glm::dvec3 axis(Handle handle) {
    return handle == Handle::x   ? glm::dvec3(1, 0, 0)
           : handle == Handle::y ? glm::dvec3(0, 1, 0)
                                 : glm::dvec3(0, 0, 1);
}
minirender::GizmoView view(VulkanViewport& viewport) {
    return {viewport.camera(), {viewport.width(), viewport.height()}};
}
glm::dvec2 project(VulkanViewport& viewport, glm::dvec3 position) {
    const auto result = view(viewport).project(position);
    require(result.has_value(), "Gizmo test point is outside the camera depth range");
    return *result;
}

enum class Phase {
    startup,
    move,
    moved,
    undo_move,
    redo_move,
    restore_move,
    cancel_move,
    cancelled,
    rotate_start,
    rotate,
    rotated,
    undo_rotate,
    redo_rotate,
    ungrab_start,
    ungrab_move,
    ungrab_finish,
    hide_start,
    hide_move,
    hide_finish,
    save,
    reopen,
    offline,
    finish
};

struct State {
    Phase phase = Phase::startup;
    int ticks = 0, step = 0, frame = 0;
    double maximum_callback_ms = 0;
    minirender::Transform original, moved, rotated, gesture_start;
    minirender::Camera camera;
    glm::dvec2 start{0}, delta{0};
    glm::dvec3 radial{0}, rotation_axis{0};
    double radius = 0;
    Handle rotation_handle = Handle::none;
    std::uint64_t uploads = 0, descriptors = 0;
    QImage baseline, moved_image, rotated_image;
    QString source_scene;
};

void mouse(State& state, VulkanViewport& viewport, QEvent::Type type, glm::dvec2 point,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
    const QPointF local(point.x, point.y);
    QMouseEvent event(type, local, viewport.mapToGlobal(local), button, buttons, Qt::NoModifier);
    QElapsedTimer clock;
    clock.start();
    QCoreApplication::sendEvent(&viewport, &event);
    state.maximum_callback_ms =
        std::max(state.maximum_callback_ms, static_cast<double>(clock.nsecsElapsed()) / 1000000.0);
}

void begin_axis(State& state, VulkanViewport& viewport) {
    viewport.set_gizmo_mode(Mode::translate);
    const auto& layout = viewport.gizmo_layout();
    bool found = false;
    for (const auto& segment : layout.segments) {
        const auto point = segment.a * 0.35 + segment.b * 0.65;
        if (segment.handle == Handle::x && glm::length(segment.b - segment.a) > 20 &&
            layout.hit(point).handle == Handle::x) {
            state.start = point;
            state.delta = glm::normalize(segment.b - segment.a) * 45.0;
            found = true;
            break;
        }
    }
    require(found, "X translation handle is not visible or selectable");
    mouse(state, viewport, QEvent::MouseButtonPress, state.start, Qt::LeftButton, Qt::LeftButton);
    require(viewport.gizmo_dragging(), "Pressing a gizmo handle did not begin a gesture");
}

void verify_overlay(VulkanViewport& viewport, const QString& path, Handle handle) {
    const auto image = viewport.grab();
    const auto& layout = viewport.gizmo_layout();
    const std::array<int, 3> expected = handle == Handle::x   ? std::array{245, 61, 51}
                                        : handle == Handle::y ? std::array{77, 224, 97}
                                                              : std::array{71, 140, 255};
    bool matched = false;
    for (const auto& segment : layout.segments) {
        if (segment.handle != handle) {
            continue;
        }
        const auto point = segment.a * 0.4 + segment.b * 0.6;
        const int x = static_cast<int>(point.x * image.width() / viewport.width());
        const int y = static_cast<int>(point.y * image.height() / viewport.height());
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                if (x + dx < 0 || y + dy < 0 || x + dx >= image.width() ||
                    y + dy >= image.height()) {
                    continue;
                }
                const auto c = image.pixelColor(x + dx, y + dy);
                matched = matched || (std::abs(c.red() - expected[0]) < 16 &&
                                      std::abs(c.green() - expected[1]) < 16 &&
                                      std::abs(c.blue() - expected[2]) < 16);
            }
        }
    }
    const bool saved = image.save(path);
    qInfo() << "Gizmo overlay" << static_cast<int>(layout.mode) << "visible" << layout.visible
            << "segments" << layout.segments.size() << "center" << layout.center.x
            << layout.center.y << "logical" << viewport.size() << "image" << image.size()
            << "matched" << matched;
    require(matched && saved, "GPU did not display the expected colored gizmo handle");
}

// 使用分帧事件序列，避免把一次连续拖拽压缩到同一轮事件循环。
void begin_rotation(State& state, VulkanViewport& viewport, const minirender::Transform& original) {
    viewport.set_gizmo_mode(Mode::rotate);
    const auto& layout = viewport.gizmo_layout();
    const auto forward = glm::normalize(glm::dvec3(viewport.camera().target) -
                                        glm::dvec3(viewport.camera().position));
    state.rotation_handle = Handle::x;
    for (const auto candidate : {Handle::y, Handle::z}) {
        if (std::abs(glm::dot(axis(candidate), forward)) >
            std::abs(glm::dot(axis(state.rotation_handle), forward))) {
            state.rotation_handle = candidate;
        }
    }
    state.rotation_axis = axis(state.rotation_handle);
    state.radius = layout.world_radius;
    const auto basis =
        state.rotation_handle == Handle::x ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const auto other = glm::cross(state.rotation_axis, basis);
    bool found = false;
    for (int i = 1; i < 96; ++i) {
        const auto angle = i * 2 * std::numbers::pi / 96;
        const auto radial = basis * std::cos(angle) + other * std::sin(angle);
        const auto point =
            project(viewport, glm::dvec3(original.translation) + state.radius * radial);
        if (layout.hit(point).handle == state.rotation_handle) {
            state.radial = radial;
            state.start = point;
            found = true;
            break;
        }
    }
    require(found, "Rotation ring has no selectable visible arc");
    mouse(state, viewport, QEvent::MouseButtonPress, state.start, Qt::LeftButton, Qt::LeftButton);
    require(viewport.gizmo_dragging(), "Rotation ring did not begin a gesture");
}

} // namespace

void start_gizmo_smoke(Workbench& window, const QString& directory, bool measure_responsiveness) {
    require(QDir().mkpath(directory), "Cannot create gizmo output directory");
    const auto state = std::make_shared<State>();
    state->source_scene = directory + "/initial.json";
    window.save_scene_file(state->source_scene);
    // 普通启动使用异步场景导入，先检查该路径的默认选择，再测试显式选中物体。
    window.open_scene_file(state->source_scene);
    auto* viewport = window.viewport();
    viewport->set_fxaa_enabled(false);
    auto* timer = new QTimer(&window);
    timer->setInterval(25);
    QObject::connect(
        timer, &QTimer::timeout, &window,
        [state, timer, &window, directory, measure_responsiveness] {
            try {
                auto& viewport = *window.viewport();
                require(++state->ticks < 3000 && viewport.failure().isEmpty(),
                        "Gizmo test timed out or renderer failed");
                if (!viewport.can_render()) {
                    return;
                }
                if (viewport.rendered_frames() < 5 || !viewport.isValid()) {
                    return;
                }
                const auto advance = [&](Phase phase) {
                    qInfo() << "Gizmo phase" << static_cast<int>(state->phase) << "->"
                            << static_cast<int>(phase);
                    state->phase = phase;
                    state->frame = viewport.rendered_frames();
                    state->step = 0;
                };
                const auto rendered = [&] {
                    return viewport.rendered_frames() > state->frame + 2 &&
                           viewport.preview_wait_ms() == 0;
                };
                const auto gpu = [&](const QString& label) {
                    viewport.set_gizmo_mode(Mode::select);
                    return verify_geometry_gpu(window, directory, label);
                };
                const auto step_move = [&] {
                    ++state->step;
                    mouse(*state, viewport, QEvent::MouseMove,
                          state->start + state->delta * (state->step / 8.0), Qt::NoButton,
                          Qt::LeftButton);
                    require(viewport.gizmo_dragging(),
                            "Gizmo gesture ended during its mouse moves");
                    require(window.document().camera.position == state->camera.position &&
                                window.document().camera.target == state->camera.target,
                            "Object dragging changed the camera");
                    return state->step >= 8;
                };
                switch (state->phase) {
                case Phase::startup: {
                    if (window.asset_loading() ||
                        window.ibl_controller()->status().state != IblTaskState::ready) {
                        return;
                    }
                    viewport.set_gizmo_mode(Mode::translate);
                    require(viewport.gizmo_layout().visible,
                            "Async scene import lost the default manipulation target");
                    verify_overlay(viewport, directory + "/default-selection.png", Handle::x);
                    window.select_object("blue_cube");
                    state->original = transform(window);
                    state->camera = window.document().camera;
                    state->baseline = gpu("initial");
                    const auto hdr_before = viewport.grab_hdr();
                    control<QAction>(window, "gizmo_translate")->setChecked(true);
                    require(viewport.gizmo_mode() == Mode::translate,
                            "Checked translation tool did not update the viewport mode");
                    verify_overlay(viewport, directory + "/translate-overlay.png", Handle::x);
                    const auto hdr_after = viewport.grab_hdr();
                    double maximum = 0;
                    for (std::size_t y = 0; y < hdr_before.height(); ++y) {
                        for (std::size_t x = 0; x < hdr_before.width(); ++x) {
                            maximum =
                                std::max(maximum, static_cast<double>(glm::length(
                                                      hdr_before.at(x, y) - hdr_after.at(x, y))));
                        }
                    }
                    require(maximum == 0, "Editor gizmo changed the linear HDR image");
                    capture_workbench_ui(window, directory + "/translate-ui.png");
                    state->uploads = viewport.upload_statistics().mesh_uploads;
                    state->descriptors = viewport.upload_statistics().descriptor_rebuilds;
                    begin_axis(*state, viewport);
                    advance(Phase::move);
                    return;
                }
                case Phase::move:
                    if (!step_move()) {
                        return;
                    }
                    mouse(*state, viewport, QEvent::MouseButtonRelease, state->start + state->delta,
                          Qt::LeftButton, Qt::NoButton);
                    state->moved = transform(window);
                    require(std::abs(state->moved.translation.x - state->original.translation.x) >
                                    0.1F &&
                                state->moved.translation.y == state->original.translation.y &&
                                state->moved.translation.z == state->original.translation.z,
                            "X handle failed to constrain world-space translation");
                    require(viewport.upload_statistics().mesh_uploads == state->uploads &&
                                viewport.upload_statistics().descriptor_rebuilds ==
                                    state->descriptors,
                            "Transform dragging rebuilt immutable GPU assets");
                    advance(Phase::moved);
                    return;
                case Phase::moved:
                    if (!rendered()) {
                        return;
                    }
                    state->moved_image = gpu("translated");
                    require(state->moved_image != state->baseline,
                            "Translation did not change GPU geometry");
                    require(std::abs(control<QDoubleSpinBox>(window, "translation.x")->value() -
                                     state->moved.translation.x) < 0.001,
                            "Translation inspector did not follow the gesture");
                    control<QAction>(window, "undo")->trigger();
                    require(equal(transform(window), state->original),
                            "One undo did not restore the whole translation drag");
                    require(!control<QAction>(window, "undo")->isEnabled(),
                            "A drag created multiple undo entries");
                    advance(Phase::undo_move);
                    return;
                case Phase::undo_move:
                    if (!rendered()) {
                        return;
                    }
                    require(gpu("translation-undo") == state->baseline,
                            "Translation undo did not restore GPU pixels");
                    control<QAction>(window, "redo")->trigger();
                    require(equal(transform(window), state->moved), "Translation redo failed");
                    advance(Phase::redo_move);
                    return;
                case Phase::redo_move:
                    if (!rendered()) {
                        return;
                    }
                    require(gpu("translation-redo") == state->moved_image,
                            "Translation redo did not restore GPU pixels");
                    control<QAction>(window, "undo")->trigger();
                    advance(Phase::restore_move);
                    return;
                case Phase::restore_move:
                    if (!rendered()) {
                        return;
                    }
                    viewport.set_gizmo_mode(Mode::translate);
                    state->start = viewport.gizmo_layout().center;
                    state->delta = {25, -30};
                    mouse(*state, viewport, QEvent::MouseButtonPress, state->start, Qt::LeftButton,
                          Qt::LeftButton);
                    require(viewport.highlighted_gizmo_handle() == Handle::screen,
                            "Center handle did not start screen-plane movement");
                    advance(Phase::cancel_move);
                    return;
                case Phase::cancel_move:
                    if (!step_move()) {
                        return;
                    }
                    {
                        const auto delta =
                            glm::dvec3(transform(window).translation - state->original.translation);
                        const auto forward = glm::normalize(glm::dvec3(state->camera.target) -
                                                            glm::dvec3(state->camera.position));
                        require(glm::length(delta) > 0.05 &&
                                    std::abs(glm::dot(delta, forward)) < 1e-5,
                                "Center handle did not constrain movement to the screen plane");
                        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                        QCoreApplication::sendEvent(&viewport, &escape);
                    }
                    require(!viewport.gizmo_dragging() && equal(transform(window), state->original),
                            "Escape did not cancel the drag");
                    mouse(*state, viewport, QEvent::MouseButtonRelease, state->start + state->delta,
                          Qt::LeftButton, Qt::NoButton);
                    require(!control<QAction>(window, "undo")->isEnabled(),
                            "Cancelled drag added undo history");
                    advance(Phase::cancelled);
                    return;
                case Phase::cancelled:
                    if (!rendered()) {
                        return;
                    }
                    require(gpu("cancelled") == state->baseline,
                            "Cancelled movement did not restore GPU pixels");
                    control<QAction>(window, "gizmo_rotate")->trigger();
                    advance(Phase::rotate_start);
                    return;
                case Phase::rotate_start:
                    if (!rendered()) {
                        return;
                    }
                    begin_rotation(*state, viewport, state->original);
                    // 截图前退出高亮状态只会改变显示；实际拖拽在下一阶段开始前重建。
                    viewport.finish_gizmo_drag(false);
                    viewport.set_gizmo_mode(Mode::rotate);
                    verify_overlay(viewport, directory + "/rotate-overlay.png",
                                   state->rotation_handle);
                    capture_workbench_ui(window, directory + "/rotate-ui.png");
                    begin_rotation(*state, viewport, state->original);
                    advance(Phase::rotate);
                    return;
                case Phase::rotate: {
                    ++state->step;
                    const double angle = state->step * std::numbers::pi / (3 * 8);
                    const auto radial =
                        glm::dvec3(glm::rotate(glm::dmat4(1), angle, state->rotation_axis) *
                                   glm::dvec4(state->radial, 0));
                    const auto pointer = project(viewport, glm::dvec3(state->original.translation) +
                                                               state->radius * radial);
                    mouse(*state, viewport, QEvent::MouseMove, pointer, Qt::NoButton,
                          Qt::LeftButton);
                    require(viewport.gizmo_dragging(), "Rotation drag lost capture");
                    if (state->step < 8) {
                        return;
                    }
                    mouse(*state, viewport, QEvent::MouseButtonRelease, pointer, Qt::LeftButton,
                          Qt::NoButton);
                    state->rotated = transform(window);
                    const auto pivot = glm::dvec3(state->original.translation);
                    const auto expected =
                        glm::translate(glm::dmat4(1), pivot) *
                        glm::rotate(glm::dmat4(1), std::numbers::pi / 3, state->rotation_axis) *
                        glm::translate(glm::dmat4(1), -pivot) *
                        glm::dmat4(state->original.matrix());
                    require(close(state->rotated.matrix(), expected),
                            "Rotation ring did not compose the expected world rotation");
                    require(state->rotated.translation == state->original.translation &&
                                state->rotated.scale == state->original.scale,
                            "Rotation drag changed position or scale");
                    advance(Phase::rotated);
                    return;
                }
                case Phase::rotated:
                    if (!rendered()) {
                        return;
                    }
                    state->rotated_image = gpu("rotated");
                    require(state->rotated_image != state->baseline,
                            "Rotation did not change GPU normals or depth");
                    control<QAction>(window, "undo")->trigger();
                    require(equal(transform(window), state->original),
                            "One undo did not restore the entire rotation");
                    advance(Phase::undo_rotate);
                    return;
                case Phase::undo_rotate:
                    if (!rendered()) {
                        return;
                    }
                    require(gpu("rotation-undo") == state->baseline,
                            "Rotation undo did not restore GPU pixels");
                    control<QAction>(window, "redo")->trigger();
                    require(equal(transform(window), state->rotated), "Rotation redo failed");
                    advance(Phase::redo_rotate);
                    return;
                case Phase::redo_rotate:
                    if (!rendered()) {
                        return;
                    }
                    require(gpu("rotation-redo") == state->rotated_image,
                            "Rotation redo did not restore GPU pixels");
                    advance(Phase::ungrab_start);
                    return;
                case Phase::ungrab_start:
                    state->gesture_start = transform(window);
                    begin_axis(*state, viewport);
                    advance(Phase::ungrab_move);
                    return;
                case Phase::ungrab_move:
                    if (!step_move()) {
                        return;
                    }
                    {
                        QEvent ungrab(QEvent::UngrabMouse);
                        QCoreApplication::sendEvent(&viewport, &ungrab);
                    }
                    require(!viewport.gizmo_dragging() &&
                                !equal(transform(window), state->gesture_start),
                            "Mouse capture release did not finish the object gesture");
                    mouse(*state, viewport, QEvent::MouseMove, state->start + state->delta * 2.0,
                          Qt::NoButton, Qt::NoButton);
                    control<QAction>(window, "undo")->trigger();
                    require(equal(transform(window), state->gesture_start),
                            "Capture-release gesture did not form one undo entry");
                    advance(Phase::ungrab_finish);
                    return;
                case Phase::ungrab_finish:
                    if (!rendered()) {
                        return;
                    }
                    advance(Phase::hide_start);
                    return;
                case Phase::hide_start:
                    begin_axis(*state, viewport);
                    advance(Phase::hide_move);
                    return;
                case Phase::hide_move:
                    if (!step_move()) {
                        return;
                    }
                    window.hide();
                    require(!viewport.gizmo_dragging() &&
                                equal(transform(window), state->gesture_start),
                            "Hiding the window did not cancel an active object drag");
                    window.show();
                    advance(Phase::hide_finish);
                    return;
                case Phase::hide_finish:
                    if (!rendered()) {
                        return;
                    }
                    window.save_scene_async(directory + "/操纵 场景.json");
                    advance(Phase::save);
                    return;
                case Phase::save:
                    if (window.asset_loading()) {
                        return;
                    }
                    window.load_scene_file(directory + "/操纵 场景.json");
                    window.select_object("blue_cube");
                    require(equal(transform(window), state->rotated),
                            "Scene save lost the manipulated transform");
                    advance(Phase::reopen);
                    return;
                case Phase::reopen:
                    if (!rendered() ||
                        window.ibl_controller()->status().state != IblTaskState::ready) {
                        return;
                    }
                    require(gpu("reopened") == state->rotated_image,
                            "Reopened transform changed GPU pixels");
                    window.offline_panel()->set_settings(
                        {.width = 8, .height = 6, .samples_per_pixel = 4}, {}, {1, 4, 2});
                    control<QPushButton>(window, "offline_start")->click();
                    advance(Phase::offline);
                    return;
                case Phase::offline:
                    if (window.offline_panel()->session().progress().state !=
                        minirender::RenderState::completed) {
                        return;
                    }
                    require(viewport.offline_visible(), "Offline fixture is not visible");
                    viewport.set_gizmo_mode(Mode::translate);
                    require(!viewport.gizmo_layout().visible,
                            "Offline output displayed an editor gizmo");
                    mouse(*state, viewport, QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton,
                          Qt::LeftButton);
                    mouse(*state, viewport, QEvent::MouseMove, {150, 150}, Qt::NoButton,
                          Qt::LeftButton);
                    mouse(*state, viewport, QEvent::MouseButtonRelease, {150, 150}, Qt::LeftButton,
                          Qt::NoButton);
                    require(equal(transform(window), state->rotated),
                            "Offline display accepted an object drag");
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    require(viewport.gizmo_layout().visible,
                            "Returning to real time did not restore the gizmo");
                    window.resize(1080, 760);
                    advance(Phase::finish);
                    return;
                case Phase::finish:
                    if (!rendered()) {
                        return;
                    }
                    begin_axis(*state, viewport);
                    require(viewport.gizmo_dragging(),
                            "A new gesture could not start after capture recovery and resize");
                    mouse(*state, viewport, QEvent::MouseMove, state->start + state->delta * 0.5,
                          Qt::NoButton, Qt::LeftButton);
                    require(!equal(transform(window), state->rotated),
                            "Shutdown fixture requires an active transform preview");
                    if (measure_responsiveness) {
                        require(state->maximum_callback_ms < 500,
                                "Object dragging blocked the UI for 500 milliseconds");
                    }
                    qInfo() << "Gizmo smoke PASS: axes, screen plane, rotation, CPU/GPU, HDR "
                               "isolation, one-step history, cancel, capture, save and offline; "
                               "maximum callback ms"
                            << state->maximum_callback_ms;
                    timer->stop();
                    QApplication::exit(0); // 析构时仍有手势，检查回调与编辑器的销毁顺序。
                    return;
                }
            } catch (const std::exception& error) {
                qCritical() << "Gizmo smoke FAIL at phase" << static_cast<int>(state->phase) << ':'
                            << error.what();
                timer->stop();
                QApplication::exit(1);
            }
        });
    timer->start();
}
