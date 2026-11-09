#include "geometry_smoke.hpp"
#include "geometry_panel.hpp"
#include "ibl_controller.hpp"
#include "offline_panel.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path native(const QString& path) {
    const auto text = path.toUtf8();
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

template <class T> T* control(Workbench& window, const char* name) {
    auto* result = window.findChild<T*>(name);
    require(result != nullptr, "Geometry regression control is missing");
    return result;
}

const minirender::SceneObject& object(const minirender::SceneDocument& scene,
                                      const std::string& id) {
    const auto found = std::find_if(scene.objects.begin(), scene.objects.end(),
                                    [&](const auto& item) { return item.id == id; });
    require(found != scene.objects.end(), "Geometry target is missing");
    return *found;
}

std::shared_ptr<const minirender::MeshData> mesh_for(const minirender::SceneDocument& scene,
                                                     const std::string& id) {
    const auto& target = object(scene, id);
    const auto found = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                    [&](const auto& item) { return item.id == target.mesh_id; });
    require(found != scene.meshes.end(), "Geometry asset is missing");
    return found->data;
}

void prepare(Workbench& window, const QString& directory) {
    minirender::Image height(64, 64);
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            const double u = (static_cast<double>(x) + 0.5) / 64;
            const double v = 1 - (static_cast<double>(y) + 0.5) / 64;
            height.at(x, y) =
                glm::vec3(static_cast<float>(0.5 + 0.35 * std::sin(2 * std::numbers::pi * u) *
                                                       std::sin(2 * std::numbers::pi * v)));
        }
    }
    height.write_exr(native(directory + "/height.exr"));
    minirender::save_scene(window.document(), native(directory + "/start.json"));
    std::ofstream(native(directory + "/cancelled.json"), std::ios::binary)
        << "preserve existing scene bytes\n";
    window.viewport()->set_fxaa_enabled(false);
    auto ao = window.viewport()->ssao_settings();
    ao.enabled = false;
    window.viewport()->set_ssao_settings(ao);
    window.select_object("blue_cube");
    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(6);
}

// 检查表单边界及行间遮挡，并让滚动区实际定位到底部控件。
void verify_layout(Workbench& window) {
    auto* area =
        qobject_cast<QScrollArea*>(control<QTabWidget>(window, "inspector_tabs")->widget(6));
    require(area && area->widget()->width() <= area->viewport()->width(),
            "Geometry panel requires hidden horizontal scrolling");
    auto* panel = area->widget();
    auto* form = qobject_cast<QFormLayout*>(panel->layout());
    require(form, "Geometry form is missing");
    std::vector<QWidget*> fields;
    QWidget* bottom = nullptr;
    for (int i = 0; i < form->count(); ++i) {
        auto* field = form->itemAt(i)->widget();
        if (!field || !field->isVisibleTo(panel)) {
            continue;
        }
        const auto rect = field->geometry();
        require(rect.left() >= 0 && rect.right() < panel->width(),
                "Geometry field exceeds panel bounds");
        for (auto* other : fields) {
            const auto overlap = rect.intersected(other->geometry());
            require(overlap.width() <= 1 || overlap.height() <= 1, "Geometry fields overlap");
        }
        fields.push_back(field);
        if (!bottom || rect.bottom() > bottom->geometry().bottom()) {
            bottom = field;
        }
    }
    require(fields.size() > 10 && bottom, "Geometry form controls are missing");
    area->ensureWidgetVisible(bottom);
    require(
        area->viewport()->rect().contains(bottom->mapTo(area->viewport(), bottom->rect().center())),
        "Geometry bottom controls are inaccessible");
}

// 使用 CPU 世界空间命中法线和相机深度，核对新网格的实际 GPU 缓冲内容。
QImage verify_gpu(Workbench& window, const QString& directory, const QString& label) {
    auto* viewport = window.viewport();
    const auto snapshot = minirender::make_snapshot(window.document());
    const minirender::SceneBvh bvh(snapshot);
    control<QComboBox>(window, "view_mode")->setCurrentIndex(1);
    const auto normals = viewport->grab();
    control<QComboBox>(window, "view_mode")->setCurrentIndex(2);
    const auto depth = viewport->grab();
    require(normals.size() == depth.size(), "Geometry debug capture dimensions changed");
    int checked = 0, matched = 0, target = 0;
    const double aspect = static_cast<double>(normals.width()) / normals.height();
    const glm::dmat4 view(snapshot->camera.view_matrix());
    for (int gy = 0; gy < 64; ++gy) {
        for (int gx = 0; gx < 80; ++gx) {
            const int x = static_cast<int>((gx + 0.43) * normals.width() / 80);
            const int y = static_cast<int>((gy + 0.37) * normals.height() / 64);
            const auto ray = snapshot->camera.generate_ray(
                {(x + 0.5) / normals.width(), (y + 0.5) / normals.height()}, aspect);
            const auto hit = bvh.intersect(ray);
            if (!hit ||
                std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < 0.025) {
                continue;
            }
            auto n = glm::normalize(glm::dvec3(hit->shading_normal));
            if (glm::dot(n, -ray.direction) < 0) {
                n = -n;
            }
            const auto expected_normal = glm::vec3(n * 0.5 + 0.5);
            const double d = -(view * glm::dvec4(hit->position, 1)).z;
            const auto expected_depth = static_cast<float>(1 / (1 + d));
            const auto actual = normals.pixelColor(x, y);
            const int channels[] = {actual.red(), actual.green(), actual.blue()};
            bool equal = true;
            for (int channel = 0; channel < 3; ++channel) {
                const int value =
                    minirender::encode_unorm8(minirender::linear_to_srgb(expected_normal[channel]));
                equal = equal && std::abs(channels[channel] - value) <= 3;
            }
            const int depth_value =
                minirender::encode_unorm8(minirender::linear_to_srgb(expected_depth));
            equal = equal && std::abs(depth.pixelColor(x, y).red() - depth_value) <= 3;
            ++checked;
            matched += equal;
            target += snapshot->instances[hit->instance_index].object_id == "blue_cube";
        }
    }
    qInfo() << "Geometry GPU normal/depth" << label << matched << "/" << checked << "target"
            << target;
    require(target >= 5 && checked > 100 && matched * 100 >= checked * 99,
            "Geometry GPU normal/depth disagrees with CPU intersections");
    require(normals.save(directory + "/" + label + "-normals.png") &&
                depth.save(directory + "/" + label + "-depth.png"),
            "Cannot save geometry captures");
    control<QComboBox>(window, "view_mode")->setCurrentIndex(0);
    return normals;
}

