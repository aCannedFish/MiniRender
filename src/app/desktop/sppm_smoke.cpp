#include "sppm_smoke.hpp"
#include "builtin_scenes.hpp"
#include "geometry_smoke.hpp"
#include "offline_panel.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
using namespace minirender;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* value = window.findChild<T*>(name);
    require(value, "SPPM regression control is missing");
    return value;
}
void click(Workbench& window, const char* name) {
    auto* value = control<QPushButton>(window, name);
    if (!value->isEnabled()) {
        throw std::runtime_error(std::string("SPPM regression button is disabled: ") + name);
    }
    value->click();
}
void action(Workbench& window, const char* name) {
    auto* value = control<QAction>(window, name);
    require(value->isEnabled(), "SPPM import action is disabled");
    value->trigger();
}

// 独立积分器重新计算启动快照，检查 UI 参数、暂停重试和后台发布的完整结果。
void verify_result(Workbench& window, std::shared_ptr<const SceneSnapshot> snapshot,
                   RenderSettings render, IntegratorSettings integrator) {
    SppmStatistics statistics;
    const auto expected = [&] {
        if (integrator.kind == IntegratorKind::path_tracing) {
            return PathTracer(snapshot, integrator).render(render);
        }
        SppmIntegrator reference(snapshot, render, integrator);
        for (std::uint32_t i = 0; i < render.samples_per_pixel; ++i) {
            reference.advance();
        }
        statistics = reference.statistics();
        return reference.image();
    }();
    const auto actual = window.offline_panel()->session().result();
    require(actual && actual->width() == render.width && actual->height() == render.height,
            "SPPM result dimensions differ");
    double largest = 0;
    for (std::size_t y = 0; y < render.height; ++y) {
        for (std::size_t x = 0; x < render.width; ++x) {
            largest = std::max(largest, double(glm::length(actual->at(x, y) - expected.at(x, y))));
        }
    }
    const auto p = window.offline_panel()->session().progress();
    require(p.integrator == integrator.kind && largest < 1e-6,
            "Offline backend or parameters differ from the serial reference");
    if (integrator.kind == IntegratorKind::sppm) {
        require(p.sppm.iterations == render.samples_per_pixel &&
                    p.sppm.emitted_photons == statistics.emitted_photons &&
                    p.sppm.gathered_contributions == statistics.gathered_contributions &&
                    std::abs(p.sppm.mean_radius - statistics.mean_radius) < 1e-15,
                "SPPM committed statistics differ from the reference");
    }
    qInfo() << "SPPM serial RGB maximum" << largest << "photons" << p.sppm.emitted_photons;
}

// 对照每个源像素的显示变换，覆盖新图像上传和仅修改曝光时的 Vulkan 路径。
void verify_display(Workbench& window, const QString& directory, const QString& label) {
    auto* viewport = window.viewport();
    const auto actual = viewport->grab();
    const auto image = window.offline_panel()->session().result();
    require(image && !actual.isNull() && viewport->offline_visible(), "SPPM GPU image is missing");
    const auto expected = display_image(*image, viewport->exposure());
    const double scale = std::min(double(actual.width()) / double(image->width()),
                                  double(actual.height()) / double(image->height()));
    const int width = int(std::floor(double(image->width()) * scale));
    const int height = int(std::floor(double(image->height()) * scale));
    const int left = (actual.width() - width) / 2, top = (actual.height() - height) / 2;
    for (std::size_t y = 0; y < image->height(); ++y) {
        for (std::size_t x = 0; x < image->width(); ++x) {
            const auto pixel =
                actual.pixelColor(left + int((double(x) + .5) * width / double(image->width())),
                                  top + int((double(y) + .5) * height / double(image->height())));
            const std::array channels{pixel.red(), pixel.green(), pixel.blue()};
            for (int c = 0; c < 3; ++c) {
                const auto wanted = encode_unorm8(linear_to_srgb(expected.at(x, y)[c]));
                require(std::abs(channels[std::size_t(c)] - wanted) <= 2,
                        "SPPM Vulkan display differs from CPU encoding");
            }
        }
    }
    require(actual.save(directory + '/' + label + ".png"), "Cannot save SPPM display");
    qInfo() << "SPPM GPU display" << label << image->width() * image->height();
}

