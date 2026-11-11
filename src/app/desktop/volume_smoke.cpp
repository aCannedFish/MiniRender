#include "volume_smoke.hpp"
#include "geometry_smoke.hpp"
#include "offline_panel.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
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
    require(result, "Volume regression control is missing");
    return result;
}
void click(Workbench& window, const char* name) {
    auto* button = control<QPushButton>(window, name);
    require(button->isEnabled(), "Volume regression button is disabled");
    button->click();
}
void set(Workbench& window, const char* name, double number) {
    control<QDoubleSpinBox>(window, name)->setValue(number);
}
void action(Workbench& window, const char* name) {
    auto* value = control<QAction>(window, name);
    require(value->isEnabled(), "Volume history or import action is disabled");
    value->trigger();
}
const MediumRegion& region(Workbench& window) {
    require(!window.document().media.empty(), "Volume scene has no medium");
    return window.document().media[0];
}

// CPU 完整图像是独立的串行采样参考，任务线程、参数草稿和快照变更均不参与它。
void verify_result(Workbench& window, const std::shared_ptr<const SceneSnapshot>& snapshot,
                   RenderSettings settings, IntegratorSettings integrator) {
    const auto expected = PathTracer(snapshot, integrator).render(settings);
    const auto actual = window.offline_panel()->session().result();
    require(actual && actual->width() == settings.width && actual->height() == settings.height,
            "Volume render result dimensions differ");
    double largest = 0;
    for (std::size_t y = 0; y < settings.height; ++y) {
        for (std::size_t x = 0; x < settings.width; ++x) {
            largest = std::max(largest, double(glm::length(actual->at(x, y) - expected.at(x, y))));
        }
    }
    qInfo() << "Volume serial RGB maximum" << largest;
    require(largest < 1e-6, "Volume task differs from the frozen serial reference");
    require(window.offline_panel()->session().progress().statistics.medium_scattering_events > 0,
            "Volume UI task recorded no medium scattering events");
}

// 取源像素映射后的屏幕中心，验证离线图的实际曝光、Reinhard 与 sRGB 编码。
void verify_display(Workbench& window, const QString& directory, const QString& label) {
    auto* viewport = window.viewport();
    require(viewport->offline_visible(), "Volume image is not selected for display");
    const auto actual = viewport->grab();
    const auto linear = window.offline_panel()->session().result();
    require(linear && !actual.isNull(), "Volume GPU display is missing");
    const auto expected = display_image(*linear, viewport->exposure());
    const double scale = std::min(double(actual.width()) / double(linear->width()),
                                  double(actual.height()) / double(linear->height()));
    const int width = int(std::floor(double(linear->width()) * scale));
    const int height = int(std::floor(double(linear->height()) * scale));
    const int left = (actual.width() - width) / 2, top = (actual.height() - height) / 2;
    int probes = 0;
    for (std::size_t y = 0; y < linear->height(); ++y) {
        for (std::size_t x = 0; x < linear->width(); ++x) {
            const auto pixel =
                actual.pixelColor(left + int((double(x) + .5) * width / double(linear->width())),
                                  top + int((double(y) + .5) * height / double(linear->height())));
            const std::array channels{pixel.red(), pixel.green(), pixel.blue()};
            for (int c = 0; c < 3; ++c) {
                const auto encoded = encode_unorm8(linear_to_srgb(expected.at(x, y)[c]));
                require(std::abs(channels[std::size_t(c)] - encoded) <= 2,
                        "Volume Vulkan display differs from CPU encoding");
            }
            ++probes;
        }
    }
    require(actual.save(directory + '/' + label + ".png"), "Cannot save volume display capture");
    qInfo() << "Volume GPU display" << label << probes;
}

void verify_layout(Workbench& window, const QString& directory) {
    auto* tabs = control<QTabWidget>(window, "inspector_tabs");
    tabs->setCurrentIndex(10);
    auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
    auto* panel = control<QWidget>(window, "volume_panel");
    require(area, "Volume page is not scrollable");
    for (const auto size : {QSize(1200, 800), QSize(960, 700)}) {
        window.resize(size);
        window.layout()->activate();
        if (auto* layout = area->layout()) {
            layout->activate();
        }
        panel->layout()->activate();
        area->verticalScrollBar()->setValue(0);
        require(panel->width() <= area->viewport()->width(),
                "Volume page needs horizontal scrolling");
        std::vector<QRect> rectangles;
        for (auto* field : panel->findChildren<QWidget*>()) {
            if (!field->objectName().startsWith("volume_") || !field->isVisibleTo(panel) ||
                field->height() == 0) {
                continue;
            }
            const QRect rectangle(field->mapTo(panel, QPoint()), field->size());
            require(rectangle.left() >= 0 && rectangle.right() < panel->width(),
                    "Volume field exceeds page bounds");
            for (const auto& previous : rectangles) {
                const auto overlap = previous.intersected(rectangle);
                require(overlap.width() <= 1 || overlap.height() <= 1, "Volume controls overlap");
            }
            rectangles.push_back(rectangle);
        }
        require(rectangles.size() >= 22, "Volume layout omitted fields");
        capture_workbench_ui(window,
                             directory + QString("/volume-layout-%1.png").arg(size.width()));
        auto* bottom = control<QPushButton>(window, "volume_apply");
        area->ensureWidgetVisible(bottom);
        require(area->viewport()->rect().contains(
                    bottom->mapTo(area->viewport(), bottom->rect().center())),
                "Volume apply button is inaccessible");
        capture_workbench_ui(window,
                             directory + QString("/volume-bottom-%1.png").arg(size.width()));
        qInfo() << "Volume layout" << size << rectangles.size();
    }
}