int changed_pixels(const QImage& first, const QImage& second) {
    require(first.size() == second.size(), "Geometry image extents changed");
    int changed = 0;
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            const auto a = first.pixelColor(x, y), b = second.pixelColor(x, y);
            changed += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
                           std::abs(a.blue() - b.blue()) >
                       8;
        }
    }
    return changed;
}

// 地面阴影以 CPU 方向光遮挡查询为参照，剔除 PCF 边界附近的采样点。
void verify_shadows(Workbench& window, const QString& directory) {
    auto* viewport = window.viewport();
    const auto snapshot = minirender::make_snapshot(window.document());
    require(snapshot->directional_lights.size() == 1, "Geometry shadow fixture requires one light");
    const minirender::SceneBvh bvh(snapshot);
    control<QComboBox>(window, "view_mode")->setCurrentIndex(0);
    control<QCheckBox>(window, "shadows_enabled")->setChecked(true);
    const auto on = viewport->grab();
    control<QCheckBox>(window, "shadows_enabled")->setChecked(false);
    const auto off = viewport->grab();
    control<QCheckBox>(window, "shadows_enabled")->setChecked(true);
    require(on.size() == off.size(), "Geometry shadow capture dimensions changed");
    const auto direction = -glm::normalize(glm::dvec3(snapshot->directional_lights[0].direction));
    const double aspect = static_cast<double>(on.width()) / on.height();
    const auto matrix = snapshot->camera.projection_matrix(static_cast<float>(aspect)) *
                        snapshot->camera.view_matrix();
    int blocked = 0, blocked_matches = 0, lit = 0, lit_matches = 0;
    for (int gz = -40; gz <= 40; ++gz) {
        for (int gx = -40; gx <= 40; ++gx) {
            const glm::vec3 point{static_cast<float>(gx) * 0.06F, 0,
                                  static_cast<float>(gz) * 0.06F};
            const auto clip = matrix * glm::vec4(point, 1);
            if (clip.w <= 0) {
                continue;
            }
            const auto uv = glm::vec2(clip) / clip.w * 0.5F + 0.5F;
            if (uv.x < 0.02F || uv.x > 0.98F || uv.y < 0.02F || uv.y > 0.98F) {
                continue;
            }
            const int x = static_cast<int>(uv.x * static_cast<float>(on.width()));
            const int y = static_cast<int>(uv.y * static_cast<float>(on.height()));
            const auto ray = snapshot->camera.generate_ray(
                {(x + 0.5) / on.width(), (y + 0.5) / on.height()}, aspect);
            const auto hit = bvh.intersect(ray);
            if (!hit || snapshot->instances[hit->instance_index].object_id != "ground") {
                continue;
            }
            const auto origin = minirender::offset_origin(*hit, direction);
            const bool expected = bvh.occluded({origin, direction});
            bool stable = true;
            for (const auto offset : {glm::dvec3(0.03, 0, 0), glm::dvec3(-0.03, 0, 0),
                                      glm::dvec3(0, 0, 0.03), glm::dvec3(0, 0, -0.03)}) {
                stable = stable && bvh.occluded({origin + offset, direction}) == expected;
            }
            if (!stable) {
                continue;
            }
            const auto a = on.pixelColor(x, y), b = off.pixelColor(x, y);
            const bool darker =
                b.red() + b.green() + b.blue() > a.red() + a.green() + a.blue() + 15;
            if (expected) {
                ++blocked;
                blocked_matches += darker;
            } else {
                ++lit;
                lit_matches += !darker;
            }
        }
    }
    qInfo() << "Geometry shadow probes blocked" << blocked_matches << "/" << blocked << "lit"
            << lit_matches << "/" << lit;
    require(blocked >= 3 && lit >= 100 && blocked_matches * 100 >= blocked * 90 &&
                lit_matches * 100 >= lit * 98,
            "Generated geometry GPU shadows disagree with CPU rays");
    int surface_lit = 0, surface_matches = 0;
    for (int gy = 0; gy < 80; ++gy) {
        for (int gx = 0; gx < 100; ++gx) {
            const int x = static_cast<int>((gx + 0.43) * on.width() / 100);
            const int y = static_cast<int>((gy + 0.37) * on.height() / 80);
            const auto ray = snapshot->camera.generate_ray(
                {(x + 0.5) / on.width(), (y + 0.5) / on.height()}, aspect);
            const auto hit = bvh.intersect(ray);
            if (!hit || snapshot->instances[hit->instance_index].object_id != "blue_cube" ||
                glm::dot(glm::dvec3(hit->geometric_normal), direction) < 0.3 ||
                glm::dot(glm::dvec3(hit->shading_normal), direction) < 0.3 ||
                bvh.occluded(minirender::spawn_ray(*hit, direction))) {
                continue;
            }
            const auto a = on.pixelColor(x, y), b = off.pixelColor(x, y);
            const bool equal = std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
                                   std::abs(a.blue() - b.blue()) <=
                               8;
            ++surface_lit;
            surface_matches += equal;
        }
    }
    qInfo() << "Geometry lit surface probes" << surface_matches << "/" << surface_lit;
    require(on.save(directory + "/shadows-on.png") && off.save(directory + "/shadows-off.png"),
            "Cannot save geometry shadow captures");
    require(surface_lit >= 10 && surface_matches * 100 >= surface_lit * 98,
            "Generated geometry has self-shadow artifacts on directly lit surfaces");
}

minirender::RenderSettings offline_settings() {
    minirender::RenderSettings settings;
    settings.width = 16;
    settings.height = 12;
    settings.samples_per_pixel = 8;
    settings.seed = 101;
    return settings;
}
minirender::IntegratorSettings integrator() {
    minirender::IntegratorSettings result;
    result.maximum_depth = 3;
    result.russian_roulette = false;
    return result;
}

void verify_offline(Workbench& window, std::shared_ptr<const minirender::SceneSnapshot> snapshot) {
    const minirender::PathTracer reference(std::move(snapshot), integrator());
    const auto expected = reference.render(offline_settings());
    const auto actual = window.offline_panel()->session().result();
    require(actual && actual->width() == expected.width() && actual->height() == expected.height(),
            "Geometry offline result has incorrect dimensions");
    for (std::size_t y = 0; y < expected.height(); ++y) {
        for (std::size_t x = 0; x < expected.width(); ++x) {
            require(actual->at(x, y) == expected.at(x, y),
                    "Geometry offline snapshot changed during rendering");
        }
    }
}