void verify_layout(Workbench& window, const QString& directory) {
    auto* tabs = control<QTabWidget>(window, "inspector_tabs");
    tabs->setCurrentIndex(3);
    auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
    auto* panel = window.offline_panel();
    require(area, "Offline page is not scrollable");
    for (const auto size : {QSize(1200, 800), QSize(960, 700)}) {
        window.resize(size);
        window.layout()->activate();
        if (auto* layout = area->layout()) {
            layout->activate();
        }
        panel->layout()->activate();
        area->verticalScrollBar()->setValue(0);
        require(panel->width() <= area->viewport()->width(),
                "SPPM page requires horizontal scrolling");
        std::vector<QRect> rectangles;
        for (auto* field : panel->findChildren<QWidget*>()) {
            if (!field->objectName().startsWith("offline_") || !field->isVisibleTo(panel) ||
                field->height() == 0) {
                continue;
            }
            const QRect rectangle(field->mapTo(panel, QPoint()), field->size());
            require(rectangle.left() >= 0 && rectangle.right() < panel->width(),
                    "SPPM field exceeds page bounds");
            for (const auto& previous : rectangles) {
                const auto overlap = previous.intersected(rectangle);
                require(overlap.width() <= 1 || overlap.height() <= 1, "SPPM controls overlap");
            }
            rectangles.push_back(rectangle);
        }
        require(rectangles.size() >= 24, "SPPM layout omitted fields");
        capture_workbench_ui(window, directory + QString("/layout-%1.png").arg(size.width()));
        auto* bottom = control<QProgressBar>(window, "offline_phase_progress");
        area->ensureWidgetVisible(bottom);
        require(area->viewport()->rect().contains(
                    bottom->mapTo(area->viewport(), bottom->rect().center())),
                "SPPM phase progress cannot be reached");
        capture_workbench_ui(window, directory + QString("/bottom-%1.png").arg(size.width()));
        qInfo() << "SPPM layout" << size << rectangles.size();
    }
}

enum class Stage {
    import,
    ready,
    running,
    paused,
    resumed,
    old_result,
    new_result,
    display,
    exposure,
    exported,
    pt_result,
    aperture,
    medium_ready,
    mirror_ready,
    budget,
    cancelling,
    cancelled,
    exit_running
};

// 每个步骤通过事件循环等待真实任务，参考计算和截图单独排除出响应测量。
struct State {
    Stage stage = Stage::import;
    int ticks = 0, frame = 0, settle = 0;
    RenderSettings render{24, 24, 64, 101};
    IntegratorSettings integrator{};
    std::shared_ptr<const SceneSnapshot> snapshot;
    std::shared_ptr<const Image> paused_image;
    std::uint64_t job = 0, samples = 0;
    QElapsedTimer cancellation;
    UiResponseProbe* probe = nullptr;
};
} // namespace

