#include "adaptive_smoke.hpp"
#include "geometry_panel.hpp"
#include "geometry_smoke.hpp"
#include "offline_panel.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <minirender/assets/scene_io.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* value = window.findChild<T*>(name);
    require(value, "Adaptive control missing");
    return value;
}

// 后台线程每 20ms 投递事件，测量 Qt 主线程的实际排队时延。
// 显式 CPU 参考和 GPU 读回暂停计时；控件操作、任务发布和等待阶段继续计时。
class ResponseProbe final : public QObject {
  public:
    explicit ResponseProbe(QObject* parent) : QObject(parent) {
        event_type_ = QEvent::registerEventType();
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto* event = new Event(event_type_);
                event->epoch = epoch_.load(std::memory_order_relaxed);
                event->sent = std::chrono::steady_clock::now();
                QCoreApplication::postEvent(this, event);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }

    template <class Work> void verify(Work work) {
        QCoreApplication::sendPostedEvents(this, event_type_);
        epoch_.fetch_add(1, std::memory_order_relaxed);
        work();
        epoch_.fetch_add(1, std::memory_order_relaxed);
    }

    void report(bool enforce) {
        QCoreApplication::sendPostedEvents(this, event_type_);
        qInfo() << "Adaptive response samples" << samples_ << "maximum ms" << maximum_ms_;
        if (enforce) {
            require(samples_ >= 10, "Adaptive response probe collected too few events");
            require(maximum_ms_ < 500, "Adaptive UI event latency exceeded 500 milliseconds");
        }
    }

  protected:
    bool event(QEvent* event) override {
        if (event->type() != event_type_) {
            return QObject::event(event);
        }
        const auto& probe = *static_cast<Event*>(event);
        if (probe.epoch == epoch_.load(std::memory_order_relaxed)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - probe.sent)
                                     .count();
            maximum_ms_ = std::max(maximum_ms_, elapsed);
            ++samples_;
        }
        return true;
    }

  private:
    struct Event final : QEvent {
        explicit Event(int type) : QEvent(static_cast<QEvent::Type>(type)) {}
        std::chrono::steady_clock::time_point sent;
        std::uint64_t epoch = 0;
    };
    int event_type_ = 0;
    std::atomic<std::uint64_t> epoch_{0};
    std::chrono::milliseconds::rep maximum_ms_ = 0;
    std::size_t samples_ = 0;
    // 最先销毁 worker，保证 QObject 清理前停止投递。
    std::jthread worker_;
};
std::shared_ptr<const minirender::MeshData> mesh_for(const minirender::SceneDocument& scene,
                                                     const std::string& id) {
    const auto object = std::find_if(scene.objects.begin(), scene.objects.end(),
                                     [&](const auto& o) { return o.id == id; });
    require(object != scene.objects.end(), "Adaptive object missing");
    const auto mesh = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                   [&](const auto& m) { return m.id == object->mesh_id; });
    require(mesh != scene.meshes.end(), "Adaptive mesh missing");
    return mesh->data;
}
struct State {
    int stage = 0;
    int ticks = 0;
    int frame = 0;
    std::shared_ptr<const minirender::MeshData> original;
    std::shared_ptr<const minirender::MeshData> generated;
    std::shared_ptr<const minirender::SceneSnapshot> snapshot;
    minirender::AdaptiveSettings settings;
    std::shared_ptr<const minirender::Texture> height;
    QElapsedTimer cancellation_timer;
    QImage before;
    QImage after;
};
void verify_offline(Workbench& window,
                    const std::shared_ptr<const minirender::SceneSnapshot>& scene) {
    require(window.offline_panel()->session().progress().state ==
                minirender::RenderState::completed,
            "Adaptive offline failed");
    const auto reference =
        minirender::PathTracer(scene, {.maximum_depth = 5}).render({24, 24, 16, 101});
    const auto actual = window.offline_panel()->session().preview().image;
    require(actual && actual->width() == 24, "Adaptive offline preview is missing");
    for (std::size_t y = 0; y < 24; ++y) {
        for (std::size_t x = 0; x < 24; ++x) {
            require(glm::length(actual->at(x, y) - reference.at(x, y)) < 1e-6F,
                    "Adaptive offline differs from independent reference");
        }
    }
}
} // namespace