// QWidget 截图中补入同一视口的原生 QWindow 读回，保留表单与实际渲染画面。
void capture_ui(Workbench& window, const QString& path) {
    auto image = window.grab();
    const auto viewport_image = window.viewport()->grab();
    QPainter painter(&image);
    const auto origin =
        window.viewport()->mapToGlobal(QPoint(0, 0)) - window.mapToGlobal(QPoint(0, 0));
    painter.drawImage(QRect(origin, window.viewport()->size()), viewport_image);
    painter.end();
    require(image.save(path), "Cannot save geometry UI screenshot");
}

enum class Stage {
    startup,
    height_import,
    small_apply,
    small_upload,
    offline_new,
    undo,
    redo,
    export_small,
    save_snapshot,
    guard_export,
    cancel_start,
    cancel_wait,
    discard_wait,
    load_cancel_wait,
    large_apply,
    large_upload,
    save_cancel_start,
    save_cancel_wait,
    export_large,
    import_large,
    qem_start,
    qem_apply,
    qem_offline,
    qem_undo,
    qem_redo,
    qem_limited,
    qem_constrained,
    qem_export,
    qem_save,
    remesh_apply,
    remesh_offline,
    remesh_undo,
    remesh_redo,
    remesh_export,
    remesh_save,
    remesh_budget_wait,
    qem_cancel_start,
    qem_cancel_phase,
    qem_cancel_wait,
    remesh_cancel_phase,
    remesh_cancel_wait,
    shutdown
};

struct State {
    Stage stage = Stage::startup;
    int ticks = 0, maximum_ticks = 4000, frame = 0;
    std::uint64_t uploads = 0;
    bool monitor = false, numeric_progress = false, cancel_sent = false, renderable = true;
    bool active = false;
    bool offscreen = false;
    qint64 last = 0, maximum_gap = 0, cancel_at = 0;
    qint64 maximum_event_latency = 0;
    std::size_t probe_samples = 0;
    std::atomic<std::uint64_t> probe_epoch{0};
    QObject* probe_receiver = nullptr;
    int probe_event_type = 0;
    std::chrono::steady_clock::time_point measurement_started{};
    QElapsedTimer clock;
    std::shared_ptr<const minirender::MeshData> source, generated, large, simplified, remeshed,
        remesh_input;
    minirender::RemeshSettings remesh_settings;
    std::shared_ptr<const minirender::SceneSnapshot> snapshot;
    QImage baseline, after, qem_before, qem_after, remesh_before, remesh_after;
    std::size_t original_meshes = 0, original_objects = 0;
    float saved_x = 0;
    std::filesystem::path input_file;
    std::string original_input;
};

struct UiProbeEvent final : QEvent {
    explicit UiProbeEvent(int type) : QEvent(static_cast<QEvent::Type>(type)) {}
    std::chrono::steady_clock::time_point sent;
    std::uint64_t epoch = 0;
};

// 先创建事件，再记录投递时间，避免把发送线程的事件分配耗时计入界面排队时延。
class UiEventProbe final : public QObject {
  public:
    UiEventProbe(QObject* parent, std::shared_ptr<State> state) : QObject(parent), state_(state) {
        state_->probe_receiver = this;
        state_->probe_event_type = QEvent::registerEventType();
        worker_ = std::jthread([this, state](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto* event = new UiProbeEvent(state->probe_event_type);
                event->epoch = state->probe_epoch.load(std::memory_order_relaxed);
                event->sent = std::chrono::steady_clock::now();
                QCoreApplication::postEvent(this, event);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }

  protected:
    bool event(QEvent* event) override {
        if (event->type() != state_->probe_event_type) {
            return QObject::event(event);
        }
        const auto& probe = *static_cast<UiProbeEvent*>(event);
        if (state_->monitor && state_->active && state_->renderable &&
            probe.epoch == state_->probe_epoch.load(std::memory_order_relaxed) &&
            probe.sent >= state_->measurement_started) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - probe.sent)
                                     .count();
            state_->maximum_event_latency =
                std::max(state_->maximum_event_latency, static_cast<qint64>(elapsed));
            ++state_->probe_samples;
            if (elapsed > 150) {
                qInfo() << "Geometry queued event ms" << elapsed << "stage"
                        << static_cast<int>(state_->stage);
            }
        }
        return true;
    }

  private:
    std::shared_ptr<State> state_;
    std::jthread worker_;
};

void record_gap(State& state) {
    const auto now = state.clock.elapsed();
    if (!state.active || !state.renderable) {
        state.last = 0;
        return;
    }
    if (state.monitor && state.last) {
        if (now - state.last > 150) {
            qInfo() << "Geometry heartbeat gap ms" << now - state.last << "stage"
                    << static_cast<int>(state.stage) << "application state"
                    << static_cast<int>(QGuiApplication::applicationState()) << "renderable"
                    << state.renderable;
        }
        state.maximum_gap = std::max(state.maximum_gap, now - state.last);
    }
    state.last = now;
}

// 显式截图/CPU 对照不属于交互响应计时，进入对照前先记录此前未处理的主线程间隔。
template <class Work> void verify(State& state, Work work) {
    QCoreApplication::sendPostedEvents(state.probe_receiver, state.probe_event_type);
    record_gap(state);
    state.monitor = false;
    state.probe_epoch.fetch_add(1, std::memory_order_relaxed);
    work();
    state.last = state.clock.elapsed();
    state.measurement_started = std::chrono::steady_clock::now();
    state.probe_epoch.fetch_add(1, std::memory_order_relaxed);
    state.monitor = true;
}

bool finished(Workbench& window) {
    return !window.geometry_controller()->busy() && !window.asset_loading();
}
void ready(Workbench& window) {
    require(window.geometry_controller()->status().state == GeometryTaskState::ready,
            "Geometry task did not finish successfully");
}
void generate(Workbench& window, int levels, bool displace) {
    control<QComboBox>(window, "geometry_operation")->setCurrentIndex(0);
    control<QSpinBox>(window, "geometry_levels")->setValue(levels);
    control<QCheckBox>(window, "geometry_displacement")->setChecked(displace);
    auto* apply = control<QPushButton>(window, "geometry_apply");
    require(apply->isEnabled(), "Geometry apply is unexpectedly disabled");
    apply->click();
    require(window.geometry_controller()->busy() && window.asset_loading(),
            "Geometry task did not start");
}

} // namespace

QImage verify_geometry_gpu(Workbench& window, const QString& directory, const QString& label) {
    return verify_gpu(window, directory, label);
}
void verify_geometry_shadows(Workbench& window, const QString& directory) {
    verify_shadows(window, directory);
}
void verify_geometry_layout(Workbench& window) {
    verify_layout(window);
}
void capture_workbench_ui(Workbench& window, const QString& path) {
    capture_ui(window, path);
}

