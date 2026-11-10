#include "point_cloud_editor_smoke.hpp"
#include "geometry_smoke.hpp"
#include "offline_panel.hpp"
#include "point_cloud_panel.hpp"
#include "point_cloud_smoke.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/point_io.hpp>
#include <minirender/assets/scene_io.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* value = window.findChild<T*>(name);
    require(value, "Point editor control is missing");
    return value;
}
std::filesystem::path native(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
const minirender::SceneObject& object_for(const minirender::SceneDocument& scene,
                                          const std::string& id) {
    const auto found = std::find_if(scene.objects.begin(), scene.objects.end(),
                                    [&](const auto& o) { return o.id == id; });
    require(found != scene.objects.end(), "Point editor scene object is missing");
    return *found;
}
std::shared_ptr<const minirender::MeshData> mesh_for(const minirender::SceneDocument& scene,
                                                     const std::string& id) {
    const auto& object = object_for(scene, id);
    const auto found = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                    [&](const auto& mesh) { return mesh.id == object.mesh_id; });
    require(found != scene.meshes.end(), "Point editor scene mesh is missing");
    return found->data;
}
minirender::PointCloud sphere() {
    minirender::PointCloud result;
    constexpr std::size_t count = 1200;
    for (std::size_t i = 0; i < count; ++i) {
        const double z = 1 - 2 * (static_cast<double>(i) + .5) / count;
        const double angle = static_cast<double>(i) * std::numbers::pi * (3 - std::sqrt(5.0));
        const double r = std::sqrt(1 - z * z);
        result.positions.emplace_back(.1 * r * std::cos(angle), .1 * r * std::sin(angle), .1 * z);
    }
    return result;
}
void mouse(VulkanViewport& viewport, QEvent::Type type, QPointF p, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, p, viewport.mapToGlobal(p), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&viewport, &event);
}

// 真实按下/移动/释放重复驱动相机，核对工具导航与主场景编辑的隔离。
void drag_points(Workbench& window) {
    auto* viewport = window.viewport();
    const auto scene = window.document().camera;
    for (int i = 0; i < 6; ++i) {
        const auto before = viewport->camera();
        const QPointF origin(viewport->width() * .5, viewport->height() * .5);
        mouse(*viewport, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        mouse(*viewport, QEvent::MouseMove, origin + QPointF(22, 8), Qt::NoButton, Qt::LeftButton);
        mouse(*viewport, QEvent::MouseButtonRelease, origin + QPointF(22, 8), Qt::LeftButton,
              Qt::NoButton);
        require(viewport->camera().position != before.position, "Point camera drag became stuck");
        const auto stopped = viewport->camera();
        mouse(*viewport, QEvent::MouseMove, origin + QPointF(44, 16), Qt::NoButton, Qt::NoButton);
        require(viewport->camera().position == stopped.position,
                "Point camera moved after release");
    }
    require(window.document().camera.position == scene.position &&
                window.document().camera.target == scene.target,
            "Point camera gestures changed the scene camera");
}
void equal_mesh(const minirender::MeshData& a, const minirender::MeshData& b) {
    require(a.positions == b.positions && a.triangles.size() == b.triangles.size(),
            "Point mesh differs from fixed core input");
    for (std::size_t i = 0; i < a.triangles.size(); ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            require(a.triangles[i].corners[c].position == b.triangles[i].corners[c].position,
                    "Point triangle connectivity differs from core");
        }
    }
}
void verify_offline(Workbench& window,
                    const std::shared_ptr<const minirender::SceneSnapshot>& snapshot) {
    require(window.offline_panel()->session().progress().state ==
                minirender::RenderState::completed,
            "Point editor offline task failed");
    const auto actual = window.offline_panel()->session().preview().image;
    const auto expected =
        minirender::PathTracer(snapshot, {.maximum_depth = 5}).render({24, 24, 8, 101});
    require(actual && actual->width() == 24 && actual->height() == 24,
            "Point editor offline preview is missing");
    for (std::size_t y = 0; y < 24; ++y) {
        for (std::size_t x = 0; x < 24; ++x) {
            require(glm::length(actual->at(x, y) - expected.at(x, y)) < 1e-6F,
                    "Point editor offline image differs from serial snapshot reference");
        }
    }
}