void start_adaptive_smoke(Workbench& window, const QString& directory,
                          bool measure_responsiveness) {
    require(QDir().mkpath(directory), "Cannot create adaptive output directory");
    minirender::Image height_image(8, 8);
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            height_image.at(x, y) = glm::vec3(.75F);
        }
    }
    height_image.write_exr(std::filesystem::path((directory + "/height.exr").toStdU16String()));
    minirender::save_scene(window.document(),
                           std::filesystem::path((directory + "/start.json").toStdU16String()));
    const auto state = std::make_shared<State>();
    state->original = mesh_for(window.document(), "blue_cube");
    window.select_object("blue_cube");
    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(6);
    auto* timer = new QTimer(&window);
    auto* probe = new ResponseProbe(&window);
    timer->setInterval(60);
    QObject::connect(
        timer, &QTimer::timeout, &window,
        [state, timer, probe, &window, directory, measure_responsiveness] {
            try {
                auto* viewport = window.viewport();
                auto* controller = window.geometry_controller();
                require(++state->ticks < 2000 && viewport->failure().isEmpty(),
                        "Adaptive GUI timed out or Vulkan failed");
                if (!viewport->isValid() || viewport->rendered_frames() < 5) {
                    return;
                }
                const bool ready = !controller->busy() && !window.asset_loading() &&
                                   viewport->rendered_frames() > state->frame + 2 &&
                                   viewport->preview_wait_ms() == 0;
                const auto advance = [&](int stage) {
                    state->stage = stage;
                    state->frame = viewport->rendered_frames();
                };
                switch (state->stage) {
                case 0:
                    probe->verify([&] {
                        state->before = verify_geometry_gpu(window, directory, "adaptive-before");
                    });
                    window.offline_panel()->set_settings(
                        {24, 24, 16, 101}, {.maximum_depth = 5},
                        {.threads = 2, .tile_size = 8, .batch_samples = 4});
                    state->snapshot = minirender::make_snapshot(window.document());
                    window.offline_panel()->start();
                    control<QComboBox>(window, "geometry_operation")->setCurrentIndex(3);
                    control<QSpinBox>(window, "geometry_adaptive_max")->setValue(4);
                    control<QDoubleSpinBox>(window, "geometry_adaptive_error")->setValue(.01);
                    state->settings = *window.geometry_panel()->parameters().adaptive;
                    control<QPushButton>(window, "geometry_apply")->click();
                    require(controller->busy(), "Adaptive task did not start");
                    control<QDoubleSpinBox>(window, "geometry_adaptive_error")->setValue(.1);
                    control<QDoubleSpinBox>(window, "roughness")->setValue(.27);
                    advance(1);
                    return;
                case 1:
                    if (!ready || !minirender::render_terminal(
                                      window.offline_panel()->session().progress().state)) {
                        return;
                    }
                    require(controller->status().state == GeometryTaskState::ready,
                            "Adaptive geometry failed");
                    state->generated = mesh_for(window.document(), "blue_cube");
                    require(state->generated != state->original &&
                                mesh_for(window.document(), "metal_cube") == state->original,
                            "Adaptive asset replacement affected the wrong instance");
                    probe->verify([&] {
                        const auto expected =
                            minirender::subdivide_adaptive(state->original, {}, state->settings);
                        require(expected.mesh->positions == state->generated->positions,
                                "Adaptive job settings changed during execution");
                    });
                    require(control<QLabel>(window, "geometry_metrics")
                                ->text()
                                .contains("本次目标 0.01 m"),
                            "Adaptive report used edited settings");
                    require(
                        control<QLabel>(window, "geometry_metrics")->text().contains("各叶子层级"),
                        "Adaptive level report missing");
                    require(std::abs(control<QDoubleSpinBox>(window, "roughness")->value() - .27) <
                                1e-6,
                            "Adaptive result overwrote material edit");
                    probe->verify([&] { verify_offline(window, state->snapshot); });
                    require(window.offline_panel()->stale(),
                            "Adaptive geometry did not stale old offline snapshot");
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    probe->verify([&] {
                        state->after = verify_geometry_gpu(window, directory, "adaptive-after");
                        control<QComboBox>(window, "view_mode")->setCurrentIndex(3);
                        require(viewport->grab().save(directory + "/adaptive-wire.png"),
                                "Adaptive wire capture failed");
                        control<QComboBox>(window, "view_mode")->setCurrentIndex(0);
                        verify_geometry_layout(window);
                        require(QDir().mkpath(directory + "/shadows"),
                                "Cannot create adaptive shadow directory");
                        verify_geometry_shadows(window, directory + "/shadows");
                        capture_workbench_ui(window, directory + "/adaptive-ui.png");
                    });
                    control<QAction>(window, "undo")->trigger();
                    advance(2);
                    return;
                case 2:
                    if (!ready) {
                        return;
                    }
                    require(mesh_for(window.document(), "blue_cube") == state->original,
                            "Adaptive undo failed");
                    probe->verify([&] {
                        require(verify_geometry_gpu(window, directory, "adaptive-undo") ==
                                    state->before,
                                "Adaptive undo image differs");
                    });
                    control<QAction>(window, "redo")->trigger();
                    advance(3);
                    return;
                case 3:
                    if (!ready) {
                        return;
                    }
                    probe->verify([&] {
                        require(verify_geometry_gpu(window, directory, "adaptive-redo") ==
                                    state->after,
                                "Adaptive redo image differs");
                    });
                    window.export_geometry_file(directory + "/自适应 模型.obj");
                    advance(4);
                    return;
                case 4:
                    if (!ready) {
                        return;
                    }
                    window.save_scene_async(directory + "/自适应 场景.json");
                    advance(5);
                    return;
                case 5:
                    if (!ready) {
                        return;
                    }
                    window.load_scene_file(directory + "/自适应 场景.json");
                    require(mesh_for(window.document(), "blue_cube")->positions ==
                                state->generated->positions,
                            "Adaptive scene round trip lost mesh");
                    probe->verify([&] {
                        require(minirender::load_obj(
                                    std::filesystem::path(
                                        (directory + "/自适应 模型.obj").toStdU16String()))
                                        .positions == state->generated->positions,
                                "Adaptive OBJ round trip lost mesh");
                    });
                    state->snapshot = minirender::make_snapshot(window.document());
                    window.offline_panel()->start();
                    advance(6);
                    return;
                case 6:
                    if (!ready || !minirender::render_terminal(
                                      window.offline_panel()->session().progress().state)) {
                        return;
                    }
                    probe->verify([&] { verify_offline(window, state->snapshot); });
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    window.select_object("blue_cube");
                    control<QSpinBox>(window, "geometry_adaptive_budget")->setValue(1);
                    require(!control<QPushButton>(window, "geometry_apply")->isEnabled(),
                            "Adaptive input budget did not disable generation");
                    control<QSpinBox>(window, "geometry_adaptive_budget")->setValue(500000);
                    window.load_scene_file(directory + "/start.json");
                    window.select_object("blue_cube");
                    control<QComboBox>(window, "geometry_adaptive_metric")->setCurrentIndex(1);
                    control<QSpinBox>(window, "geometry_adaptive_max")->setValue(3);
                    control<QDoubleSpinBox>(window, "geometry_adaptive_error")->setValue(2);
                    state->settings = *window.geometry_panel()->parameters().adaptive;
                    {
                        const auto extent = viewport->swapChainImageSize();
                        const auto& camera = viewport->camera();
                        const auto object = std::find_if(
                            window.document().objects.begin(), window.document().objects.end(),
                            [](const auto& o) { return o.id == "blue_cube"; });
                        state->settings.projection = minirender::AdaptiveProjection{
                            glm::dmat4(
                                camera.projection_matrix(static_cast<float>(extent.width()) /
                                                         static_cast<float>(extent.height()))) *
                                glm::dmat4(camera.view_matrix()) *
                                glm::dmat4(object->transform.matrix()),
                            {extent.width(), extent.height()}};
                    }
                    window.apply_geometry();
                    control<QDoubleSpinBox>(window, "camera_position.x")->setValue(3.3);
                    advance(7);
                    return;
                case 7:
                    if (!ready) {
                        return;
                    }
                    require(controller->status().state == GeometryTaskState::ready,
                            "Projected adaptive job failed");
                    probe->verify([&] {
                        const auto expected =
                            minirender::subdivide_adaptive(state->original, {}, state->settings);
                        require(mesh_for(window.document(), "blue_cube")->positions ==
                                    expected.mesh->positions,
                                "Adaptive projection did not retain start-time view");
                    });
                    require(control<QLabel>(window, "geometry_metrics")->text().contains("px"),
                            "Adaptive pixel units missing");
                    probe->verify(
                        [&] { verify_geometry_gpu(window, directory, "adaptive-pixels"); });
                    window.load_scene_file(directory + "/start.json");
                    window.select_object("blue_cube");
                    window.import_height_file(directory + "/height.exr");
                    advance(8);
                    return;
                case 8:
                    if (!ready) {
                        return;
                    }
                    require(controller->status().state == GeometryTaskState::ready,
                            "Adaptive height import failed");
                    control<QComboBox>(window, "geometry_adaptive_metric")->setCurrentIndex(0);
                    control<QSpinBox>(window, "geometry_adaptive_max")->setValue(4);
                    control<QDoubleSpinBox>(window, "geometry_adaptive_error")->setValue(.01);
                    control<QCheckBox>(window, "geometry_displacement")->setChecked(true);
                    control<QDoubleSpinBox>(window, "geometry_scale")->setValue(.08);
                    {
                        const auto parameters = window.geometry_panel()->parameters();
                        state->settings = *parameters.adaptive;
                        const auto texture = std::find_if(
                            window.document().textures.begin(), window.document().textures.end(),
                            [&](const auto& t) { return t.id == parameters.height_id; });
                        require(texture != window.document().textures.end(),
                                "Adaptive height selection missing");
                        state->height = texture->data;
                    }
                    window.apply_geometry();
                    advance(9);
                    return;
                case 9:
                    if (!ready) {
                        return;
                    }
                    require(controller->status().state == GeometryTaskState::ready,
                            "Displaced adaptive task failed");
                    probe->verify([&] {
                        const auto expected = minirender::subdivide_adaptive(
                            state->original, state->height, state->settings);
                        require(mesh_for(window.document(), "blue_cube")->positions ==
                                    expected.mesh->positions,
                                "Adaptive height settings differ from core");
                    });
                    probe->verify(
                        [&] { verify_geometry_gpu(window, directory, "adaptive-height"); });
                    window.load_scene_file(directory + "/start.json");
                    window.select_object("blue_cube");
                    control<QCheckBox>(window, "geometry_displacement")->setChecked(false);
                    control<QSpinBox>(window, "geometry_adaptive_budget")->setValue(12);
                    control<QDoubleSpinBox>(window, "geometry_adaptive_error")->setValue(.001);
                    state->original = mesh_for(window.document(), "blue_cube");
                    window.apply_geometry();
                    advance(10);
                    return;
                case 10:
                    if (!ready) {
                        return;
                    }
                    require(controller->status().state == GeometryTaskState::failed,
                            "Adaptive growth budget unexpectedly succeeded");
                    require(mesh_for(window.document(), "blue_cube") == state->original,
                            "Adaptive budget failure changed the scene");
                    control<QSpinBox>(window, "geometry_adaptive_budget")->setValue(1000000);
                    control<QSpinBox>(window, "geometry_adaptive_max")->setValue(8);
                    control<QSpinBox>(window, "geometry_adaptive_min")->setValue(8);
                    window.apply_geometry();
                    advance(11);
                    return;
                case 11:
                    require(controller->busy(),
                            "Adaptive cancellation fixture completed too early");
                    // 面板按自己的定时器刷新，等待按钮反映已启动的任务后再触发点击。
                    if (!control<QPushButton>(window, "geometry_cancel")->isEnabled()) {
                        return;
                    }
                    if (controller->status().progress.completed == 0 ||
                        (controller->status().progress.stage.find("平衡相邻层级") ==
                             std::string::npos &&
                         controller->status().progress.stage.find("细化误差区域") ==
                             std::string::npos)) {
                        return;
                    }
                    state->cancellation_timer.start();
                    control<QPushButton>(window, "geometry_cancel")->click();
                    require(controller->status().state == GeometryTaskState::cancelling,
                            "Adaptive cancel button did not request cancellation");
                    advance(12);
                    return;
                case 12:
                    if (controller->busy() || window.asset_loading()) {
                        return;
                    }
                    require(controller->status().state == GeometryTaskState::cancelled,
                            "Adaptive task cancellation failed");
                    require(state->cancellation_timer.elapsed() < 2000,
                            "Adaptive cancellation exceeded two seconds");
                    require(mesh_for(window.document(), "blue_cube") == state->original,
                            "Cancelled adaptive job changed input");
                    window.apply_geometry();
                    require(controller->busy(), "Adaptive shutdown fixture missing");
                    probe->report(measure_responsiveness);
                    timer->stop();
                    qInfo()
                        << "Adaptive smoke PASS: local/pixel/height settings, levels, assets, "
                           "CPU/GPU, "
                           "shadows, wire, history, snapshots, file IO, budget and cancellation";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Adaptive smoke FAIL stage" << state->stage << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
