#include "outline_smoke.hpp"
#include "geometry_smoke.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/bvh.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QTimer>

#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace {
using namespace minirender;
void require(bool condition, const char* error) {
    if (!condition) {
        throw std::runtime_error(error);
    }
}
std::filesystem::path native(const QString& path) {
    const auto bytes = path.toUtf8();
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}
void enabled(Workbench& window, bool value) {
    window.findChild<QCheckBox*>("preview_outline")->setChecked(value);
}
void strength(Workbench& window, double value) {
    window.findChild<QDoubleSpinBox*>("preview_outline_strength")->setValue(value);
}
void mode(Workbench& window, int value) {
    window.findChild<QComboBox*>("viewport_view_mode")->setCurrentIndex(value);
}

SceneDocument fixture(const SceneDocument& input) {
    auto scene = input;
    scene.objects.erase(std::remove_if(scene.objects.begin(), scene.objects.end(),
                                       [](const auto& object) {
                                           return object.id != "ground" && object.id != "blue_cube";
                                       }),
                        scene.objects.end());
    scene.environment = glm::vec3(0);
    scene.directional_lights.clear();
    for (auto& material : scene.materials) {
        material.base_color = glm::vec3(.73F);
        material.emission = glm::vec3(0);
        material.kind = MaterialKind::diffuse;
        material.base_color_texture.clear();
    }
    for (auto& object : scene.objects) {
        object.transform = {};
        if (object.id == "ground") {
            object.transform.translation = {0, 1, -2};
            object.transform.rotation_degrees = {90, 0, 0};
            object.transform.scale = {5, 1, 5};
        } else {
            object.transform.translation = {0, 1, 0};
        }
    }
    scene.camera.position = {0, 1, 5};
    scene.camera.target = {0, 1, 0};
    scene.camera.aperture_radius = 0;
    return scene;
}

// 用 CPU 最近命中判定真正的轮廓与折角；同一平面的内部像素必须保持原色。
void verify_edges(Workbench& window, const QImage& off, const QImage& on, bool expect_crease) {
    require(off.size() == on.size(), "Outline changed viewport dimensions");
    const SceneBvh bvh(window.viewport()->scene());
    const auto& camera = window.viewport()->camera();
    const double aspect = double(off.width()) / off.height();
    const int radius = std::max(1, qRound(window.devicePixelRatioF()));
    const auto hit = [&](int x, int y) {
        return bvh.intersect(
            camera.generate_ray({(x + .5) / off.width(), (y + .5) / off.height()}, aspect));
    };
    const std::array<QPoint, 8> directions{QPoint(1, 0),  QPoint(-1, 0), QPoint(0, 1),
                                           QPoint(0, -1), QPoint(1, 1),  QPoint(1, -1),
                                           QPoint(-1, 1), QPoint(-1, -1)};
    int silhouette = 0, silhouette_matches = 0, creases = 0, crease_matches = 0, interior = 0,
        false_edges = 0;
    for (int y = radius * 4; y < off.height() - radius * 4; y += 2) {
        for (int x = radius * 4; x < off.width() - radius * 4; x += 2) {
            const auto center = hit(x, y);
            if (!center) {
                continue;
            }
            bool boundary = false, crease = false, flat = true;
            for (const auto& direction : directions) {
                const auto other = hit(x + direction.x() * radius, y + direction.y() * radius);
                if (!other || other->instance_index != center->instance_index) {
                    if ((direction.x() == 0 || direction.y() == 0) &&
                        (!other || other->distance > center->distance + .01)) {
                        boundary = true;
                    }
                    flat = false;
                } else if (glm::dot(center->shading_normal, other->shading_normal) < .8F) {
                    crease = true;
                    flat = false;
                }
                const auto distant =
                    hit(x + direction.x() * radius * 3, y + direction.y() * radius * 3);
                if (!distant || distant->instance_index != center->instance_index ||
                    glm::dot(center->shading_normal, distant->shading_normal) < .98F) {
                    flat = false;
                }
            }
            const int delta = off.pixelColor(x, y).red() - on.pixelColor(x, y).red();
            if (boundary) {
                ++silhouette;
                silhouette_matches += std::abs(delta) >= 3;
            }
            if (crease) {
                ++creases;
                crease_matches += std::abs(delta) >= 2;
            }
            if (flat) {
                ++interior;
                false_edges += std::abs(delta) > 1;
            }
        }
    }
    qInfo() << "Outline geometry probes" << "silhouette" << silhouette_matches << '/' << silhouette
            << "crease" << crease_matches << '/' << creases << "flat errors" << false_edges << '/'
            << interior << "DPR" << window.devicePixelRatioF();
    require(silhouette > 80 && silhouette_matches >= silhouette * .90,
            "Depth outline did not separate foreground from same-color background");
    require(interior > 1000 && false_edges < interior * .001,
            "Outline shaded planar interiors or created triangle diagonals");
    if (expect_crease) {
        require(creases > 15 && crease_matches >= creases * .80, "Normal creases were not visible");
    }
}