// 检查停靠页实际布局、字段重叠和底部可达性，尺寸变化保持窗口隐藏。
void verify_layout(Workbench& window, const QString& directory) {
    auto* tabs = control<QTabWidget>(window, "inspector_tabs");
    tabs->setCurrentIndex(7);
    auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
    auto* panel = window.point_cloud_panel();
    require(area, "Point panel is not inside a scroll area");
    for (const auto size : {QSize(1200, 800), QSize(960, 700)}) {
        window.resize(size);
        window.layout()->activate();
        if (auto* layout = area->layout()) {
            layout->activate();
        }
        panel->layout()->activate();
        capture_workbench_ui(window,
                             directory + QString("/points-layout-%1.png").arg(size.width()));
        auto* form = qobject_cast<QFormLayout*>(panel->layout());
        require(form, "Point panel form layout is missing");
        std::vector<QWidget*> fields;
        QWidget* bottom = nullptr;
        for (int i = 0; i < form->count(); ++i) {
            auto* field = form->itemAt(i)->widget();
            if (!field || !field->isVisibleTo(panel) || field->height() == 0) {
                continue;
            }
            const auto rect = field->geometry();
            require(rect.left() >= 0 && rect.right() < panel->width(),
                    "Point field exceeds panel bounds");
            for (auto* other : fields) {
                const auto overlap = rect.intersected(other->geometry());
                if (overlap.width() > 1 && overlap.height() > 1) {
                    qInfo() << "Point layout overlap" << field->objectName()
                            << field->metaObject()->className() << rect << other->objectName()
                            << other->metaObject()->className() << other->geometry() << "panel"
                            << panel->size();
                }
                require(overlap.width() <= 1 || overlap.height() <= 1,
                        "Point panel controls overlap");
            }
            fields.push_back(field);
            if (!bottom || rect.bottom() > bottom->geometry().bottom()) {
                bottom = field;
            }
        }
        require(fields.size() >= 30 && bottom, "Point panel controls are missing");
        area->ensureWidgetVisible(bottom);
        require(area->viewport()->rect().contains(
                    bottom->mapTo(area->viewport(), bottom->rect().center())),
                "Point panel bottom controls are inaccessible");
    }
}

enum class Stage {
    setup,
    imported,
    prepared,
    display_ready,
    exported,
    protected_path,
    file_generated,
    file_undo,
    file_redo,
    file_updated,
    sampled,
    sample_generated,
    sample_undo,
    sample_redo,
    budget_failed,
    stale_done,
    stale_restored,
    deleted_done,
    delete_restored,
    saved,
    reopened,
    scene_cancelled,
    large_sampled,
    cancel_phase,
    cancel_wait,
    shutdown
};
struct State {
    Stage stage = Stage::setup;
    int ticks = 0;
    int frame = 0;
    std::size_t original_objects = 0;
    std::size_t original_materials = 0;
    std::uint64_t point_uploads = 0;
    minirender::PointCloud fixture;
    PointReconstructionInput input;
    minirender::Transform file_transform;
    minirender::Transform sampled_transform;
    std::shared_ptr<const minirender::PointCloud> raw;
    std::shared_ptr<const minirender::PointCloud> large;
    std::shared_ptr<const PointNormalCache> cache;
    std::shared_ptr<const minirender::MeshData> original;
    std::shared_ptr<const minirender::MeshData> file_mesh;
    std::shared_ptr<const minirender::MeshData> sampled_mesh;
    std::shared_ptr<const minirender::SceneSnapshot> snapshot;
    std::shared_ptr<const minirender::SceneSnapshot> saved_snapshot;
    std::string file_object;
    std::string original_input;
    QString input_path;
    QElapsedTimer cancellation;
};
} // namespace

