#include "offline_smoke.hpp"
#include "offline_panel.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::filesystem::path file(const QString& value) {
    const auto bytes = value.toUtf8();
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}

// 在每个源像素映射到的屏幕中心采样，逐通道对照 CPU 的 Reinhard/sRGB 输出。
void verify_display(Workbench& window, const QString& directory) {
    auto* viewport = window.viewport();
    const auto image = viewport->grab();
    require(!image.isNull() && image.save(directory + "/display.png"), "Offline capture failed");
    const auto linear = window.offline_panel()->session().result();
    require(linear != nullptr, "Offline result is missing");
    const auto expected = minirender::display_image(*linear, viewport->exposure());
    const auto source_width = static_cast<double>(linear->width());
    const auto source_height = static_cast<double>(linear->height());
    const double scale = std::min(static_cast<double>(image.width()) / source_width,
                                  static_cast<double>(image.height()) / source_height);
    const auto width = static_cast<int>(std::floor(source_width * scale));
    const auto height = static_cast<int>(std::floor(source_height * scale));
    const int left = (image.width() - width) / 2, top = (image.height() - height) / 2;
    int probes = 0, nonblack = 0;
    for (std::size_t y = 0; y < linear->height(); ++y) {
        for (std::size_t x = 0; x < linear->width(); ++x) {
            const auto pixel = image.pixelColor(
                left + static_cast<int>((static_cast<double>(x) + 0.5) * width / source_width),
                top + static_cast<int>((static_cast<double>(y) + 0.5) * height / source_height));
            const auto color = expected.at(x, y);
            const std::array<int, 3> actual{pixel.red(), pixel.green(), pixel.blue()};
            for (int channel = 0; channel < 3; ++channel) {
                const auto wanted =
                    minirender::encode_unorm8(minirender::linear_to_srgb(color[channel]));
                require(std::abs(actual[static_cast<std::size_t>(channel)] - wanted) <= 2,
                        "Offline Vulkan display differs from CPU display transform");
            }
            nonblack += glm::length(linear->at(x, y)) > 0;
            ++probes;
        }
    }
    require(nonblack > probes / 4, "Offline scene is unexpectedly black");
    if (left > 2 || top > 2) {
        require(image.pixelColor(0, 0) == QColor(0, 0, 0), "Offline image bars are not black");
    }
    qInfo() << "Offline display probes:" << probes << "nonblack" << nonblack;
}

struct State {
    int step = 0, ticks = 0, paused_ticks = 0, cycles = 0;
    std::uint64_t samples = 0, job = 0;
    qint64 previous = 0, maximum_gap = 0;
    int heartbeats = 0, generations = 0;
    QElapsedTimer clock;
    SceneUploadStatistics uploads;
};

} // namespace

