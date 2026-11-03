#include "parameter_smoke.hpp"
#include "workbench.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QProgressBar>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {

struct State {
    int step = 0;
    int ticks = 0;
    int previous_frame = 0;
    bool waiting = false;
    QElapsedTimer elapsed;
    double maximum_callback_ms = 0;
    qint64 maximum_frame_ms = 0;
    SceneUploadStatistics initial;
    int import_stage = 0;
    int import_frame = 0;
    int import_objects = 0;
    bool import_heartbeat = false;
};

} // namespace

void start_parameter_smoke(Workbench& window, const QString& directory) {
    if (!QDir().mkpath(directory)) {
        throw std::runtime_error("Cannot create parameter test directory");
    }
    const auto state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            auto* viewport = window.viewport();
            if (++state->ticks > 1000 || !viewport->failure().isEmpty()) {
                throw std::runtime_error("Parameter test timed out or renderer failed");
            }
            if (viewport->rendered_frames() < 5) {
                return;
            }
            if (!viewport->isExposed()) {
                window.raise();
                window.activateWindow();
                return;
            }
            if (state->waiting) {
                if (viewport->rendered_frames() <= state->previous_frame + 1) {
                    return;
                }
                qInfo() << "Parameter" << state->step - 1 << "next frame ms"
                        << state->elapsed.elapsed() << "last CPU scene update ms"
                        << viewport->upload_statistics().last_update_ms;
                state->maximum_frame_ms =
                    std::max(state->maximum_frame_ms, state->elapsed.elapsed());
                if (viewport->upload_statistics().mesh_uploads != state->initial.mesh_uploads ||
                    viewport->upload_statistics().descriptor_rebuilds !=
                        state->initial.descriptor_rebuilds) {
                    throw std::runtime_error(
                        "Ordinary parameter edit rebuilt immutable GPU assets");
                }
                state->waiting = false;
                return;
            }
            if (state->step == 12) {
                // 后台导入必须立即显示忙碌条；解析期间 Qt timer 和视口仍可前进。
                if (state->import_stage == 0) {
                    const auto source =
                        window.document().asset_directory / window.document().meshes.back().source;
                    state->import_objects = static_cast<int>(window.document().objects.size());
                    state->import_frame = viewport->rendered_frames();
                    window.import_object_file(QString::fromStdU16String(source.u16string()));
                    auto* progress = window.findChild<QProgressBar*>("task_progress");
                    if (!window.asset_loading() || !progress->isVisible() ||
                        progress->maximum() != 0) {
                        throw std::runtime_error("Import did not expose indeterminate progress");
                    }
                    QTimer::singleShot(0, &window, [state] { state->import_heartbeat = true; });
                    state->import_stage = 1;
                    return;
                }
                if (window.asset_loading() || viewport->preview_wait_ms() != 0) {
                    return;
                }
                if (state->import_stage == 1) {
                    if (!state->import_heartbeat ||
                        viewport->rendered_frames() <= state->import_frame ||
                        static_cast<int>(window.document().objects.size()) !=
                            state->import_objects + 1) {
                        throw std::runtime_error(
                            "Background OBJ import failed to complete responsively");
                    }
                    const auto saved = directory + "/saved.json";
                    window.save_scene_file(saved);
                    window.open_scene_file(saved);
                    if (!window.asset_loading()) {
                        throw std::runtime_error("Scene open did not start in background");
                    }
                    state->import_stage = 2;
                    return;
                }
                if (window.findChild<QProgressBar*>("task_progress")->isVisible()) {
                    return; // 状态 timer 在 GPU fence 完成后收起进度条。
                }
                if (state->maximum_callback_ms > 50 || state->maximum_frame_ms > 500) {
                    throw std::runtime_error(
                        "Parameter responsiveness exceeded local regression budget");
                }
                timer->stop();
                qInfo() << "Parameter smoke PASS: 12 operations, no mesh/descriptor rebuilds, "
                           "background OBJ/scene import and progress; max callback ms"
                        << state->maximum_callback_ms << "max next frame ms"
                        << state->maximum_frame_ms;
                QApplication::exit(0);
                return;
            }
            state->previous_frame = viewport->rendered_frames();
            if (state->step == 0) {
                state->initial = viewport->upload_statistics();
                const auto revision = viewport->scene_revision();
                // 没有数值变化的失焦提交不得发布新快照或增加撤销项。
                QMetaObject::invokeMethod(window.findChild<QDoubleSpinBox*>("roughness"),
                                          "editingFinished", Qt::DirectConnection);
                QMetaObject::invokeMethod(window.findChild<QDoubleSpinBox*>("fov"),
                                          "editingFinished", Qt::DirectConnection);
                if (viewport->scene_revision() != revision) {
                    throw std::runtime_error("Unchanged input committed an unnecessary edit");
                }
            }
            state->elapsed.start();
            const int step = state->step++;
            if (step < 3 || step >= 9) {
                window.findChild<QComboBox*>("view_mode")->setCurrentIndex(step % 3);
            } else if (step == 3 || step == 4) {
                auto* spin = window.findChild<QDoubleSpinBox*>("light_intensity");
                spin->setValue(step == 3 ? 4 : 3);
                QMetaObject::invokeMethod(spin, "editingFinished", Qt::DirectConnection);
            } else if (step == 5 || step == 6) {
                window.findChild<QComboBox*>("texture_filter")->setCurrentIndex(step == 5 ? 0 : 2);
            } else {
                auto* spin =
                    window.findChild<QDoubleSpinBox*>(step == 7 ? "roughness" : "translation.x");
                spin->setValue(spin->value() + 0.1);
                QMetaObject::invokeMethod(spin, "editingFinished", Qt::DirectConnection);
            }
            const double callback_ms = static_cast<double>(state->elapsed.nsecsElapsed()) / 1e6;
            state->maximum_callback_ms = std::max(state->maximum_callback_ms, callback_ms);
            qInfo() << "Parameter" << step << "callback ms" << callback_ms;
            state->waiting = true;
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Parameter smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