void start_point_cloud_editor_smoke(Workbench& window, const QString& directory,
                                    bool measure_responsiveness) {
    const auto state = std::make_shared<State>();
    auto* probe = new UiResponseProbe(&window, "Points");
    auto* timer = new QTimer(&window);
    timer->setInterval(60);
    QObject::connect(
        timer, &QTimer::timeout, &window,
        [state, timer, probe, &window, directory, measure_responsiveness] {
            try {
                auto* viewport = window.viewport();
                auto* tasks = window.geometry_controller();
                auto* panel = window.point_cloud_panel();
                const auto status = tasks->status();
                require(++state->ticks < 6000 && viewport->failure().isEmpty(),
                        "Point editor timed out or Vulkan failed");
                if (!viewport->can_render() || viewport->rendered_frames() < 5) {
                    return;
                }
                const bool ready = !tasks->busy() && !window.asset_loading() &&
                                   viewport->rendered_frames() > state->frame + 2 &&
                                   viewport->preview_wait_ms() == 0;
                const auto advance = [&](Stage stage) {
                    qInfo() << "Point editor stage" << static_cast<int>(state->stage) << "->"
                            << static_cast<int>(stage);
                    state->stage = stage;
                    state->frame = viewport->rendered_frames();
                };
                const auto offline_done = [&] {
                    const auto progress = window.offline_panel()->session().progress();
                    require(progress.state != minirender::RenderState::failed,
                            "Point editor offline failed");
                    return minirender::render_terminal(progress.state);
                };
                const auto launch_reconstruction = [&] {
                    state->input = panel->reconstruction_parameters();
                    auto* button = control<QPushButton>(window, "points_reconstruct");
                    require(button->isEnabled(), "Point reconstruction button is disabled");
                    button->click();
                    require(tasks->busy() && window.asset_loading(),
                            "Point reconstruction did not start");
                };
                switch (state->stage) {
                case Stage::setup:
                    probe->verify([&] {
                        state->fixture = sphere();
                        state->input_path = directory + "/点云 原输入.ply";
                        minirender::save_point_cloud(state->fixture, native(state->input_path));
                        state->original_input = bytes(native(state->input_path));
                        minirender::save_scene(window.document(),
                                               native(directory + "/start.json"));
                        state->snapshot = minirender::make_snapshot(window.document());
                    });
                    state->original_objects = window.document().objects.size();
                    state->original_materials = window.document().materials.size();
                    state->original = mesh_for(window.document(), "blue_cube");
                    require(mesh_for(window.document(), "metal_cube") == state->original,
                            "Point fixture lost shared mesh input");
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(7);
                    window.offline_panel()->set_settings(
                        {24, 24, 8, 101}, {.maximum_depth = 5},
                        {.threads = 2, .tile_size = 8, .batch_samples = 4});
                    window.offline_panel()->start();
                    window.import_point_file(state->input_path);
                    advance(Stage::imported);
                    return;
                case Stage::imported:
                    if (!ready || !offline_done()) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready, "Point import failed");
                    state->raw = panel->source().cloud;
                    require(state->raw && state->raw->positions == state->fixture.positions,
                            "PLY import changed source points");
                    require(viewport->point_preview_visible() &&
                                !viewport->point_preview_data()->has_normals,
                            "Raw point preview did not open without normals");
                    require(!control<QAction>(window, "export_hdr")->isEnabled(),
                            "Point diagnostic preview enabled scene HDR export");
                    require(window.document().objects.size() == state->original_objects &&
                                !window.offline_panel()->stale(),
                            "Point import changed scene geometry or offline snapshot");
                    probe->verify([&] {
                        verify_offline(window, state->snapshot);
                        drag_points(window);
                    });
                    control<QPushButton>(window, "points_locate")->click();
                    control<QCheckBox>(window, "points_auto_radii")->setChecked(false);
                    control<QLineEdit>(window, "points_radii")->setText("0.04, 0.02");
                    require(!control<QPushButton>(window, "points_reconstruct")->isEnabled(),
                            "Invalid ball radii enabled reconstruction");
                    control<QCheckBox>(window, "points_auto_radii")->setChecked(true);
                    control<QPushButton>(window, "points_prepare")->click();
                    require(tasks->busy(), "Point normal preparation did not start");
                    control<QSpinBox>(window, "points_neighbors")->setValue(12);
                    window.select_object("blue_cube");
                    control<QDoubleSpinBox>(window, "roughness")->setValue(.27);
                    advance(Stage::prepared);
                    return;
                case Stage::prepared:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready,
                            "Point normal preparation failed");
                    state->cache = panel->reconstruction_parameters().cached_normals;
                    require(state->cache && state->cache->settings.neighbors == 20,
                            "Point normal settings were not frozen");
                    require(control<QSpinBox>(window, "points_neighbors")->value() == 12,
                            "Point normal completion overwrote edited controls");
                    require(
                        control<QLabel>(window, "points_metrics")->text().contains("本次邻域 20"),
                        "Point metrics used edited parameters");
                    control<QSpinBox>(window, "points_neighbors")->setValue(20);
                    control<QCheckBox>(window, "points_normal_colors")->setChecked(true);
                    control<QCheckBox>(window, "points_show_boundary")->setChecked(false);
                    control<QDoubleSpinBox>(window, "points_size")->setValue(8);
                    state->point_uploads = viewport->point_uploads();
                    advance(Stage::display_ready);
                    return;
                case Stage::display_ready:
                    if (!ready) {
                        return;
                    }
                    probe->verify([&] {
                        verify_point_cloud_probe(window, directory, "points-prepared-source");
                    });
                    require(viewport->point_uploads() == state->point_uploads,
                            "Point display edit reuploaded data");
                    window.export_point_file(directory + "/已准备 点云.ply");
                    advance(Stage::exported);
                    return;
                case Stage::exported:
                    if (!ready) {
                        return;
                    }
                    probe->verify([&] {
                        const auto loaded =
                            minirender::load_point_cloud(native(directory + "/已准备 点云.ply"));
                        require(loaded.positions == state->cache->value.cloud->positions &&
                                    loaded.normals == state->cache->value.cloud->normals,
                                "Prepared point PLY round trip changed data");
                    });
                    window.export_point_file(state->input_path);
                    advance(Stage::protected_path);
                    return;
                case Stage::protected_path:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::failed &&
                                bytes(native(state->input_path)) == state->original_input,
                            "Point export overwrote its source file");
                    probe->verify(
                        [&] { state->snapshot = minirender::make_snapshot(window.document()); });
                    window.offline_panel()->start();
                    launch_reconstruction();
                    control<QCheckBox>(window, "points_auto_radii")->setChecked(false);
                    control<QLineEdit>(window, "points_radii")->setText("0.04, 0.06");
                    control<QCheckBox>(window, "points_fit_scene")->setChecked(false);
                    control<QDoubleSpinBox>(window, "roughness")->setValue(.29);
                    advance(Stage::file_generated);
                    return;
                case Stage::file_generated:
                    if (!ready || !offline_done()) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready,
                            "Imported point reconstruction failed");
                    state->file_object = panel->target_id();
                    state->file_mesh = mesh_for(window.document(), state->file_object);
                    require(window.document().objects.size() == state->original_objects + 1 &&
                                window.document().materials.size() == state->original_materials + 1,
                            "Imported point reconstruction did not create one independent "
                            "object/material");
                    require(object_for(window.document(), state->file_object).transform.scale.x >
                                    5 &&
                                !control<QCheckBox>(window, "points_fit_scene")->isChecked(),
                            "Point creation did not retain the captured fit setting");
                    require(panel->reconstruction_parameters().cached_normals == state->cache,
                            "Point reconstruction recomputed a matching normal cache");
                    require(window.offline_panel()->stale(),
                            "Point mesh did not stale the old offline snapshot");
                    probe->verify([&] {
                        auto settings = state->input.balls;
                        const auto spacing = state->cache->value.spacing_p90;
                        settings.radii = {2 * spacing, 3 * spacing, 4 * spacing};
                        const auto expected =
                            minirender::reconstruct_ball_pivot(state->cache->value.cloud, settings);
                        equal_mesh(*state->file_mesh, *expected.mesh);
                        verify_offline(window, state->snapshot);
                        verify_geometry_gpu(window, directory, "points-file-mesh",
                                            state->file_object);
                    });
                    control<QAction>(window, "undo")->trigger();
                    advance(Stage::file_undo);
                    return;
                case Stage::file_undo:
                    if (!ready) {
                        return;
                    }
                    require(window.document().objects.size() == state->original_objects,
                            "Point object undo failed");
                    control<QAction>(window, "redo")->trigger();
                    advance(Stage::file_redo);
                    return;
                case Stage::file_redo:
                    if (!ready) {
                        return;
                    }
                    require(mesh_for(window.document(), state->file_object) == state->file_mesh,
                            "Point object redo failed");
                    window.select_object(state->file_object);
                    control<QDoubleSpinBox>(window, "translation.x")->setValue(-3);
                    control<QDoubleSpinBox>(window, "rotation.y")->setValue(17);
                    launch_reconstruction();
                    control<QDoubleSpinBox>(window, "translation.y")
                        ->setValue(object_for(window.document(), state->file_object)
                                       .transform.translation.y +
                                   .1);
                    state->file_transform =
                        object_for(window.document(), state->file_object).transform;
                    advance(Stage::file_updated);
                    return;
                case Stage::file_updated:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready &&
                                window.document().objects.size() == state->original_objects + 1,
                            "Repeated file reconstruction stacked duplicate objects");
                    require(
                        object_for(window.document(), state->file_object).transform.translation ==
                            state->file_transform.translation,
                        "Point reconstruction overwrote an in-flight transform edit");
                    probe->verify([&] {
                        const auto expected = minirender::reconstruct_ball_pivot(
                            state->cache->value.cloud, state->input.balls);
                        equal_mesh(*mesh_for(window.document(), state->file_object),
                                   *expected.mesh);
                    });
                    window.select_object("blue_cube");
                    control<QSpinBox>(window, "points_samples")->setValue(3000);
                    control<QSpinBox>(window, "points_seed")->setValue(77);
                    control<QPushButton>(window, "points_sample")->click();
                    require(tasks->busy(), "Surface point sampling did not start");
                    control<QSpinBox>(window, "points_samples")->setValue(2000);
                    control<QSpinBox>(window, "points_seed")->setValue(78);
                    advance(Stage::sampled);
                    return;
                case Stage::sampled:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready &&
                                panel->source().cloud->positions.size() == 3000,
                            "Point sampler did not freeze count/seed");
                    state->raw = panel->source().cloud;
                    require(panel->source().object_id == "blue_cube",
                            "Point sampling lost its target object");
                    probe->verify([&] {
                        const auto expected =
                            minirender::sample_mesh_surface(*state->original, 3000, 77);
                        require(state->raw->positions == expected.positions &&
                                    state->raw->normals == expected.normals,
                                "Point sampler differs from its captured mesh/seed");
                        verify_point_cloud_probe(window, directory, "points-sampled-source");
                    });
                    control<QCheckBox>(window, "points_estimate")->setChecked(false);
                    control<QCheckBox>(window, "points_orient")->setChecked(false);
                    control<QCheckBox>(window, "points_auto_radii")->setChecked(true);
                    launch_reconstruction();
                    control<QDoubleSpinBox>(window, "translation.x")->setValue(-.6);
                    control<QDoubleSpinBox>(window, "rotation.y")->setValue(15);
                    control<QDoubleSpinBox>(window, "roughness")->setValue(.35);
                    state->sampled_transform = object_for(window.document(), "blue_cube").transform;
                    advance(Stage::sample_generated);
                    return;
                case Stage::sample_generated:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready,
                            "Sampled point reconstruction failed");
                    state->sampled_mesh = mesh_for(window.document(), "blue_cube");
                    require(state->sampled_mesh != state->original &&
                                mesh_for(window.document(), "metal_cube") == state->original,
                            "Sampled point reconstruction changed a shared mesh instance");
                    require(
                        window.document().objects.size() == state->original_objects + 1 &&
                            object_for(window.document(), "blue_cube").transform.translation ==
                                state->sampled_transform.translation &&
                            object_for(window.document(), "blue_cube").transform.rotation_degrees ==
                                state->sampled_transform.rotation_degrees,
                        "Sampled point result did not preserve its target transform");
                    probe->verify([&] {
                        verify_geometry_gpu(window, directory, "points-sampled-mesh");
                        verify_layout(window, directory);
                        capture_workbench_ui(window, directory + "/points-ui.png");
                    });
                    control<QAction>(window, "undo")->trigger();
                    advance(Stage::sample_undo);
                    return;
                case Stage::sample_undo:
                    if (!ready) {
                        return;
                    }
                    require(mesh_for(window.document(), "blue_cube") == state->original,
                            "Point mesh undo did not restore the source");
                    control<QAction>(window, "redo")->trigger();
                    advance(Stage::sample_redo);
                    return;
                case Stage::sample_redo:
                    if (!ready) {
                        return;
                    }
                    require(mesh_for(window.document(), "blue_cube") == state->sampled_mesh,
                            "Point mesh redo did not restore output");
                    control<QSpinBox>(window, "points_work_budget")->setValue(1000);
                    launch_reconstruction();
                    advance(Stage::budget_failed);
                    return;
                case Stage::budget_failed:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::failed &&
                                mesh_for(window.document(), "blue_cube") == state->sampled_mesh,
                            "Point budget failure published a partial mesh");
                    control<QSpinBox>(window, "points_work_budget")->setValue(20000000);
                    launch_reconstruction();
                    control<QAction>(window, "undo")->trigger();
                    advance(Stage::stale_done);
                    return;
                case Stage::stale_done:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::discarded &&
                                mesh_for(window.document(), "blue_cube") == state->original,
                            "Stale point result replaced an undone target mesh");
                    control<QAction>(window, "redo")->trigger();
                    advance(Stage::stale_restored);
                    return;
                case Stage::stale_restored:
                    if (!ready) {
                        return;
                    }
                    require(mesh_for(window.document(), "blue_cube") == state->sampled_mesh,
                            "Stale point job changed redo history");
                    window.select_object("blue_cube");
                    launch_reconstruction();
                    control<QAction>(window, "delete")->trigger();
                    advance(Stage::deleted_done);
                    return;
                case Stage::deleted_done:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::discarded &&
                                std::none_of(window.document().objects.begin(),
                                             window.document().objects.end(),
                                             [](const auto& o) { return o.id == "blue_cube"; }),
                            "Deleted point target was recreated by an old job");
                    control<QAction>(window, "undo")->trigger();
                    advance(Stage::delete_restored);
                    return;
                case Stage::delete_restored:
                    if (!ready) {
                        return;
                    }
                    require(mesh_for(window.document(), "blue_cube") == state->sampled_mesh,
                            "Point target deletion undo failed");
                    probe->verify([&] {
                        state->saved_snapshot = minirender::make_snapshot(window.document());
                    });
                    window.save_scene_async(directory + "/点云 编辑场景.json");
                    advance(Stage::saved);
                    return;
                case Stage::saved:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready, "Point scene save failed");
                    probe->verify(
                        [&] { window.load_scene_file(directory + "/点云 编辑场景.json"); });
                    require(!panel->source().cloud && !viewport->point_preview_visible(),
                            "Scene replacement kept stale point tool data");
                    state->snapshot = minirender::make_snapshot(window.document());
                    window.offline_panel()->start();
                    advance(Stage::reopened);
                    return;
                case Stage::reopened:
                    if (!ready || !offline_done()) {
                        return;
                    }
                    probe->verify([&] {
                        verify_offline(window, state->saved_snapshot);
                        require(bytes(native(state->input_path)) == state->original_input,
                                "Point operations modified the PLY input");
                    });
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    window.select_object("blue_cube");
                    control<QSpinBox>(window, "points_samples")->setValue(500000);
                    control<QPushButton>(window, "points_sample")->click();
                    require(tasks->busy(), "Point scene-switch fixture did not start");
                    probe->verify([&] { window.load_scene_file(directory + "/start.json"); });
                    advance(Stage::scene_cancelled);
                    return;
                case Stage::scene_cancelled:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::cancelled && !panel->source().cloud,
                            "Point sampling survived a scene replacement");
                    window.select_object("blue_cube");
                    control<QPushButton>(window, "points_sample")->click();
                    advance(Stage::large_sampled);
                    return;
                case Stage::large_sampled:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::ready &&
                                panel->source().cloud->positions.size() == 500000,
                            "Large point sample fixture failed");
                    state->large = panel->source().cloud;
                    control<QPushButton>(window, "points_prepare")->click();
                    advance(Stage::cancel_phase);
                    return;
                case Stage::cancel_phase:
                    require(tasks->busy(), "Point cancellation fixture completed too early");
                    if (status.progress.stage.find("准备法线与邻域") == std::string::npos ||
                        status.progress.completed == 0 ||
                        !control<QPushButton>(window, "points_cancel")->isEnabled()) {
                        return;
                    }
                    state->cancellation.start();
                    control<QPushButton>(window, "points_cancel")->click();
                    require(tasks->status().state == GeometryTaskState::cancelling,
                            "Point cancel button did not request cancellation");
                    advance(Stage::cancel_wait);
                    return;
                case Stage::cancel_wait:
                    if (!ready) {
                        return;
                    }
                    require(status.state == GeometryTaskState::cancelled &&
                                panel->source().cloud == state->large,
                            "Cancelled point preparation replaced its source");
                    require(state->cancellation.elapsed() < 2000,
                            "Point cancellation exceeded two seconds");
                    qInfo() << "Point cancellation ms" << state->cancellation.elapsed();
                    control<QPushButton>(window, "points_prepare")->click();
                    require(tasks->busy(), "Point shutdown fixture did not start");
                    advance(Stage::shutdown);
                    return;
                case Stage::shutdown:
                    require(tasks->busy(), "Point shutdown requires an active task");
                    probe->report(measure_responsiveness);
                    timer->stop();
                    qInfo()
                        << "Points smoke PASS: preview, import/sample, cached normals, fixed "
                           "parameters, scene history, CPU/GPU, files, stale jobs and cancellation";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Points editor FAIL stage" << static_cast<int>(state->stage) << ':'
                            << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