void start_offline_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create offline test directory");
    auto state = std::make_shared<State>();
    state->clock.start();
    auto* heartbeat = new QTimer(&window);
    heartbeat->setInterval(10);
    QObject::connect(heartbeat, &QTimer::timeout, &window, [state] {
        const auto now = state->clock.elapsed();
        if (state->previous) {
            state->maximum_gap = std::max(state->maximum_gap, now - state->previous);
        }
        state->previous = now;
        ++state->heartbeats;
    });
    heartbeat->start();
    auto* timer = new QTimer(&window);
    timer->setInterval(100);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, heartbeat, &window, directory] {
            try {
                auto* viewport = window.viewport();
                auto* panel = window.offline_panel();
                if (++state->ticks >= 500) {
                    qCritical() << "Offline timeout at step" << state->step << "state"
                                << QString::fromStdString(minirender::render_state_name(
                                       panel->session().progress().state));
                }
                require(state->ticks < 500 && viewport->failure().isEmpty(),
                        "Offline test timed out or GPU failed");
                if (viewport->rendered_frames() < 5 || !viewport->isExposed()) {
                    window.raise();
                    return;
                }
                const auto button = [&](const char* name) {
                    return panel->findChild<QPushButton*>(name);
                };
                const auto p = panel->session().progress();
                using minirender::RenderState;
                switch (state->step) {
                case 0:
                    window.findChild<QTabWidget*>("inspector_tabs")->setCurrentIndex(3);
                    window.findChild<QDoubleSpinBox*>("aperture_radius")->setValue(0.015);
                    window.findChild<QDoubleSpinBox*>("focus_distance")->setValue(4);
                    require(window.document().camera.aperture_radius > 0,
                            "Lens controls did not edit camera");
                    minirender::save_scene(window.document(), file(directory + "/snapshot.json"));
                    panel->set_settings({32, 24, 1000000, 19},
                                        {minirender::SamplingMode::mis, 6, true, 3}, {2, 8, 2});
                    button("offline_start")->click();
                    ++state->step;
                    break;
                case 1:
                    require(p.state != RenderState::failed, "Offline preparation failed");
                    if (p.state != RenderState::running || !p.recorded_samples ||
                        !button("offline_pause")->isEnabled()) {
                        return;
                    }
                    require(viewport->offline_visible() &&
                                !window.findChild<QComboBox*>("view_mode")->isEnabled(),
                            "Offline display controls are inconsistent");
                    button("offline_pause")->click();
                    ++state->step;
                    break;
                case 2:
                    if (p.state != RenderState::paused) {
                        return;
                    }
                    if (!state->paused_ticks++) {
                        state->samples = p.recorded_samples;
                    }
                    require(p.recorded_samples == state->samples,
                            "Paused UI task continued sampling");
                    if (state->paused_ticks < 3) {
                        return;
                    }
                    require(panel->findChild<QProgressBar*>("offline_progress")->maximum() == 10000,
                            "Paused progress is indeterminate");
                    {
                        const auto before = viewport->camera();
                        const QPointF origin(viewport->width() * 0.5, viewport->height() * 0.5);
                        QMouseEvent press(QEvent::MouseButtonPress, origin,
                                          viewport->mapToGlobal(origin), Qt::LeftButton,
                                          Qt::LeftButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(viewport, &press);
                        QMouseEvent move(QEvent::MouseMove, origin + QPointF(30, 10),
                                         viewport->mapToGlobal(origin + QPointF(30, 10)),
                                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(viewport, &move);
                        QMouseEvent release(QEvent::MouseButtonRelease, origin + QPointF(30, 10),
                                            viewport->mapToGlobal(origin + QPointF(30, 10)),
                                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(viewport, &release);
                        require(viewport->camera().position == before.position &&
                                    viewport->camera().target == before.target,
                                "Offline image drag changed scene camera");
                    }
                    panel->save_image(directory + "/partial.exr", true);
                    ++state->step;
                    break;
                case 3:
                    if (panel->exporting()) {
                        return;
                    }
                    require(std::filesystem::exists(file(directory + "/partial.exr")),
                            "Partial EXR was not exported");
                    require(minirender::Image::read_exr(file(directory + "/partial.exr")).width() ==
                                32,
                            "Partial image dimensions changed");
                    button("offline_pause")->click();
                    ++state->step;
                    break;
                case 4:
                    if (p.recorded_samples <= state->samples) {
                        return;
                    }
                    button("offline_cancel")->click();
                    ++state->step;
                    break;
                case 5:
                    if (!minirender::render_terminal(p.state)) {
                        return;
                    }
                    require(p.state == RenderState::cancelled,
                            "Cancel did not reach terminal state");
                    // 核心终止与 UI 轮询可以错开，等按钮反映终止状态后再点击。
                    if (!button("offline_start")->isEnabled()) {
                        return;
                    }
                    if (state->cycles++ < 3) {
                        button("offline_start")->click();
                        button("offline_cancel")->click();
                        return;
                    }
                    panel->set_settings({24, 16, 8, 19},
                                        {minirender::SamplingMode::mis, 6, true, 3}, {2, 8, 2});
                    button("offline_start")->click();
                    ++state->step;
                    break;
                case 6: {
                    if (!minirender::render_terminal(p.state)) {
                        return;
                    }
                    require(p.state == RenderState::completed && p.recorded_samples == 3072,
                            "Offline final render failed");
                    // 用 snapshot.json 在独立 CLI 进程中对照，避免参考渲染阻塞 UI。
                    panel->save_image(directory + "/result.exr", true);
                    state->job = p.job_id;
                    state->samples = p.recorded_samples;
                    window.findChild<QDoubleSpinBox*>("offline_exposure")->setValue(0.75);
                    state->generations = viewport->resource_generations();
                    window.resize(1000, 600);
                    ++state->step;
                    break;
                }
                case 7:
                    if (panel->exporting() ||
                        viewport->resource_generations() <= state->generations ||
                        !viewport->offline_image()) {
                        return;
                    }
                    require(!panel->stale() && p.job_id == state->job &&
                                p.recorded_samples == state->samples,
                            "Exposure or resize restarted sampling or marked result stale");
                    verify_display(window, directory);
                    panel->save_image(directory + "/result.png", false);
                    ++state->step;
                    break;
                case 8: {
                    if (panel->exporting()) {
                        return;
                    }
                    const auto result =
                        minirender::Image::read_exr(file(directory + "/result.exr"));
                    require(result.at(12, 8) == panel->session().result()->at(12, 8),
                            "EXR export applied exposure");
                    auto* area = qobject_cast<QScrollArea*>(
                        window.findChild<QTabWidget*>("inspector_tabs")->currentWidget());
                    require(area && area->widget()->width() <= area->viewport()->width(),
                            "Offline controls require hidden horizontal scrolling");
                    auto* progress = panel->findChild<QProgressBar*>("offline_progress");
                    area->ensureWidgetVisible(progress);
                    require(area->viewport()->rect().contains(
                                progress->mapTo(area->viewport(), progress->rect().center())),
                            "Offline progress cannot be reached");
                    window.findChild<QDoubleSpinBox*>("fov")->setValue(
                        window.document().camera.vertical_fov_degrees + 1);
                    require(panel->stale(), "Scene edit did not mark offline result stale");
                    state->uploads = viewport->upload_statistics();
                    state->generations = viewport->resource_generations();
                    panel->findChild<QCheckBox*>("offline_show")->setChecked(false);
                    ++state->step;
                    break;
                }
                case 9:
                    require(!viewport->offline_visible() &&
                                window.findChild<QComboBox*>("view_mode")->isEnabled(),
                            "Realtime display did not restore controls");
                    require(viewport->resource_generations() == state->generations &&
                                viewport->upload_statistics().mesh_uploads ==
                                    state->uploads.mesh_uploads &&
                                viewport->upload_statistics().descriptor_rebuilds ==
                                    state->uploads.descriptor_rebuilds,
                            "Display switch rebuilt GPU scene resources");
                    require(state->heartbeats > 50 && state->maximum_gap < 500,
                            "Offline work blocked the Qt event loop");
                    timer->stop();
                    heartbeat->stop();
                    qInfo() << "Offline smoke PASS: pause/resume/cancel/restart, partial/full "
                               "export, Vulkan display, resize, stale state and "
                               "cached realtime; max heartbeat gap"
                            << state->maximum_gap;
                    QApplication::exit(0);
                    break;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Offline smoke FAIL:" << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