void start_geometry_smoke(Workbench& window, const QString& directory,
                          bool measure_responsiveness) {
    require(QDir().mkpath(directory), "Cannot create geometry output directory");
    prepare(window, directory);
    const auto state = std::make_shared<State>();
    bool budget_valid = false;
    const int budget =
        qEnvironmentVariableIntValue("MINIRENDER_GEOMETRY_TEST_TICKS", &budget_valid);
    if (budget_valid && budget > 0 && budget <= 20000) {
        state->maximum_ticks = budget;
    }
    state->clock.start();
    state->offscreen = window.viewport()->offscreen_rendering();
    state->active =
        state->offscreen || QGuiApplication::applicationState() == Qt::ApplicationActive;
    QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, &window,
                     [state](Qt::ApplicationState value) {
                         state->active = state->offscreen || value == Qt::ApplicationActive;
                         state->last = 0;
                         state->measurement_started = std::chrono::steady_clock::now();
                         state->probe_epoch.fetch_add(1, std::memory_order_relaxed);
                     });
    new UiEventProbe(&window, state);
    auto* heartbeat = new QTimer(&window);
    heartbeat->setInterval(10);
    QObject::connect(heartbeat, &QTimer::timeout, &window, [state, &window] {
        state->renderable = window.viewport()->can_render();
        record_gap(*state);
    });
    heartbeat->start();
    auto* timer = new QTimer(&window);
    timer->setInterval(20);
    QObject::connect(
        timer, &QTimer::timeout, &window,
        [state, timer, &window, directory, measure_responsiveness] {
            try {
                auto* viewport = window.viewport();
                auto* tasks = window.geometry_controller();
                const auto status = tasks->status();
                if (++state->ticks > state->maximum_ticks || !viewport->failure().isEmpty()) {
                    throw std::runtime_error("Geometry test timed out or renderer failed");
                }
                if (!viewport->can_render()) {
                    state->last = 0;
                    return;
                }
                if (viewport->rendered_frames() < 5) {
                    return;
                }
                const auto advance = [&](Stage stage) {
                    qInfo() << "Geometry stage" << static_cast<int>(state->stage) << "->"
                            << static_cast<int>(stage);
                    state->stage = stage;
                    state->frame = viewport->rendered_frames();
                    state->uploads = viewport->upload_statistics().mesh_uploads;
                };
                const auto preview_ready = [&] {
                    return viewport->rendered_frames() > state->frame + 2 &&
                           viewport->preview_wait_ms() == 0;
                };
                const auto offline_done = [&] {
                    const auto progress = window.offline_panel()->session().progress();
                    require(progress.state != minirender::RenderState::failed,
                            "Geometry offline task failed");
                    return progress.state == minirender::RenderState::completed;
                };
                switch (state->stage) {
                case Stage::startup: {
                    if (window.ibl_controller()->status().state != IblTaskState::ready) {
                        return;
                    }
                    state->source = mesh_for(window.document(), "blue_cube");
                    require(mesh_for(window.document(), "metal_cube") == state->source,
                            "Geometry fixture does not share the cube mesh");
                    state->original_meshes = window.document().meshes.size();
                    state->original_objects = window.document().objects.size();
                    state->input_file =
                        window.document().asset_directory / window.document().meshes[0].source;
                    state->original_input = bytes(state->input_file);
                    state->monitor = true;
                    state->measurement_started = std::chrono::steady_clock::now();
                    state->probe_epoch.fetch_add(1, std::memory_order_relaxed);
                    verify(*state, [&] { verify_layout(window); });
                    window.import_height_file(directory + "/height.exr");
                    require(window.asset_loading(),
                            "Height import did not start in the background");
                    advance(Stage::height_import);
                    return;
                }
                case Stage::height_import:
                    if (!finished(window) || !preview_ready()) {
                        return;
                    }
                    ready(window);
                    require(window.geometry_panel()->parameters().displacement.has_value(),
                            "Height import did not select displacement parameters");
                    for (const auto& material : window.document().materials) {
                        require(material.base_color_texture.empty(),
                                "Height import changed albedo textures");
                    }
                    require(window.document().environment_map.texture_id.empty(),
                            "Height import changed the environment");
                    require(window.document().textures.back().encoding ==
                                minirender::ColorEncoding::linear,
                            "Height import applied sRGB decoding");
                    verify(*state,
                           [&] { state->baseline = verify_gpu(window, directory, "before"); });
                    state->snapshot = minirender::make_snapshot(window.document());
                    window.offline_panel()->set_settings(offline_settings(), integrator(),
                                                         {2, 4, 2});
                    control<QPushButton>(window, "offline_start")->click();
                    control<QDoubleSpinBox>(window, "geometry_scale")->setValue(0.15);
                    generate(window, 3, true);
                    control<QDoubleSpinBox>(window, "roughness")->setValue(0.32);
                    advance(Stage::small_apply);
                    return;
                case Stage::small_apply:
                    if (!finished(window) || !offline_done()) {
                        return;
                    }
                    ready(window);
                    state->generated = mesh_for(window.document(), "blue_cube");
                    require(state->generated != state->source &&
                                state->generated->triangles.size() == 768,
                            "Geometry result was not applied to the selected object");
                    require(mesh_for(window.document(), "metal_cube") == state->source,
                            "Geometry processing changed another shared instance");
                    require(window.document().meshes.size() == state->original_meshes + 1,
                            "Geometry result did not create an independent asset");
                    require(std::abs(control<QDoubleSpinBox>(window, "roughness")->value() - 0.32) <
                                1e-6,
                            "Geometry result overwrote an unrelated parameter edit");
                    require(window.offline_panel()->stale(),
                            "Geometry changes did not mark the old render stale");
                    verify(*state, [&] { verify_offline(window, state->snapshot); });
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    {
                        const auto initial_uploads = state->uploads;
                        advance(Stage::small_upload);
                        state->uploads = initial_uploads;
                    }
                    return;
                case Stage::small_upload:
                    if (!preview_ready() ||
                        viewport->upload_statistics().mesh_uploads <= state->uploads ||
                        !control<QPushButton>(window, "offline_start")->isEnabled()) {
                        return;
                    }
                    verify(*state, [&] {
                        state->after = verify_gpu(window, directory, "displaced");
                        require(changed_pixels(state->baseline, state->after) > 100,
                                "Geometry processing did not change the GPU silhouette or normals");
                        verify_shadows(window, directory);
                    });
                    state->snapshot = minirender::make_snapshot(window.document());
                    {
                        const auto previous_job =
                            window.offline_panel()->session().progress().job_id;
                        control<QPushButton>(window, "offline_start")->click();
                        require(window.offline_panel()->session().progress().job_id != previous_job,
                                "Fresh geometry offline task did not start");
                    }
                    advance(Stage::offline_new);
                    return;
                case Stage::offline_new:
                    if (!offline_done()) {
                        return;
                    }
                    verify(*state, [&] { verify_offline(window, state->snapshot); });
                    require(!window.offline_panel()->stale(),
                            "A fresh geometry render is already stale");
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    control<QAction>(window, "undo")->trigger();
                    require(mesh_for(window.document(), "blue_cube") == state->source,
                            "Geometry undo failed");
                    advance(Stage::undo);
                    return;
                case Stage::undo:
                    if (!preview_ready()) {
                        return;
                    }
                    verify(*state, [&] {
                        const auto undo = verify_gpu(window, directory, "undo");
                        require(changed_pixels(state->baseline, undo) == 0,
                                "GPU geometry undo did not restore the original mesh");
                    });
                    control<QAction>(window, "redo")->trigger();
                    require(mesh_for(window.document(), "blue_cube") == state->generated,
                            "Geometry redo failed");
                    advance(Stage::redo);
                    return;
                case Stage::redo:
                    if (!preview_ready()) {
                        return;
                    }
                    verify(*state, [&] {
                        require(changed_pixels(state->after,
                                               verify_gpu(window, directory, "redo")) == 0,
                                "GPU geometry redo did not restore the generated mesh");
                    });
                    window.export_geometry_file(directory + "/输出 模型.obj");
                    advance(Stage::export_small);
                    return;
                case Stage::export_small:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    verify(*state, [&] {
                        const auto exported =
                            minirender::load_obj(native(directory + "/输出 模型.obj"));
                        require(exported.positions == state->generated->positions &&
                                    exported.triangles.size() == 768,
                                "Geometry OBJ export changed shared positions");
                    });
                    state->saved_x = object(window.document(), "blue_cube").transform.translation.x;
                    window.save_scene_async(directory + "/保存 场景.json");
                    control<QDoubleSpinBox>(window, "translation.x")
                        ->setValue(state->saved_x + 0.15);
                    advance(Stage::save_snapshot);
                    return;
                case Stage::save_snapshot:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    verify(*state, [&] {
                        const auto saved =
                            minirender::load_scene(native(directory + "/保存 场景.json"));
                        require(std::abs(object(saved, "blue_cube").transform.translation.x -
                                         state->saved_x) < 1e-6,
                                "Scene save did not preserve its start-time snapshot");
                        require(mesh_for(saved, "blue_cube")->positions ==
                                    state->generated->positions,
                                "Scene reopen lost generated geometry");
                        require(std::abs(
                                    object(window.document(), "blue_cube").transform.translation.x -
                                    state->saved_x - 0.15) < 1e-6,
                                "Scene save discarded edits made while writing");
                        require(window.windowTitle().contains('*'),
                                "Unsaved edits during save were not indicated");
                    });
                    window.export_geometry_file(
                        QString::fromStdU16String(state->input_file.u16string()));
                    advance(Stage::guard_export);
                    return;
                case Stage::guard_export:
                    if (!finished(window)) {
                        return;
                    }
                    require(status.state == GeometryTaskState::failed &&
                                bytes(state->input_file) == state->original_input,
                            "Geometry export overwrote an input asset");
                    window.load_scene_file(directory + "/start.json");
                    window.select_object("blue_cube");
                    state->source = mesh_for(window.document(), "blue_cube");
                    advance(Stage::cancel_start);
                    return;
                case Stage::cancel_start:
                    if (!preview_ready()) {
                        return;
                    }
                    generate(window, 8, false);
                    state->cancel_sent = false;
                    advance(Stage::cancel_wait);
                    return;
                case Stage::cancel_wait:
                    if (!state->cancel_sent) {
                        require(tasks->busy(),
                                "Cancellation fixture finished before a cancellation request");
                        if (status.elapsed_ms < 100) {
                            return;
                        }
                        control<QPushButton>(window, "geometry_cancel")->click();
                        state->cancel_at = state->clock.elapsed();
                        state->cancel_sent = true;
                        return;
                    }
                    if (!finished(window)) {
                        require(state->clock.elapsed() - state->cancel_at < 2000,
                                "Geometry cancellation exceeded two seconds");
                        return;
                    }
                    require(status.state == GeometryTaskState::cancelled &&
                                mesh_for(window.document(), "blue_cube") == state->source,
                            "Geometry cancellation published a partial result");
                    generate(window, 7, false);
                    control<QAction>(window, "delete")->trigger();
                    advance(Stage::discard_wait);
                    return;
                case Stage::discard_wait:
                    if (!finished(window)) {
                        return;
                    }
                    require(status.state == GeometryTaskState::discarded &&
                                window.document().meshes.size() == state->original_meshes &&
                                window.document().objects.size() + 1 == state->original_objects,
                            "A result for a deleted target was applied");
                    window.load_scene_file(directory + "/start.json");
                    window.select_object("blue_cube");
                    generate(window, 7, false);
                    window.load_scene_file(directory + "/start.json");
                    window.select_object("blue_cube");
                    state->source = mesh_for(window.document(), "blue_cube");
                    advance(Stage::load_cancel_wait);
                    return;
                case Stage::load_cancel_wait:
                    if (!finished(window)) {
                        return;
                    }
                    require(status.state == GeometryTaskState::cancelled &&
                                mesh_for(window.document(), "blue_cube") == state->source,
                            "Changing scenes accepted an old geometry result");
                    generate(window, 8, false);
                    advance(Stage::large_apply);
                    return;
                case Stage::large_apply:
                    // 读取同一轮 UI 刷新的进度条与计数；worker 可能已经进入下一个阶段。
                    // 将最新 worker 阶段与上一次 UI 刷新混用，会漏掉短阶段的真实显示。
                    {
                        const auto* bar = control<QProgressBar>(window, "geometry_progress");
                        const auto* counts = control<QLabel>(window, "geometry_counts");
                        state->numeric_progress =
                            state->numeric_progress ||
                            (bar->maximum() == 1000 && bar->value() > 0 && bar->value() < 1000 &&
                             counts->text().contains('/'));
                    }
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    state->large = mesh_for(window.document(), "blue_cube");
                    require(state->large->triangles.size() == 786432 && state->numeric_progress,
                            "Large geometry task did not show actual numeric progress");
                    advance(Stage::large_upload);
                    return;
                case Stage::large_upload:
                    if (!preview_ready()) {
                        return;
                    }
                    qInfo() << "Geometry large GPU scene update ms"
                            << viewport->upload_statistics().last_update_ms;
                    verify(*state, [&] {
                        verify_gpu(window, directory, "large");
                        control<QSpinBox>(window, "geometry_levels")->setValue(0);
                        verify_layout(window);
                        capture_ui(window, directory + "/geometry-ui.png");
                    });
                    control<QSpinBox>(window, "geometry_levels")->setValue(8);
                    require(!control<QPushButton>(window, "geometry_apply")->isEnabled(),
                            "Geometry budget did not disable an oversized request");
                    window.save_scene_async(directory + "/cancelled.json");
                    state->cancel_sent = false;
                    advance(Stage::save_cancel_start);
                    return;
                case Stage::save_cancel_start:
                    require(tasks->busy(), "Scene cancellation fixture completed too soon");
                    if (status.progress.stage.find("写出生成网格") == std::string::npos ||
                        status.progress.completed < 1024) {
                        return;
                    }
                    tasks->cancel();
                    state->cancel_at = state->clock.elapsed();
                    advance(Stage::save_cancel_wait);
                    return;
                case Stage::save_cancel_wait:
                    if (!finished(window)) {
                        require(state->clock.elapsed() - state->cancel_at < 2000,
                                "Scene save cancellation exceeded two seconds");
                        return;
                    }
                    require(status.state == GeometryTaskState::cancelled &&
                                bytes(native(directory + "/cancelled.json")) ==
                                    "preserve existing scene bytes\n",
                            "Cancelled scene save replaced its destination");
                    window.export_geometry_file(directory + "/large.obj");
                    advance(Stage::export_large);
                    return;
                case Stage::export_large:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    window.import_object_file(directory + "/large.obj");
                    advance(Stage::import_large);
                    return;
                case Stage::import_large:
                    if (window.asset_loading() || !preview_ready()) {
                        return;
                    }
                    qInfo() << "Geometry imported GPU scene update ms"
                            << viewport->upload_statistics().last_update_ms;
                    require(window.document().objects.size() == state->original_objects + 1,
                            "Large geometry OBJ import did not create an object");
                    require(window.document().meshes.back().data->positions ==
                                state->large->positions,
                            "Large geometry OBJ import changed shared positions");
                    window.load_scene_file(directory + "/保存 场景.json");
                    window.select_object("blue_cube");
                    control<QComboBox>(window, "geometry_operation")->setCurrentIndex(1);
                    control<QSpinBox>(window, "geometry_target_faces")->setValue(400);
                    advance(Stage::qem_start);
                    return;
                case Stage::qem_start:
                    if (!preview_ready() ||
                        window.ibl_controller()->status().state != IblTaskState::ready) {
                        return;
                    }
                    state->generated = mesh_for(window.document(), "blue_cube");
                    state->source = mesh_for(window.document(), "metal_cube");
                    verify(*state, [&] {
                        verify_layout(window);
                        state->qem_before = verify_gpu(window, directory, "qem-before");
                    });
                    state->snapshot = minirender::make_snapshot(window.document());
                    window.offline_panel()->set_settings(offline_settings(), integrator(),
                                                         {2, 4, 2});
                    control<QPushButton>(window, "offline_start")->click();
                    require(control<QPushButton>(window, "geometry_apply")->isEnabled(),
                            "QEM apply control is disabled");
                    control<QPushButton>(window, "geometry_apply")->click();
                    require(tasks->busy(), "QEM did not start a background job");
                    control<QSpinBox>(window, "geometry_target_faces")->setValue(300);
                    control<QDoubleSpinBox>(window, "roughness")->setValue(0.28);
                    advance(Stage::qem_apply);
                    return;
                case Stage::qem_apply:
                    if (!finished(window) || !offline_done() || !preview_ready()) {
                        return;
                    }
                    ready(window);
                    state->simplified = mesh_for(window.document(), "blue_cube");
                    require(state->simplified != state->generated &&
                                state->simplified->triangles.size() <= 400,
                            "QEM did not publish the requested simplification");
                    require(mesh_for(window.document(), "metal_cube") == state->source,
                            "QEM changed another shared asset");
                    require(std::abs(control<QDoubleSpinBox>(window, "roughness")->value() - 0.28) <
                                1e-6,
                            "QEM overwrote an unrelated edit");
                    require(
                        control<QLabel>(window, "geometry_metrics")->text().contains("目标 400"),
                        "QEM did not preserve its start-time settings");
                    require(window.offline_panel()->stale(),
                            "QEM did not mark an old render stale");
                    verify(*state, [&] {
                        verify_offline(window, state->snapshot);
                        control<QCheckBox>(window, "offline_show")->setChecked(false);
                        state->qem_after = verify_gpu(window, directory, "qem-after");
                        require(changed_pixels(state->qem_before, state->qem_after) > 30,
                                "QEM GPU geometry remained unchanged");
                        require(QDir().mkpath(directory + "/qem-shadow"),
                                "Cannot create the QEM shadow output directory");
                        verify_shadows(window, directory + "/qem-shadow");
                        capture_ui(window, directory + "/qem-ui.png");
                    });
                    state->snapshot = minirender::make_snapshot(window.document());
                    control<QPushButton>(window, "offline_start")->click();
                    advance(Stage::qem_offline);
                    return;
                case Stage::qem_offline:
                    if (!offline_done()) {
                        return;
                    }
                    verify(*state, [&] { verify_offline(window, state->snapshot); });
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    control<QAction>(window, "undo")->trigger();
                    require(mesh_for(window.document(), "blue_cube") == state->generated,
                            "QEM undo did not restore its input");
                    advance(Stage::qem_undo);
                    return;
                case Stage::qem_undo:
                    if (!preview_ready()) {
                        return;
                    }
                    verify(*state, [&] {
                        require(changed_pixels(state->qem_before,
                                               verify_gpu(window, directory, "qem-undo")) == 0,
                                "QEM GPU undo image differs from its input");
                    });
                    control<QAction>(window, "redo")->trigger();
                    require(mesh_for(window.document(), "blue_cube") == state->simplified,
                            "QEM redo did not restore its output");
                    advance(Stage::qem_redo);
                    return;
                case Stage::qem_redo:
                    if (!preview_ready()) {
                        return;
                    }
                    verify(*state, [&] {
                        require(changed_pixels(state->qem_after,
                                               verify_gpu(window, directory, "qem-redo")) == 0,
                                "QEM GPU redo image differs from its output");
                    });
                    control<QSpinBox>(window, "geometry_target_faces")->setValue(100);
                    control<QDoubleSpinBox>(window, "geometry_quadric_error")->setValue(0);
                    control<QCheckBox>(window, "geometry_limit_error")->setChecked(true);
                    window.apply_geometry();
                    advance(Stage::qem_limited);
                    return;
                case Stage::qem_limited:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    require(control<QLabel>(window, "geometry_metrics")
                                ->text()
                                .contains("已达 QEM 代价上限"),
                            "QEM error-limit reason is missing from the panel");
                    state->simplified = mesh_for(window.document(), "blue_cube");
                    require(state->simplified->triangles.size() > 100,
                            "QEM ignored its error limit");
                    window.select_object("metal_cube");
                    control<QSpinBox>(window, "geometry_target_faces")->setValue(1);
                    control<QCheckBox>(window, "geometry_limit_error")->setChecked(false);
                    window.apply_geometry();
                    advance(Stage::qem_constrained);
                    return;
                case Stage::qem_constrained:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    require(mesh_for(window.document(), "metal_cube") == state->source,
                            "QEM constraint-only result changed a protected asset");
                    require(control<QLabel>(window, "geometry_metrics")
                                ->text()
                                .contains("受拓扑或属性约束"),
                            "QEM constraint reason is missing from the panel");
                    window.select_object("blue_cube");
                    window.export_geometry_file(directory + "/简化 模型.obj");
                    advance(Stage::qem_export);
                    return;
                case Stage::qem_export:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    verify(*state, [&] {
                        const auto loaded =
                            minirender::load_obj(native(directory + "/简化 模型.obj"));
                        require(loaded.positions == state->simplified->positions &&
                                    loaded.triangles.size() == state->simplified->triangles.size(),
                                "QEM OBJ round trip changed the simplified geometry");
                    });
                    window.save_scene_async(directory + "/简化 场景.json");
                    advance(Stage::qem_save);
                    return;
                case Stage::qem_save:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    verify(*state, [&] {
                        const auto loaded =
                            minirender::load_scene(native(directory + "/简化 场景.json"));
                        require(mesh_for(loaded, "blue_cube")->positions ==
                                    state->simplified->positions,
                                "QEM saved scene lost its generated asset");
                    });
                    state->remesh_input = mesh_for(window.document(), "blue_cube");
                    state->snapshot = minirender::make_snapshot(window.document());
                    verify(*state, [&] {
                        state->remesh_before = verify_gpu(window, directory, "remesh-before");
                    });
                    control<QPushButton>(window, "offline_start")->click();
                    control<QComboBox>(window, "geometry_operation")->setCurrentIndex(2);
                    control<QDoubleSpinBox>(window, "geometry_remesh_length")->setValue(0.1);
                    control<QSpinBox>(window, "geometry_remesh_iterations")->setValue(3);
                    state->remesh_settings = *window.geometry_panel()->parameters().remesh;
                    require(control<QPushButton>(window, "geometry_apply")->isEnabled(),
                            "Remeshing apply control is disabled");
                    control<QPushButton>(window, "geometry_apply")->click();
                    control<QDoubleSpinBox>(window, "geometry_remesh_length")->setValue(0.2);
                    control<QDoubleSpinBox>(window, "roughness")->setValue(0.24);
                    advance(Stage::remesh_apply);
                    return;
                case Stage::remesh_apply:
                    if (!finished(window) || !offline_done() || !preview_ready()) {
                        return;
                    }
                    ready(window);
                    state->remeshed = mesh_for(window.document(), "blue_cube");
                    require(state->remeshed != state->remesh_input,
                            "Remeshing did not publish a new asset");
                    require(mesh_for(window.document(), "metal_cube") == state->source,
                            "Remeshing changed another instance");
                    require(std::abs(control<QDoubleSpinBox>(window, "roughness")->value() - 0.24) <
                                1e-6,
                            "Remeshing overwrote an unrelated material edit");
                    require(control<QLabel>(window, "geometry_metrics")
                                ->text()
                                .contains("平均角度偏差"),
                            "Remeshing quality report is missing");
                    require(control<QLabel>(window, "geometry_metrics")
                                ->text()
                                .contains("本次边长 0.1 m"),
                            "Remeshing report did not retain the applied edge length");
                    verify(*state, [&] {
                        const auto expected = minirender::remesh_isotropic(state->remesh_input,
                                                                           state->remesh_settings);
                        require(expected.mesh->positions == state->remeshed->positions &&
                                    expected.mesh->triangles.size() ==
                                        state->remeshed->triangles.size(),
                                "Remeshing did not preserve its start-time settings");
                        verify_offline(window, state->snapshot);
                        require(window.offline_panel()->stale(),
                                "Remeshing did not mark an old offline snapshot stale");
                        control<QCheckBox>(window, "offline_show")->setChecked(false);
                        state->remesh_after = verify_gpu(window, directory, "remeshed");
                        require(QDir().mkpath(directory + "/remesh-shadow"),
                                "Cannot create remeshing shadow directory");
                        verify_shadows(window, directory + "/remesh-shadow");
                        control<QComboBox>(window, "view_mode")->setCurrentIndex(3);
                        require(viewport->grab().save(directory + "/remeshed-wire.png"),
                                "Cannot save remeshing wireframe");
                        control<QComboBox>(window, "view_mode")->setCurrentIndex(0);
                        verify_layout(window);
                        capture_ui(window, directory + "/remesh-ui.png");
                    });
                    state->snapshot = minirender::make_snapshot(window.document());
                    control<QPushButton>(window, "offline_start")->click();
                    advance(Stage::remesh_offline);
                    return;
                case Stage::remesh_offline:
                    if (!offline_done()) {
                        return;
                    }
                    verify(*state, [&] { verify_offline(window, state->snapshot); });
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    control<QAction>(window, "undo")->trigger();
                    require(mesh_for(window.document(), "blue_cube") == state->remesh_input,
                            "Remeshing undo did not restore its input");
                    advance(Stage::remesh_undo);
                    return;
                case Stage::remesh_undo:
                    if (!preview_ready()) {
                        return;
                    }
                    verify(*state, [&] {
                        require(changed_pixels(state->remesh_before,
                                               verify_gpu(window, directory, "remesh-undo")) == 0,
                                "Remeshing GPU undo pixels changed");
                    });
                    control<QAction>(window, "redo")->trigger();
                    require(mesh_for(window.document(), "blue_cube") == state->remeshed,
                            "Remeshing redo did not restore its output");
                    advance(Stage::remesh_redo);
                    return;
                case Stage::remesh_redo:
                    if (!preview_ready()) {
                        return;
                    }
                    verify(*state, [&] {
                        require(changed_pixels(state->remesh_after,
                                               verify_gpu(window, directory, "remesh-redo")) == 0,
                                "Remeshing GPU redo pixels changed");
                    });
                    window.export_geometry_file(directory + "/重网格 模型.obj");
                    advance(Stage::remesh_export);
                    return;
                case Stage::remesh_export:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    verify(*state, [&] {
                        const auto loaded =
                            minirender::load_obj(native(directory + "/重网格 模型.obj"));
                        require(loaded.positions == state->remeshed->positions &&
                                    loaded.triangles.size() == state->remeshed->triangles.size(),
                                "Remeshing OBJ round trip changed geometry");
                    });
                    window.save_scene_async(directory + "/重网格 场景.json");
                    advance(Stage::remesh_save);
                    return;
                case Stage::remesh_save:
                    if (!finished(window)) {
                        return;
                    }
                    ready(window);
                    verify(*state, [&] {
                        const auto saved =
                            minirender::load_scene(native(directory + "/重网格 场景.json"));
                        require(mesh_for(saved, "blue_cube")->positions ==
                                    state->remeshed->positions,
                                "Remeshing scene save lost its asset");
                    });
                    {
                        const auto faces = static_cast<int>(state->remeshed->triangles.size());
                        control<QSpinBox>(window, "geometry_remesh_budget")->setValue(faces - 1);
                        require(!control<QPushButton>(window, "geometry_apply")->isEnabled(),
                                "Remeshing input budget did not disable apply");
                        control<QSpinBox>(window, "geometry_remesh_budget")->setValue(faces);
                    }
                    control<QDoubleSpinBox>(window, "geometry_remesh_length")->setValue(0.001);
                    window.apply_geometry();
                    advance(Stage::remesh_budget_wait);
                    return;
                case Stage::remesh_budget_wait:
                    if (!finished(window)) {
                        return;
                    }
                    require(status.state == GeometryTaskState::failed &&
                                mesh_for(window.document(), "blue_cube") == state->remeshed,
                            "Failed remeshing published a partial mesh");
                    control<QComboBox>(window, "geometry_operation")->setCurrentIndex(1);
                    // 重新导入压力网格，在候选边计算阶段检查取消。
                    window.import_object_file(directory + "/large.obj");
                    advance(Stage::qem_cancel_start);
                    return;
                case Stage::qem_cancel_start:
                    if (window.asset_loading()) {
                        return;
                    }
                    control<QSpinBox>(window, "geometry_target_faces")->setValue(10000);
                    window.apply_geometry();
                    advance(Stage::qem_cancel_phase);
                    return;
                case Stage::qem_cancel_phase:
                    require(tasks->busy(),
                            "QEM cancellation fixture finished before its candidate phase");
                    if (status.progress.stage.find("计算候选边") == std::string::npos ||
                        status.progress.completed == 0) {
                        return;
                    }
                    state->cancel_at = state->clock.elapsed();
                    tasks->cancel();
                    advance(Stage::qem_cancel_wait);
                    return;
                case Stage::qem_cancel_wait:
                    if (!finished(window)) {
                        return;
                    }
                    require(status.state == GeometryTaskState::cancelled,
                            "QEM cancel was not reported");
                    require(state->clock.elapsed() - state->cancel_at < 2000,
                            "QEM cancellation exceeded two seconds");
                    require(mesh_for(window.document(), "blue_cube") == state->remeshed,
                            "Cancelled QEM changed an existing asset");
                    state->remesh_input =
                        mesh_for(window.document(), window.document().objects.back().id);
                    control<QComboBox>(window, "geometry_operation")->setCurrentIndex(2);
                    control<QSpinBox>(window, "geometry_remesh_budget")->setValue(1000000);
                    control<QDoubleSpinBox>(window, "geometry_remesh_length")->setValue(0.0005);
                    window.apply_geometry();
                    advance(Stage::remesh_cancel_phase);
                    return;
                case Stage::remesh_cancel_phase:
                    require(tasks->busy(), "Remeshing cancellation fixture finished too early");
                    if (status.progress.stage.find("建立边候选") == std::string::npos ||
                        status.progress.completed == 0) {
                        return;
                    }
                    state->cancel_at = state->clock.elapsed();
                    control<QPushButton>(window, "geometry_cancel")->click();
                    advance(Stage::remesh_cancel_wait);
                    return;
                case Stage::remesh_cancel_wait: {
                    if (!finished(window)) {
                        return;
                    }
                    require(status.state == GeometryTaskState::cancelled,
                            "Remeshing cancel was not reported");
                    require(state->clock.elapsed() - state->cancel_at < 2000,
                            "Remeshing cancellation exceeded two seconds");
                    require(mesh_for(window.document(), window.document().objects.back().id) ==
                                state->remesh_input,
                            "Cancelled remeshing changed its input asset");
                    QElapsedTimer restore_timer;
                    restore_timer.start();
                    window.load_scene_file(directory + "/start.json");
                    window.select_object("blue_cube");
                    generate(window, 8, false);
                    qInfo() << "Geometry restore before shutdown ms" << restore_timer.elapsed();
                    advance(Stage::shutdown);
                    return;
                }
                case Stage::shutdown:
                    require(tasks->busy(), "Geometry shutdown requires an active worker");
                    QCoreApplication::sendPostedEvents(state->probe_receiver,
                                                       state->probe_event_type);
                    record_gap(*state);
                    if (measure_responsiveness) {
                        qInfo() << "Geometry response samples" << state->probe_samples
                                << "maximum ms" << state->maximum_event_latency;
                        require(state->probe_samples >= 10,
                                "Geometry response probe collected too few events");
                        require(state->maximum_event_latency < 500,
                                "Geometry UI event latency exceeded 500 milliseconds");
                    }
                    qInfo() << "Geometry smoke PASS: controls, shared assets, undo/redo, CPU/GPU "
                               "geometry, "
                               "snapshot save, OBJ import/export, cancellation and stale results; "
                               "QEM/remeshing controls, measured quality/error and history; event "
                               "latency ms"
                            << state->maximum_event_latency << "heartbeat ms" << state->maximum_gap;
                    timer->stop();
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                qCritical() << "Geometry smoke FAIL at stage" << static_cast<int>(state->stage)
                            << ':' << error.what();
                timer->stop();
                QApplication::exit(1);
            }
        });
    timer->start();
}