enum class Stage {
    create,
    draft,
    history,
    multiple,
    overlap,
    disabled,
    precision_ready,
    unrelated,
    saved,
    reopened,
    old_start,
    old_done,
    old_verify,
    new_done,
    display,
    exposure,
    layout,
    stress,
    template_load,
    template_ready,
    cancel_start,
    cancelling,
    cancelled,
    exit_start,
    exit_running
};
struct State {
    Stage stage = Stage::create;
    int ticks = 0, frame = 0, settle = 0, edits = 0;
    std::vector<MediumRegion> before;
    std::shared_ptr<const SceneSnapshot> snapshot;
    SceneUploadStatistics uploads;
    RenderSettings render{32, 32, 256, 101};
    IntegratorSettings integrator{SamplingMode::mis, 6, true, 3};
    std::uint64_t job = 0, samples = 0;
    QElapsedTimer cancellation;
    UiResponseProbe* probe = nullptr;
};
} // namespace

void start_volume_smoke(Workbench& window, const QString& directory, bool enforce_response) {
    auto state = std::make_shared<State>();
    state->probe = new UiResponseProbe(&window, "Volume");
    auto* timer = new QTimer(&window);
    timer->setInterval(50);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            try {
                auto* viewport = window.viewport();
                auto* offline = window.offline_panel();
                require(++state->ticks < 3000 && viewport->failure().isEmpty(),
                        "Volume smoke timed out");
                if (!viewport->can_render() || viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto next = [&] {
                    state->stage = static_cast<Stage>(static_cast<int>(state->stage) + 1);
                    state->frame = viewport->rendered_frames();
                    state->settle = 0;
                    qInfo() << "Volume stage" << static_cast<int>(state->stage);
                };
                const auto completed = [&] {
                    const auto p = offline->session().progress();
                    require(p.state != RenderState::failed, p.error.c_str());
                    return p.state == RenderState::completed;
                };
                switch (state->stage) {
                case Stage::create:
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(10);
                    require(window.document().media.empty(),
                            "Volume fixture must initially have no media");
                    click(window, "volume_add");
                    require(window.document().media.size() == 1, "Add did not create a medium");
                    set(window, "volume_g", .1);
                    set(window, "volume_g", 0);
                    require(!control<QPushButton>(window, "volume_apply")->isEnabled() &&
                                control<QPushButton>(window, "volume_render")->isEnabled(),
                            "Returning a draft to its source leaves a false pending edit");
                    state->before = window.document().media;
                    set(window, "volume_absorption.0", .123456789);
                    set(window, "volume_scattering.2", .42);
                    set(window, "volume_g", .35);
                    control<QLineEdit>(window, "volume_name")->setText("测试雾");
                    set(window, "volume_min.0", 2);
                    require(!control<QPushButton>(window, "volume_render")->isEnabled(),
                            "Unapplied draft can silently render old parameters");
                    click(window, "volume_apply");
                    require(window.document().media == state->before,
                            "Invalid box changed the scene");
                    require(!control<QLabel>(window, "volume_notice")->text().isEmpty(),
                            "Invalid box has no inline diagnostic");
                    next();
                    return;
                case Stage::draft:
                    set(window, "volume_min.0", -1);
                    click(window, "volume_apply");
                    require(region(window).name == "测试雾" &&
                                region(window).medium.absorption.x == .123456789 &&
                                region(window).medium.scattering.z == .42 &&
                                region(window).medium.anisotropy == .35,
                            "Atomic medium apply lost fields");
                    action(window, "undo");
                    require(window.document().media == state->before,
                            "Medium apply was not one undo");
                    action(window, "redo");
                    require(region(window).medium.anisotropy == .35, "Medium redo lost parameters");
                    next();
                    return;
                case Stage::history:
                    state->before = window.document().media;
                    click(window, "volume_remove");
                    require(window.document().media.empty(), "Medium delete failed");
                    action(window, "undo");
                    require(window.document().media == state->before, "Medium delete undo failed");
                    action(window, "redo");
                    click(window, "volume_add");
                    click(window, "volume_add");
                    require(window.document().media.size() == 2,
                            "Multiple regions were not created");
                    next();
                    return;
                case Stage::multiple:
                    control<QComboBox>(window, "volume_region")->setCurrentIndex(0);
                    state->before = window.document().media;
                    set(window, "volume_max.0", 2);
                    click(window, "volume_apply");
                    require(window.document().media == state->before, "Overlap changed the scene");
                    control<QComboBox>(window, "volume_region")->setCurrentIndex(1);
                    control<QCheckBox>(window, "volume_enabled")->setChecked(false);
                    set(window, "volume_min.0", 0);
                    set(window, "volume_max.0", 1);
                    click(window, "volume_apply");
                    require(!window.document().media[1].enabled, "Disabled overlap was not stored");
                    next();
                    return;
                case Stage::overlap:
                    state->before = window.document().media;
                    control<QCheckBox>(window, "volume_enabled")->setChecked(true);
                    click(window, "volume_apply");
                    require(window.document().media == state->before,
                            "Enabled overlap changed the scene");
                    set(window, "volume_min.0", 1.25);
                    set(window, "volume_max.0", 3.25);
                    click(window, "volume_apply");
                    require(window.document().media[1].enabled, "Valid re-enable failed");
                    next();
                    return;
                case Stage::disabled:
                    state->probe->verify([&] {
                        auto scene = window.document();
                        scene.media.resize(1);
                        scene.media[0].minimum.x = -.9987654321098765;
                        scene.media[0].medium.absorption.x = .14285714285714285;
                        scene.media[0].medium.anisotropy = .12345678901234567;
                        state->before = scene.media;
                        save_scene(scene, std::filesystem::path(
                                              (directory + "/精度 准备.json").toStdU16String()));
                    });
                    window.open_scene_file(directory + "/精度 准备.json");
                    next();
                    return;
                case Stage::precision_ready:
                    if (window.asset_loading()) {
                        return;
                    }
                    require(window.document().media == state->before,
                            "Precise medium load changed doubles");
                    set(window, "volume_scattering.2", .81);
                    window.select_object("blue_cube");
                    set(window, "base_color.x", .4);
                    set(window, "fov", 48);
                    require(control<QDoubleSpinBox>(window, "volume_scattering.2")->value() ==
                                    .81 &&
                                control<QPushButton>(window, "volume_apply")->isEnabled(),
                            "Unrelated scene edit discarded the medium draft");
                    next();
                    return;
                case Stage::unrelated:
                    click(window, "volume_apply");
                    require(
                        region(window).medium.anisotropy == state->before[0].medium.anisotropy &&
                            region(window).minimum == state->before[0].minimum &&
                            region(window).medium.absorption == state->before[0].medium.absorption,
                        "Applying one scalar rounded untouched medium doubles");
                    window.save_scene_async(directory + "/介质 编辑.json");
                    next();
                    return;
                case Stage::saved:
                    if (window.asset_loading()) {
                        return;
                    }
                    set(window, "volume_g", -.4);
                    window.open_scene_file(directory + "/介质 编辑.json");
                    next();
                    return;
                case Stage::reopened:
                    if (window.asset_loading()) {
                        return;
                    }
                    require(region(window).medium.anisotropy ==
                                    state->before[0].medium.anisotropy &&
                                region(window).medium.scattering.z == .81 &&
                                !control<QPushButton>(window, "volume_apply")->isEnabled() &&
                                control<QDoubleSpinBox>(window, "volume_g")->value() != -.4,
                            "Reopening equal media retained the previous document draft");
                    next();
                    return;
                case Stage::old_start:
                    state->snapshot = make_snapshot(window.document());
                    offline->set_settings(state->render, state->integrator, {2, 8, 4});
                    click(window, "volume_render");
                    require(!render_terminal(offline->session().progress().state),
                            "Volume button did not start a live render");
                    state->job = offline->session().progress().job_id;
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(10);
                    set(window, "volume_g", .55);
                    set(window, "volume_scattering.2", .32);
                    click(window, "volume_apply");
                    require(offline->stale(), "Medium edit did not stale the active snapshot");
                    click(window, "volume_render");
                    require(offline->session().progress().job_id == state->job,
                            "Viewing an active task unexpectedly restarted it");
                    next();
                    return;
                case Stage::old_done:
                    if (!completed()) {
                        return;
                    }
                    next();
                    return;
                case Stage::old_verify:
                    if (++state->settle < 5) {
                        return;
                    }
                    state->probe->verify([&] {
                        verify_result(window, state->snapshot, state->render, state->integrator);
                        verify_display(window, directory, "volume-old-snapshot");
                    });
                    state->snapshot = make_snapshot(window.document());
                    state->render = {16, 16, 32, 101};
                    offline->set_settings(state->render, state->integrator, {2, 8, 4});
                    click(window, "volume_render");
                    require(!offline->stale(), "New volume render retained old stale flag");
                    next();
                    return;
                case Stage::new_done:
                    if (!completed()) {
                        return;
                    }
                    next();
                    return;
                case Stage::display:
                    if (++state->settle < 5) {
                        return;
                    }
                    state->probe->verify([&] {
                        verify_result(window, state->snapshot, state->render, state->integrator);
                        verify_display(window, directory, "volume-new-snapshot");
                    });
                    state->job = offline->session().progress().job_id;
                    state->samples = offline->session().progress().recorded_samples;
                    set(window, "offline_exposure", 2);
                    next();
                    return;
                case Stage::exposure:
                    require(offline->session().progress().job_id == state->job &&
                                offline->session().progress().recorded_samples == state->samples,
                            "Exposure restarted volume tracing");
                    state->probe->verify(
                        [&] { verify_display(window, directory, "volume-exposure"); });
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    set(window, "offline_exposure", 0);
                    next();
                    return;
                case Stage::layout:
                    state->probe->verify([&] { verify_layout(window, directory); });
                    state->uploads = viewport->upload_statistics();
                    next();
                    return;
                case Stage::stress:
                    set(window, "volume_absorption.1", state->edits % 2 == 0 ? .17 : .21);
                    click(window, "volume_apply");
                    if (++state->edits < 12) {
                        return;
                    }
                    next();
                    return;
                case Stage::template_load:
                    require(viewport->upload_statistics().mesh_uploads ==
                                    state->uploads.mesh_uploads &&
                                viewport->upload_statistics().descriptor_rebuilds ==
                                    state->uploads.descriptor_rebuilds,
                            "Media edits rebuilt mesh or descriptor data");
                    action(window, "builtin_bounded_fog");
                    require(window.asset_loading() &&
                                !control<QPushButton>(window, "volume_add")->isEnabled(),
                            "Volume import is not asynchronous or leaves edits enabled");
                    next();
                    return;
                case Stage::template_ready:
                    if (window.asset_loading() ||
                        !render_terminal(offline->session().progress().state)) {
                        return;
                    }
                    require(window.document().objects.size() == 3 &&
                                region(window).medium.anisotropy == .6,
                            "Bounded fog quick import lost the template");
                    require(!viewport->offline_visible(),
                            "Imported medium was hidden behind an old image");
                    state->probe->verify([&] {
                        verify_geometry_gpu(window, directory, "volume-preview", "blue_cube");
                    });
                    control<QComboBox>(window, "volume_demo_kind")->setCurrentIndex(1);
                    click(window, "volume_demo");
                    next();
                    return;
                case Stage::cancel_start:
                    if (window.asset_loading() ||
                        !render_terminal(offline->session().progress().state)) {
                        return;
                    }
                    require(window.document().objects.size() == 8 &&
                                region(window).medium.anisotropy == .25,
                            "Volume page did not import Cornell fog");
                    offline->set_settings({64, 64, 1000000, 101}, state->integrator, {2, 8, 4});
                    click(window, "volume_render");
                    next();
                    return;
                case Stage::cancelling:
                    if (!offline->session().progress().recorded_samples ||
                        !control<QPushButton>(window, "offline_cancel")->isEnabled()) {
                        return;
                    }
                    require(control<QProgressBar>(window, "offline_progress")->isVisible(),
                            "Live volume task has no progress display");
                    state->cancellation.start();
                    click(window, "offline_cancel");
                    next();
                    return;
                case Stage::cancelled:
                    if (!render_terminal(offline->session().progress().state)) {
                        return;
                    }
                    require(offline->session().progress().state == RenderState::cancelled,
                            "Volume cancel did not terminate the task");
                    qInfo() << "Volume cancellation ms" << state->cancellation.elapsed();
                    require(state->cancellation.elapsed() < 2000,
                            "Volume cancellation blocked too long");
                    next();
                    return;
                case Stage::exit_start:
                    if (!control<QPushButton>(window, "offline_start")->isEnabled()) {
                        return;
                    }
                    click(window, "volume_render");
                    next();
                    return;
                case Stage::exit_running:
                    if (!offline->session().progress().recorded_samples) {
                        return;
                    }
                    state->probe->report(enforce_response);
                    timer->stop();
                    qInfo()
                        << "Volume smoke PASS: drafts, regions, precision, history, async IO, "
                           "snapshots, CPU/GPU display, presets, response, cancellation and exit";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Volume smoke FAIL stage" << static_cast<int>(state->stage) << ':'
                            << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