double hdr_error(const Image& a, const Image& b) {
    require(a.width() == b.width() && a.height() == b.height(), "Outline changed HDR extent");
    double result = 0;
    for (std::size_t y = 0; y < a.height(); ++y) {
        for (std::size_t x = 0; x < a.width(); ++x) {
            const auto d = glm::abs(a.at(x, y) - b.at(x, y));
            result = std::max(result, double(std::max({d.x, d.y, d.z})));
        }
    }
    return result;
}

// 合成鼠标事件只发送给隐藏的测试窗口，不借用桌面焦点。
void drag(VulkanViewport& viewport, QEvent::Type type, QPointF p, Qt::MouseButton button,
          Qt::MouseButtons buttons) {
    QMouseEvent event(type, p, p, viewport.mapToGlobal(p), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&viewport, &event);
}
struct State {
    int stage = 0, ticks = 0, frame = 0, motion = 0;
    QImage off, on, light;
    std::shared_ptr<const Image> hdr;
    SceneDocument scene;
    SceneUploadStatistics uploads;
    minirender::Camera camera;
    UiResponseProbe* probe = nullptr;
};
} // namespace

void start_outline_smoke(Workbench& window, const QString& directory, bool measure_response) {
    require(QDir().mkpath(directory), "Cannot create outline output directory");
    require(window.viewport()->preview_outline(), "Outline is not enabled by default");
    auto state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(60);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, measure_response] {
            try {
                auto* view = window.viewport();
                require(++state->ticks < 1200 && view->failure().isEmpty(),
                        "Outline regression stalled or failed");
                if (window.asset_loading() || !view->can_render() ||
                    view->rendered_frames() < state->frame + 3) {
                    return;
                }
                const auto advance = [&] {
                    ++state->stage;
                    state->frame = view->rendered_frames();
                };
                switch (state->stage) {
                case 0:
                    // GPU 初始化结束后开始计时，参考计算和显式读回由 verify 排除。
                    state->probe = new UiResponseProbe(&window, "Outline");
                    state->scene = fixture(window.document());
                    state->probe->verify([&] {
                        save_scene(state->scene, native(directory + "/front.json"));
                        window.load_scene_file(directory + "/front.json");
                    });
                    window.resize(1280, 800);
                    window.select_object("blue_cube");
                    window.findChild<QCheckBox*>("preview_occlusion")->setChecked(false);
                    window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(false);
                    mode(window, VulkanViewport::base_color_preview_mode);
                    enabled(window, false);
                    advance();
                    return;
                case 1:
                    state->probe->verify([&] {
                        state->off = view->grab();
                        state->hdr = std::make_shared<const Image>(view->grab_hdr());
                        capture_workbench_ui(window, directory + "/before.png");
                    });
                    state->uploads = view->upload_statistics();
                    enabled(window, true);
                    advance();
                    return;
                case 2:
                    state->probe->verify([&] {
                        state->on = view->grab();
                        verify_edges(window, state->off, state->on, false);
                        require(hdr_error(*state->hdr, view->grab_hdr()) == 0,
                                "Display outlines modified HDR export");
                        capture_workbench_ui(window, directory + "/after.png");
                    });
                    strength(window, .7);
                    advance();
                    return;
                case 3:
                    state->probe->verify([&] {
                        const auto strong = view->grab();
                        int changed = 0;
                        for (int y = 0; y < strong.height(); ++y) {
                            for (int x = 0; x < strong.width(); ++x) {
                                require(strong.pixelColor(x, y).red() <=
                                            state->on.pixelColor(x, y).red(),
                                        "Stronger outline brightened a surface");
                                changed += strong.pixelColor(x, y).red() + 2 <
                                           state->on.pixelColor(x, y).red();
                            }
                        }
                        require(changed > 100, "Outline strength had no visible effect");
                    });
                    strength(window, 0);
                    advance();
                    return;
                case 4:
                    state->probe->verify([&] {
                        require(state->off == view->grab(),
                                "Zero strength did not restore exact original view");
                    });
                    strength(window, .35);
                    enabled(window, false);
                    advance();
                    return;
                case 5:
                    state->probe->verify([&] {
                        require(state->off == view->grab(),
                                "Outline toggle did not restore exact original view");
                    });
                    mode(window, 0);
                    advance();
                    return;
                case 6:
                    state->probe->verify([&] { state->light = view->grab(); });
                    enabled(window, true);
                    strength(window, 1);
                    advance();
                    return;
                case 7:
                    state->probe->verify([&] {
                        require(state->light == view->grab(),
                                "Preview outlines leaked into lighting mode");
                    });
                    require(view->upload_statistics().mesh_uploads == state->uploads.mesh_uploads &&
                                view->upload_statistics().descriptor_rebuilds ==
                                    state->uploads.descriptor_rebuilds,
                            "Outline settings rebuilt scene resources");
                    state->scene.camera.position = {2.3F, 2.4F, 5};
                    state->scene.objects[1].transform.rotation_degrees.y = 25;
                    state->probe->verify([&] {
                        save_scene(state->scene, native(directory + "/tilted.json"));
                        window.load_scene_file(directory + "/tilted.json");
                    });
                    mode(window, VulkanViewport::base_color_preview_mode);
                    strength(window, .35);
                    enabled(window, false);
                    advance();
                    return;
                case 8:
                    state->probe->verify([&] { state->off = view->grab(); });
                    enabled(window, true);
                    advance();
                    return;
                case 9:
                    state->probe->verify([&] {
                        state->on = view->grab();
                        verify_edges(window, state->off, state->on, true);
                        capture_workbench_ui(window, directory + "/creases.png");
                    });
                    advance();
                    return;
                case 10:
                    state->probe->verify([&] {
                        require(state->on == view->grab(),
                                "Outline changed while camera was stationary");
                    });
                    window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(true);
                    advance();
                    return;
                case 11:
                    state->probe->verify([&] {
                        require(state->on != view->grab(),
                                "Anti-aliasing did not smooth outline image");
                        capture_workbench_ui(window, directory + "/antialiased.png");
                    });
                    state->camera = view->camera();
                    advance();
                    return;
                case 12: {
                    const QPointF p(view->width() * .5, view->height() * .5);
                    drag(*view, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
                    drag(*view, QEvent::MouseMove, p + QPointF(12, 6), Qt::NoButton,
                         Qt::LeftButton);
                    drag(*view, QEvent::MouseButtonRelease, p + QPointF(12, 6), Qt::LeftButton,
                         Qt::NoButton);
                    if (++state->motion < 6) {
                        state->frame = view->rendered_frames();
                        return;
                    }
                    require(glm::length(view->camera().position - state->camera.position) > .01F,
                            "Outline view could not be dragged");
                    window.resize(960, 700);
                    advance();
                    return;
                }
                case 13:
                    state->probe->verify(
                        [&] { capture_workbench_ui(window, directory + "/narrow.png"); });
                    window.resize(1280, 800);
                    window.import_builtin_scene("cornell");
                    advance();
                    return;
                case 14:
                    window.findChild<QCheckBox*>("preview_occlusion")->setChecked(true);
                    strength(window, .35);
                    advance();
                    return;
                case 15:
                    state->probe->verify(
                        [&] { capture_workbench_ui(window, directory + "/cornell.png"); });
                    // 全黑前景和背景仍须有可见轮廓。
                    for (auto& material : state->scene.materials)
                        material.base_color = glm::vec3(0);
                    state->scene.camera.position = {0, 1, 5};
                    state->scene.camera.target = {0, 1, 0};
                    state->scene.objects[1].transform.rotation_degrees = glm::vec3(0);
                    state->probe->verify([&] {
                        save_scene(state->scene, native(directory + "/dark.json"));
                        window.load_scene_file(directory + "/dark.json");
                    });
                    window.findChild<QCheckBox*>("preview_occlusion")->setChecked(false);
                    window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(false);
                    enabled(window, false);
                    advance();
                    return;
                case 16:
                    state->probe->verify([&] { state->off = view->grab(); });
                    enabled(window, true);
                    advance();
                    return;
                case 17:
                    state->probe->verify([&] {
                        verify_edges(window, state->off, view->grab(), false);
                        capture_workbench_ui(window, directory + "/dark.png");
                    });
                    state->probe->report(measure_response);
                    timer->stop();
                    qInfo() << "Outline smoke PASS: same-color depth contours, normal creases, "
                               "stable planes, strength, HDR and mode isolation, antialiasing, DPI "
                               "and camera drags";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& e) {
                timer->stop();
                qCritical() << "Outline smoke FAIL:" << e.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
