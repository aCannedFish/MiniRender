#include "ssao_smoke.hpp"
#include "workbench.hpp"
#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::filesystem::path path(const QString& value) {
    const auto bytes = value.toUtf8();
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}
minirender::SceneDocument prepare_scene(Workbench& window, const QString& directory) {
    const auto& meshes = window.document().meshes;
    const auto cube = std::find_if(meshes.begin(), meshes.end(),
                                   [](const auto& mesh) { return mesh.id == "cube"; });
    require(cube != meshes.end(), "AO test requires the studio cube asset");
    const auto room = path(directory + "/room.obj");
    std::ofstream(room) << "v -3 0 -2\nv 3 0 -2\nv 3 0 3\nv -3 0 3\n"
                           "f 1 3 2\nf 1 4 3\n"
                           "v -3 0 -2\nv 3 0 -2\nv 3 3 -2\nv -3 3 -2\n"
                           "f 5 6 7\nf 5 7 8\n";
    minirender::SceneDocument scene;
    scene.asset_directory = window.document().asset_directory;
    scene.camera.position = {3, 2.4F, 4.2F};
    scene.camera.target = {0, 0.4F, 0};
    scene.environment = glm::vec3(0.6F);
    scene.materials.push_back({"mat", "mat"});
    scene.materials[0].base_color = glm::vec3(0.5F);
    scene.meshes.push_back(*cube);
    scene.meshes.push_back(
        {"room", room, std::make_shared<const minirender::MeshData>(minirender::load_obj(room))});
    scene.objects.push_back({"room", "room", "room", "mat", {}, true});
    scene.objects.push_back({"cube", "cube", "cube", "mat", {}, true});
    scene.objects[1].transform.translation.y = 0.5F;
    return scene;
}
void install(Workbench& window, const minirender::SceneDocument& scene, const QString& directory) {
    minirender::save_scene(scene, path(directory + "/scene.json"));
    window.load_scene_file(directory + "/scene.json");
}
QPoint project(const VulkanViewport& viewport, glm::vec3 point, QSize size) {
    const auto& camera = viewport.camera();
    const auto clip = camera.projection_matrix(static_cast<float>(size.width()) /
                                               static_cast<float>(size.height())) *
                      camera.view_matrix() * glm::vec4(point, 1);
    require(clip.w > 0, "AO probe is behind camera");
    const auto uv = glm::vec2(clip) / clip.w * 0.5F + 0.5F;
    const QPoint pixel(static_cast<int>(uv.x * static_cast<float>(size.width())),
                       static_cast<int>(uv.y * static_cast<float>(size.height())));
    require(pixel.x() > 8 && pixel.x() + 8 < size.width() && pixel.y() > 8 &&
                pixel.y() + 8 < size.height(),
            "AO probe is outside viewport");
    return pixel;
}
double average(const QImage& image, QPoint center) {
    double sum = 0;
    for (int y = -3; y <= 3; ++y) {
        for (int x = -3; x <= 3; ++x) {
            sum += image.pixelColor(center + QPoint(x, y)).redF();
        }
    }
    return sum / 49;
}
double variation(const QImage& image, QPoint center) {
    double sum = 0;
    for (int y = -5; y <= 5; ++y) {
        for (int x = -5; x <= 5; ++x) {
            const double value = image.pixelColor(center + QPoint(x, y)).redF();
            const double right = image.pixelColor(center + QPoint(x + 1, y)).redF();
            const double below = image.pixelColor(center + QPoint(x, y + 1)).redF();
            sum += (value - right) * (value - right) + (value - below) * (value - below);
        }
    }
    return sum;
}
void equal_hdr(const minirender::Image& first, const minirender::Image& second) {
    require(first.width() == second.width() && first.height() == second.height(),
            "AO HDR dimensions differ");
    float error = 0;
    for (std::size_t y = 0; y < first.height(); ++y) {
        for (std::size_t x = 0; x < first.width(); ++x) {
            error = std::max(error, glm::length(first.at(x, y) - second.at(x, y)));
        }
    }
    qInfo() << "AO isolated HDR max difference:" << error;
    require(error < 1e-6F, "AO changed an unoccluded lighting component");
}
// 直接查询场景几何，不复刻 shader 的深度比较。
double cpu_visibility(std::shared_ptr<const minirender::SceneSnapshot> scene, glm::dvec3 point) {
    minirender::SceneBvh bvh(std::move(scene));
    minirender::Sampler random(702);
    int open = 0;
    for (int i = 0; i < 2048; ++i) {
        const auto local = minirender::sample_cosine_hemisphere(random.next_2d());
        const glm::dvec3 direction{local.x, local.z, local.y};
        open += !bvh.occluded({point, direction}, {0.001, 0.9});
    }
    return static_cast<double>(open) / 2048;
}
void check_composite(const minirender::Image& baseline, const minirender::Image& hdr,
                     const QImage& ao) {
    qInfo() << "AO dimensions baseline/HDR/mask" << baseline.width() << baseline.height()
            << hdr.width() << hdr.height() << ao.size();
    require(baseline.width() == hdr.width() && baseline.height() == hdr.height(),
            "AO baseline dimensions differ");
    require(static_cast<int>(hdr.width()) == ao.width() &&
                static_cast<int>(hdr.height()) == ao.height(),
            "AO and HDR dimensions differ");
    std::size_t geometry = 0, darkened = 0, background = 0;
    float error = 0;
    for (int y = 0; y < ao.height(); ++y) {
        for (int x = 0; x < ao.width(); ++x) {
            const auto original =
                baseline.at(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
            if (std::abs(original.x - 0.3F) < 1e-5F) {
                const float factor = static_cast<float>(ao.pixelColor(x, y).redF());
                const auto actual =
                    hdr.at(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
                error = std::max(error, glm::length(actual - original * factor));
                ++geometry;
                darkened += factor < 0.95F;
            } else {
                if (ao.pixelColor(x, y).red() != 255) {
                    qInfo() << "AO background probe" << x << y << original.x << original.y
                            << original.z << "AO" << ao.pixelColor(x, y).red();
                }
                require(ao.pixelColor(x, y).red() == 255, "AO leaked into background");
                ++background;
            }
        }
    }
    qInfo() << "AO composition geometry" << geometry << "darkened" << darkened << "background"
            << background << "max HDR error" << error;
    require(geometry > 1000 && darkened > geometry / 1000 && background > 0 && error < 0.0021F,
            "AO composition disagrees with the recorded mask");
}
struct State {
    int step = 0, ticks = 0, generation = 0, drag = 0, frame = 0;
    minirender::Camera camera;
    QSize previous_extent, stable_size;
    int stable_frames = 0;
    minirender::SceneDocument scene;
    std::optional<minirender::Image> baseline;
    QImage raw;
    SceneUploadStatistics uploads;
};

bool stable_extent(const VulkanViewport& viewport, State& state) {
    const auto extent = viewport.swapChainImageSize();
    const QSize requested(static_cast<int>(viewport.width() * viewport.devicePixelRatio()),
                          static_cast<int>(viewport.height() * viewport.devicePixelRatio()));
    if (extent != requested) {
        state.stable_frames = 0;
        return false;
    }
    if (extent != state.stable_size) {
        state.stable_size = extent;
        state.stable_frames = 0;
    }
    return ++state.stable_frames >= 2;
}
} // namespace

void start_ssao_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create AO output directory");
    const auto state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(180);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            auto* viewport = window.viewport();
            require(++state->ticks < 160 && viewport->failure().isEmpty(),
                    "SSAO test timed out or renderer failed");
            if (viewport->rendered_frames() < 5 || !viewport->isExposed()) {
                window.raise();
                return;
            }
            auto* mode = window.findChild<QComboBox*>("view_mode");
            auto* enabled = window.findChild<QCheckBox*>("ssao_enabled");
            const auto capture = [&](const char* name) {
                const auto image = viewport->grab();
                require(!image.isNull() && image.save(directory + "/" + name + ".png"),
                        "AO capture failed");
                return image;
            };
            switch (state->step++) {
            case 0:
                state->scene = prepare_scene(window, directory);
                install(window, state->scene, directory);
                viewport->set_shadows_enabled(false);
                viewport->set_fxaa_enabled(false);
                window.findChild<QTabWidget*>("inspector_tabs")->setCurrentIndex(4);
                break;
            case 1:
                // Qt/Cocoa 的布局与原生 surface 尺寸可能分多次更新，先等待实际像素尺寸稳定。
                if (!stable_extent(*viewport, *state)) {
                    --state->step;
                    return;
                }
                state->baseline = viewport->grab_hdr();
                state->camera = viewport->camera();
                state->uploads = viewport->upload_statistics();
                state->generation = viewport->resource_generations();
                state->baseline->write_exr(path(directory + "/baseline.exr"));
                minirender::display_image(*state->baseline)
                    .write_png(path(directory + "/baseline-hdr.png"));
                capture("baseline");
                break;
            case 2: {
                const auto& camera = viewport->camera();
                qInfo() << "AO camera baseline/current" << state->camera.position.x
                        << state->camera.position.y << state->camera.position.z << camera.position.x
                        << camera.position.y << camera.position.z << "fov"
                        << state->camera.vertical_fov_degrees << camera.vertical_fov_degrees;
                require(camera.position == state->camera.position &&
                            camera.target == state->camera.target &&
                            camera.up == state->camera.up &&
                            camera.vertical_fov_degrees == state->camera.vertical_fov_degrees,
                        "AO camera changed between baseline and sampling");
                window.findChild<QTabWidget*>("inspector_tabs")->setCurrentIndex(4);
                window.findChild<QDoubleSpinBox*>("ssao_radius")->setValue(0.9);
                window.findChild<QSpinBox*>("ssao_samples")->setValue(64);
                window.findChild<QCheckBox*>("ssao_filtered")->setChecked(false);
                enabled->setChecked(true);
                mode->setCurrentIndex(4);
                state->raw = capture("raw-ao");
                const auto hdr = viewport->grab_hdr();
                check_composite(*state->baseline, hdr, state->raw);
                hdr.write_exr(path(directory + "/raw-composite.exr"));
                const glm::vec3 near{0.62F, 0.002F, 0.2F}, far{1.8F, 0.002F, 0.7F};
                const double near_ao =
                    average(state->raw, project(*viewport, near, state->raw.size()));
                const double far_ao =
                    average(state->raw, project(*viewport, far, state->raw.size()));
                const auto snapshot = minirender::make_snapshot(state->scene);
                const double cpu_near = cpu_visibility(snapshot, near),
                             cpu_far = cpu_visibility(snapshot, far);
                qInfo() << "AO near/far GPU" << near_ao << far_ao << "CPU geometry" << cpu_near
                        << cpu_far;
                require(near_ao + 0.04 < far_ao && cpu_near + 0.1 < cpu_far && far_ao > 0.98,
                        "AO contact and open-plane ordering failed");
                break;
            }
            case 3: {
                window.findChild<QCheckBox*>("ssao_filtered")->setChecked(true);
                mode->setCurrentIndex(5);
                const auto filtered = capture("filtered-ao");
                const auto hdr = viewport->grab_hdr();
                check_composite(*state->baseline, hdr, filtered);
                const auto probe = project(*viewport, {0.65F, 0.002F, 0.2F}, filtered.size());
                const auto before = variation(state->raw, probe),
                           after = variation(filtered, probe);
                qInfo() << "AO local gradient energy raw/filtered:" << before << after;
                require(before > 0.001 && after < before * 0.8,
                        "AO filter did not reduce sampling noise");
                mode->setCurrentIndex(0);
                capture("filtered-composite");
                mode->setCurrentIndex(3);
                capture("wire");
                equal_hdr(hdr, viewport->grab_hdr());
                require(viewport->upload_statistics().mesh_uploads == state->uploads.mesh_uploads &&
                            viewport->upload_statistics().descriptor_rebuilds ==
                                state->uploads.descriptor_rebuilds &&
                            viewport->resource_generations() == state->generation,
                        "AO controls rebuilt scene assets or swapchain");
                break;
            }
            case 4:
                state->scene.environment = glm::vec3(0);
                state->scene.materials[0].emission = {0.2F, 0.1F, 0.05F};
                state->scene.directional_lights.push_back({"sun", {-1, -2, -1}, {1, 1, 1}, 2});
                install(window, state->scene, directory);
                mode->setCurrentIndex(0);
                state->baseline = viewport->grab_hdr();
                enabled->setChecked(false);
                equal_hdr(*state->baseline, viewport->grab_hdr());
                break;
            case 5:
                state->scene.environment = glm::vec3(0.6F);
                state->scene.directional_lights.clear();
                state->scene.materials[0].kind = minirender::MaterialKind::conductor;
                install(window, state->scene, directory);
                state->baseline = viewport->grab_hdr();
                enabled->setChecked(true);
                equal_hdr(*state->baseline, viewport->grab_hdr());
                state->generation = viewport->resource_generations();
                state->previous_extent = viewport->swapChainImageSize();
                state->stable_frames = 0;
                window.resize(1000, 600);
                break;
            case 6:
                if (viewport->resource_generations() <= state->generation ||
                    viewport->swapChainImageSize() == state->previous_extent ||
                    !stable_extent(*viewport, *state)) {
                    --state->step;
                    return;
                }
                mode->setCurrentIndex(5);
                state->raw = capture("ao-resized");
                viewport->set_camera(
                    minirender::rotate_camera(viewport->camera(), 0.2F, -0.1F, true));
                {
                    const auto moved = capture("ao-camera");
                    require(moved.size() == viewport->swapChainImageSize() &&
                                moved.size() == state->raw.size(),
                            "AO resize dimensions are stale");
                    std::size_t changed = 0, contact = 0;
                    for (int y = 0; y < moved.height(); ++y) {
                        for (int x = 0; x < moved.width(); ++x) {
                            const int current = moved.pixelColor(x, y).red();
                            changed += std::abs(current - state->raw.pixelColor(x, y).red()) > 3;
                            contact += current < 240;
                        }
                    }
                    require(changed > 100 && contact > 100,
                            "AO did not update after camera movement");
                }
                break;
            case 7: {
                const QPointF origin(viewport->width() * 0.5, viewport->height() * 0.5);
                const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                                      Qt::MouseButtons buttons) {
                    QMouseEvent event(type, point, viewport->mapToGlobal(point), button, buttons,
                                      Qt::NoModifier);
                    QCoreApplication::sendEvent(viewport, &event);
                };
                if (state->drag == 0) {
                    state->camera = viewport->camera();
                    state->frame = viewport->rendered_frames();
                    send(QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
                } else if (state->drag < 6) {
                    send(QEvent::MouseMove, origin + QPointF(state->drag * 5, state->drag * 2),
                         Qt::NoButton, Qt::LeftButton);
                } else {
                    send(QEvent::MouseButtonRelease, origin + QPointF(25, 10), Qt::LeftButton,
                         Qt::NoButton);
                    require(viewport->camera().position != state->camera.position &&
                                viewport->rendered_frames() > state->frame + 2,
                            "AO rendering stalled during camera drag");
                    timer->stop();
                    qInfo()
                        << "SSAO smoke PASS: contact/open plane, CPU visibility ordering, filter "
                           "noise, linear HDR composition, light/emission/metal isolation, "
                           "debug/wire, cached assets, camera, resize and active drag";
                    QApplication::exit(0);
                    break;
                }
                ++state->drag;
                --state->step;
                break;
            }
            }
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "SSAO smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