void start_sppm_smoke(Workbench& window, const QString& directory, bool enforce_response) {
    require(QDir().mkpath(directory), "Cannot create SPPM test directory");
    auto state = std::make_shared<State>();
    state->probe = new UiResponseProbe(&window, "SPPM");
    auto* timer = new QTimer(&window);
    timer->setInterval(50);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            try {
                auto* viewport = window.viewport();
                auto* panel = window.offline_panel();
                const auto p = panel->session().progress();
                require(++state->ticks < 4000 && viewport->failure().isEmpty(),
                        "SPPM smoke timed out");
                if (!viewport->can_render() || viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto next = [&] {
                    state->stage = static_cast<Stage>(int(state->stage) + 1);
                    state->frame = viewport->rendered_frames();
                    state->settle = 0;
                    qInfo() << "SPPM stage" << int(state->stage);
                };
                const auto completed = [&] {
                    require(p.state != RenderState::failed, p.error.c_str());
                    return p.state == RenderState::completed;
                };
                switch (state->stage) {
                case Stage::import:
                    action(window, "builtin_glass_caustic");
                    require(window.asset_loading(), "Caustic import did not run in the background");
                    next();
                    return;
                case Stage::ready: {
                    if (window.asset_loading() ||
                        control<QComboBox>(window, "offline_backend")->currentIndex() != 1) {
                        return;
                    }
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(3);
                    require(control<QSpinBox>(window, "offline_photons")->value() == 40000 &&
                                control<QDoubleSpinBox>(window, "offline_radius")->value() == .12 &&
                                !control<QComboBox>(window, "offline_mode")->isEnabled(),
                            "SPPM preset settings were lost");
                    state->integrator.kind = IntegratorKind::sppm;
                    state->integrator.sppm = {9000, .120000000000001, .7000000000000002,
                                              64ULL * 1024 * 1024 + 17};
                    panel->set_settings(state->render, state->integrator, {.threads = 2});
                    // 修改一个字段并来回切换后端，其余高精度字段及非整 MiB 预算应保持原值。
                    control<QSpinBox>(window, "offline_photons")->setValue(10000);
                    state->integrator.sppm.photons_per_iteration = 10000;
                    control<QComboBox>(window, "offline_backend")->setCurrentIndex(0);
                    require(control<QComboBox>(window, "offline_mode")->isEnabled(),
                            "PT sampling control stayed disabled");
                    control<QComboBox>(window, "offline_backend")->setCurrentIndex(1);
                    const auto retained = panel->integrator_settings().sppm;
                    require(retained.initial_radius == state->integrator.sppm.initial_radius &&
                                retained.alpha == state->integrator.sppm.alpha &&
                                retained.memory_budget_bytes ==
                                    state->integrator.sppm.memory_budget_bytes,
                            "SPPM controls rounded untouched imported settings");
                    state->snapshot = make_snapshot(window.document());
                    click(window, "offline_start");
                    state->job = panel->session().progress().job_id;
                    next();
                    return;
                }
                case Stage::running:
                    require(p.state != RenderState::failed, p.error.c_str());
                    if (p.state != RenderState::running || p.sppm.iterations < 2 ||
                        !control<QPushButton>(window, "offline_pause")->isEnabled()) {
                        return;
                    }
                    require(p.integrator == IntegratorKind::sppm &&
                                !control<QComboBox>(window, "offline_backend")->isEnabled() &&
                                !control<QSpinBox>(window, "offline_photons")->isEnabled() &&
                                control<QProgressBar>(window, "offline_phase_progress")
                                    ->isVisibleTo(panel),
                            "Active SPPM task did not freeze settings or show phase progress");
                    click(window, "offline_pause");
                    next();
                    return;
                case Stage::paused:
                    if (p.state != RenderState::paused) {
                        return;
                    }
                    if (state->settle++ == 0) {
                        state->samples = p.recorded_samples;
                        state->paused_image = panel->session().preview().image;
                        window.select_object("glass_ball");
                        control<QDoubleSpinBox>(window, "translation.x")->setValue(-.3);
                        control<QDoubleSpinBox>(window, "roughness")->setValue(.37);
                        window.save_scene_file(directory + "/编辑场景.json");
                        window.load_scene_file(directory + "/编辑场景.json");
                    }
                    require(p.recorded_samples == state->samples &&
                                p.sppm.emitted_photons == p.sppm.iterations * 10000 &&
                                p.recorded_samples == p.sppm.iterations * 24 * 24 && panel->stale(),
                            "Paused SPPM task advanced or lost its frozen snapshot");
                    require(state->paused_image &&
                                panel->session().preview().image == state->paused_image,
                            "Paused SPPM image changed");
                    if (state->settle < 4) {
                        return;
                    }
                    require(control<QLabel>(window, "offline_notice")->text().contains("重新计算"),
                            "SPPM pause does not explain iteration rollback");
                    click(window, "offline_pause");
                    next();
                    return;
                case Stage::resumed:
                    require(p.job_id == state->job, "SPPM resume created a new task");
                    if (!completed()) {
                        return;
                    }
                    next();
                    return;
                case Stage::old_result:
                    if (viewport->offline_image() != panel->session().result() ||
                        ++state->settle < 3) {
                        return;
                    }
                    state->probe->verify([&] {
                        verify_result(window, state->snapshot, state->render, state->integrator);
                        verify_display(window, directory, "old-snapshot");
                    });
                    require(panel->stale(), "Scene edits did not mark the old SPPM image stale");
                    state->render = {16, 16, 8, 101};
                    state->integrator.sppm.photons_per_iteration = 2000;
                    panel->set_settings(state->render, state->integrator, {.threads = 2});
                    state->snapshot = make_snapshot(window.document());
                    click(window, "offline_start");
                    next();
                    return;
                case Stage::new_result:
                    if (!completed()) {
                        return;
                    }
                    if (viewport->offline_image() != panel->session().result() ||
                        ++state->settle < 3) {
                        return;
                    }
                    next();
                    return;
                case Stage::display:
                    state->probe->verify([&] {
                        verify_result(window, state->snapshot, state->render, state->integrator);
                        verify_display(window, directory, "new-snapshot");
                        verify_layout(window, directory);
                    });
                    state->job = p.job_id;
                    state->samples = p.recorded_samples;
                    control<QDoubleSpinBox>(window, "offline_exposure")->setValue(2);
                    next();
                    return;
                case Stage::exposure:
                    require(!panel->stale() && p.job_id == state->job &&
                                p.recorded_samples == state->samples,
                            "Exposure restarted SPPM sampling");
                    state->probe->verify([&] { verify_display(window, directory, "exposure"); });
                    panel->save_image(directory + "/result.exr", true);
                    next();
                    return;
                case Stage::exported:
                    if (panel->exporting()) {
                        return;
                    }
                    require(Image::read_exr(
                                std::filesystem::path((directory + "/result.exr").toStdU16String()))
                                    .at(8, 8) == panel->session().result()->at(8, 8),
                            "SPPM EXR export applied display exposure");
                    control<QComboBox>(window, "offline_backend")->setCurrentIndex(0);
                    state->integrator.kind = IntegratorKind::path_tracing;
                    click(window, "offline_start");
                    next();
                    return;
                case Stage::pt_result:
                    if (!completed() ||
                        !control<QPushButton>(window, "offline_start")->isEnabled()) {
                        return;
                    }
                    state->probe->verify([&] {
                        verify_result(window, state->snapshot, state->render, state->integrator);
                    });
                    control<QComboBox>(window, "offline_backend")->setCurrentIndex(1);
                    control<QDoubleSpinBox>(window, "aperture_radius")->setValue(.01);
                    state->job = p.job_id;
                    click(window, "offline_start");
                    next();
                    return;
                case Stage::aperture:
                    require(p.job_id == state->job &&
                                window.document().camera.aperture_radius > 0 &&
                                control<QLabel>(window, "offline_notice")->text().contains("针孔"),
                            "Unsupported SPPM camera was changed or lacks an inline diagnostic");
                    action(window, "builtin_cornell_fog");
                    next();
                    return;
                case Stage::medium_ready:
                    if (window.asset_loading() || window.document().media.empty() ||
                        control<QComboBox>(window, "offline_backend")->currentIndex() != 0) {
                        return;
                    }
                    control<QComboBox>(window, "offline_backend")->setCurrentIndex(1);
                    click(window, "offline_start");
                    require(panel->session().progress().job_id == state->job &&
                                !window.document().media.empty() &&
                                control<QLabel>(window, "offline_notice")->text().contains("介质"),
                            "Unsupported SPPM media were changed or silently rendered");
                    action(window, "builtin_mirror_caustic");
                    next();
                    return;
                case Stage::mirror_ready:
                    if (window.asset_loading() ||
                        control<QComboBox>(window, "offline_backend")->currentIndex() != 1) {
                        return;
                    }
                    state->integrator.kind = IntegratorKind::sppm;
                    state->integrator.sppm.memory_budget_bytes = 1024 * 1024;
                    panel->set_settings({128, 128, 8, 101}, state->integrator, {.threads = 2});
                    click(window, "offline_start");
                    next();
                    return;
                case Stage::budget:
                    if (!render_terminal(p.state) ||
                        !control<QPushButton>(window, "offline_start")->isEnabled()) {
                        return;
                    }
                    require(
                        p.state == RenderState::failed &&
                            p.error.find("budget") != std::string::npos &&
                            control<QLabel>(window, "offline_notice")->text().contains("budget"),
                        "SPPM memory failure has no inline diagnostic");
                    state->integrator.sppm.memory_budget_bytes = 128 * 1024 * 1024;
                    state->integrator.sppm.photons_per_iteration = 2000000;
                    panel->set_settings({64, 64, 1000000, 101}, state->integrator, {.threads = 2});
                    click(window, "offline_start");
                    next();
                    return;
                case Stage::cancelling:
                    require(p.state != RenderState::failed, p.error.c_str());
                    if (p.state != RenderState::running ||
                        p.sppm_progress.phase != SppmPhase::photons ||
                        p.sppm_progress.completed == 0 ||
                        control<QProgressBar>(window, "offline_phase_progress")->value() == 0 ||
                        !control<QLabel>(window, "offline_phase_status")
                             ->text()
                             .contains("光子追踪")) {
                        return;
                    }
                    require(control<QProgressBar>(window, "offline_phase_progress")->value() > 0,
                            "SPPM photon phase shows no live progress");
                    state->cancellation.start();
                    click(window, "offline_cancel");
                    next();
                    return;
                case Stage::cancelled:
                    if (!render_terminal(p.state) ||
                        !control<QPushButton>(window, "offline_start")->isEnabled()) {
                        return;
                    }
                    require(p.state == RenderState::cancelled &&
                                state->cancellation.elapsed() < 2000 &&
                                p.sppm.emitted_photons == p.sppm.iterations * 2000000,
                            "SPPM cancel blocked or published an incomplete iteration");
                    qInfo() << "SPPM cancellation ms" << state->cancellation.elapsed();
                    click(window, "offline_start");
                    next();
                    return;
                case Stage::exit_running:
                    if (p.state != RenderState::running || p.sppm_progress.completed == 0) {
                        return;
                    }
                    state->probe->report(enforce_response);
                    timer->stop();
                    qInfo()
                        << "SPPM smoke PASS: backend settings, precision, frozen snapshots, "
                           "pause/resume, iteration progress, export, PT, diagnostics and shutdown";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "SPPM smoke FAIL:" << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
