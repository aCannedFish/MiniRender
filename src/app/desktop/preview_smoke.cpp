// 隐藏窗口回归：preview 的控件操作、渲染结果与资源生命周期检查。
// 本模块接口见 preview_smoke.hpp。

#include "preview_smoke.hpp"
#include "geometry_smoke.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/bvh.hpp>
#include <minirender/render/path_tracer.hpp>
#include <minirender/scene/cloth_material.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <map>
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

void mode(Workbench& window, int value) {
    window.findChild<QComboBox*>("viewport_view_mode")->setCurrentIndex(value);
    require(window.viewport()->view_mode() == value &&
                window.findChild<QComboBox*>("view_mode")->currentIndex() == value,
            "Preview mode selectors are not synchronized");
}

void occlusion(Workbench& window, bool enabled) {
    window.findChild<QCheckBox*>("preview_occlusion")->setChecked(enabled);
}

template <typename V> auto max_component(const V& v) {
    return std::max({v.x, v.y, v.z});
}

double difference(const Image& a, const Image& b) {
    require(a.width() == b.width() && a.height() == b.height(), "Preview dimensions changed");
    double error = 0;
    for (std::size_t y = 0; y < a.height(); ++y) {
        for (std::size_t x = 0; x < a.width(); ++x) {
            error = std::max(error, double(max_component(glm::abs(a.at(x, y) - b.at(x, y)))));
        }
    }
    return error;
}

// BVH 独立决定最近可见物体，逐像素比较线性底色和 sRGB 显示；排除轮廓与三角形边界。
void verify_colors(Workbench& window, const Image& hdr, const QImage& screen) {
    const auto                 scene = window.viewport()->scene();
    const SceneBvh             bvh(scene);
    const auto&                camera = window.viewport()->camera();
    const double               aspect = double(hdr.width()) / double(hdr.height());
    std::map<std::string, int> counts;
    int                        texture_samples = 0;
    double                     maximum         = 0;
    for (std::size_t y = 8; y + 8 < hdr.height(); y += 9) {
        for (std::size_t x = 8; x + 8 < hdr.width(); x += 9) {
            const auto ray = camera.generate_ray(
                {(double(x) + .5) / double(hdr.width()), (double(y) + .5) / double(hdr.height())},
                aspect);
            const auto hit = bvh.intersect(ray);
            if (!hit ||
                std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < .025) {
                continue;
            }
            const auto& instance = scene->instances[hit->instance_index];
            const auto& material = instance.material;
            auto        expected = material.base_color;
            if (instance.base_color_texture) {
                expected *= glm::clamp(
                    instance.base_color_texture->sample(hit->uv, 0, material.texture_filter),
                    glm::vec3(0), glm::vec3(1));
                ++texture_samples;
            }
            if (material.kind == MaterialKind::cloth) {
                const auto& cloth = material.cloth->settings();
                expected *= glm::vec3(cloth.coverage[0] * cloth.threads[0].albedo +
                                      cloth.coverage[1] * cloth.threads[1].albedo);
            }
            const auto peak = max_component(material.emission);
            if (peak > 0) {
                expected = glm::max(expected, material.emission / std::max(peak, 1.0F));
            }
            maximum = std::max(maximum, double(max_component(glm::abs(expected - hdr.at(x, y)))));
            const auto pixel = screen.pixelColor(int(x), int(y));
            for (int c = 0; c < 3; ++c) {
                const int actual = c == 0 ? pixel.red() : c == 1 ? pixel.green() : pixel.blue();
                require(std::abs(actual - int(encode_unorm8(linear_to_srgb(
                                              std::clamp(expected[c], 0.0F, 1.0F))))) <= 2,
                        "Base color display changed color encoding or applied tone mapping");
            }
            ++counts[instance.object_id];
        }
    }
    qInfo() << "Preview reference" << counts.size() << "objects, textures" << texture_samples
            << "linear max" << maximum;
    require(counts.size() >= 3 && maximum < .0001,
            "Base color image differs from visible materials");
    if (!scene->environment_texture && !scene->instances.empty() &&
        scene->instances[0].object_id == "textured-floor") {
        require(texture_samples > 20, "Texture fixture was not visible");
    }
}

void verify_ao(const Image& plain, const Image& shaded) {
    require(plain.width() == shaded.width() && plain.height() == shaded.height(),
            "AO changed image extent");
    int darkened = 0;
    for (std::size_t y = 0; y < plain.height(); ++y) {
        for (std::size_t x = 0; x < plain.width(); ++x) {
            const auto a = plain.at(x, y), b = shaded.at(x, y);
            for (int c = 0; c < 3; ++c) {
                require(b[c] + 1e-5F >= .65F * a[c] && b[c] <= a[c] + 1e-5F,
                        "Contact AO changed hue or exceeded darkening bound");
            }
            darkened += max_component(a - b) > .01F;
        }
    }
    qInfo() << "Preview contact pixels" << darkened;
    require(darkened > 100, "Preview contact occlusion did not darken any surfaces");
}

// 使用 CPU 最近交点和未贴图的材质底色，独立计算点光源照明参考值。
void verify_teaching_light(VulkanViewport& viewport, const Image& image) {
    const auto     scene = viewport.scene();
    const SceneBvh bvh(scene);
    const auto&    cam     = viewport.camera();
    const auto     light   = viewport.teaching_light();
    const auto     surface = viewport.teaching_surface();
    std::size_t    used    = 0;
    for (std::size_t y = 6; y < image.height(); y += 13) {
        for (std::size_t x = 6; x < image.width(); x += 13) {
            const auto ray = cam.generate_ray({(double(x) + .5) / double(image.width()),
                                               (double(y) + .5) / double(image.height())},
                                              double(image.width()) / double(image.height()));
            const auto hit = bvh.intersect(ray);
            if (!hit ||
                std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < .08) {
                continue;
            }
            const auto& material = scene->instances[hit->instance_index].material;
            const auto  delta    = glm::vec3(light) - glm::vec3(hit->position);
            const auto  l        = glm::normalize(delta),
                        v = glm::normalize(glm::vec3(cam.position) - glm::vec3(hit->position));
            auto        n = hit->shading_normal;
            if (glm::dot(n, v) < 0) {
                n = -n;
            }
            const auto cosine = std::max(glm::dot(n, l), 0.F);
            const auto spec =
                cosine > 0 ? std::pow(std::max(glm::dot(n, glm::normalize(l + v)), 0.F), surface.x)
                           : 0.F;
            const auto expected = .05F * material.base_color +
                                  light.w / glm::dot(delta, delta) *
                                      (material.base_color * cosine + glm::vec3(surface.y * spec));
            require(max_component(glm::abs(expected - image.at(x, y))) < .002F,
                    "Blinn–Phong does not match CPU point light reference");
            ++used;
        }
    }
    require(used > 30, "Missing teaching shader reference pixels");
}

// 用同一相机分别读回两侧和分屏。拖动分割线后，各像素应来自对应的整图缓存。
void verify_comparison(Workbench& window, const QString& directory) {
    auto* viewport = window.viewport();
    auto* mode     = window.findChild<QComboBox*>("view_mode");
    auto* right    = window.findChild<QComboBox*>("compare_right");
    mode->setCurrentIndex(VulkanViewport::base_color_preview_mode);
    right->setCurrentIndex(VulkanViewport::blinn_phong_mode + 1);
    const auto left  = viewport->grab_comparison_side(0);
    const auto other = viewport->grab_comparison_side(1);
    require(!left.isNull() && !other.isNull() && left != other, "Comparison sides are identical");
    const auto count  = viewport->comparison_draws();
    const auto height = viewport->height() * 0.5;
    const auto send   = [&](QEvent::Type type, double x, Qt::MouseButton button,
                            Qt::MouseButtons buttons) {
        const QPointF pos(x, height);
        QMouseEvent   event(type, pos, pos, pos, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &event);
    };
    send(QEvent::MouseButtonPress, viewport->width() * .5, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, viewport->width() * .3, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, viewport->width() * .3, Qt::LeftButton, Qt::NoButton);
    const auto composite = viewport->grab();
    require(std::abs(viewport->split_position() - .3F) < .002F &&
                viewport->comparison_draws() == count,
            "Moving divider rerendered scene");
    int mismatched = 0, total = 0;
    for (int y = 4; y < composite.height(); y += 11) {
        for (int x = 4; x < composite.width(); x += 13) {
            const int split = static_cast<int>(static_cast<float>(composite.width()) * .3F);
            if (std::abs(x - split) < 4) {
                continue;
            }
            const auto expected = (x < split ? left : other).pixelColor(x, y);
            const auto actual   = composite.pixelColor(x, y);
            ++total;
            if (expected != actual) {
                ++mismatched;
            }
        }
    }
    require(mismatched <= total / 100, "Composite differs from source views");
    auto settings = viewport->teaching_surface();
    settings.z    = 0.F;
    viewport->set_teaching(viewport->teaching_light(), settings);
    window.findChild<QCheckBox*>("ssao_enabled")->setChecked(false);
    const auto raw = viewport->grab_comparison_hdr(1);
    verify_teaching_light(*viewport, raw);
    const auto original = viewport->teaching_light();
    auto       moved    = original;
    moved.x += 1.25F;
    const auto old_draws = viewport->comparison_draws();
    viewport->set_teaching(moved, viewport->teaching_surface());
    const auto after = viewport->grab_comparison_side(1);
    require(after != other && viewport->comparison_draws() == old_draws + 1,
            "Point light did not update teaching side independently");
    const auto moved_raw = viewport->grab_comparison_hdr(1);
    require(difference(raw, moved_raw) > .01, "Point light does not affect HDR");
    composite.save(directory + "/comparison.png");
    right->setCurrentIndex(0);
    mode->setCurrentIndex(VulkanViewport::blinn_phong_mode);
    require(difference(moved_raw, viewport->grab_hdr()) < 1e-5,
            "A/B HDR differs from single viewport");
}

struct State {
    int                          stage = 0;
    int                          ticks = 0;
    int                          frame = 0;
    std::shared_ptr<const Image> plain, shaded, lit, offline;
    SceneUploadStatistics        uploads;
    SceneDocument                cornell;
    std::filesystem::path        source;
};
} // namespace

void start_preview_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create preview directory");
    require(window.viewport()->view_mode() == VulkanViewport::base_color_preview_mode,
            "Startup did not use base color preview");
    auto  state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(60);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            auto* viewport = window.viewport();
            require(++state->ticks < 1200 && viewport->failure().isEmpty(),
                    "Preview test stalled or failed");
            if (window.asset_loading() || !viewport->can_render() ||
                viewport->rendered_frames() < state->frame + 3) {
                return;
            }
            const auto advance = [&] {
                ++state->stage;
                state->frame = viewport->rendered_frames();
            };
            switch (state->stage) {
            case 0:
                state->source = window.document().asset_directory;
                window.resize(1280, 800);
                window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(false);
                window.import_builtin_scene("cornell");
                advance();
                return;
            case 1:
                require(viewport->view_mode() == VulkanViewport::base_color_preview_mode,
                        "Cornell import did not use base colors");
                state->cornell = window.document();
                require(state->cornell.directional_lights.empty() &&
                            state->cornell.environment == glm::vec3(0),
                        "Cornell lighting fixture changed");
                capture_workbench_ui(window, directory + "/cornell-preview.png");
                occlusion(window, false);
                // 原色数学对照使用未叠加显示轮廓的图像；轮廓由独立专项检查。
                window.findChild<QCheckBox*>("preview_outline")->setChecked(false);
                advance();
                return;
            case 2:
                state->plain = std::make_shared<const Image>(viewport->grab_hdr());
                verify_colors(window, *state->plain, viewport->grab());
                state->plain->write_exr(native(directory + "/cornell-base.exr"));
                viewport->grab().save(directory + "/cornell-base.png");
                state->offline = std::make_shared<const Image>(
                    PathTracer(make_snapshot(window.document()), {.maximum_depth = 2})
                        .render({8, 8, 4, 101}));
                state->uploads = viewport->upload_statistics();
                mode(window, 0);
                advance();
                return;
            case 3:
                state->lit = std::make_shared<const Image>(viewport->grab_hdr());
                require(difference(*state->plain, *state->lit) > .3,
                        "Cornell light and base color views are not distinct");
                mode(window, VulkanViewport::base_color_preview_mode);
                occlusion(window, true);
                advance();
                return;
            case 4: {
                state->shaded = std::make_shared<const Image>(viewport->grab_hdr());
                verify_ao(*state->plain, *state->shaded);
                require(viewport->upload_statistics().mesh_uploads == state->uploads.mesh_uploads &&
                            viewport->upload_statistics().descriptor_rebuilds ==
                                state->uploads.descriptor_rebuilds,
                        "Preview mode changes rebuilt meshes or descriptors");
                const auto offline =
                    PathTracer(make_snapshot(window.document()), {.maximum_depth = 2})
                        .render({8, 8, 4, 101});
                require(difference(*state->offline, offline) == 0,
                        "Preview changes modified the scene used by offline rendering");
                auto bright        = state->cornell;
                bright.environment = {5, 3, 2};
                bright.directional_lights.push_back({"test", {-1, -2, -1}, {1, .8F, .6F}, 10});
                save_scene(bright, native(directory + "/bright.json"));
                window.load_scene_file(directory + "/bright.json");
                window.findChild<QDoubleSpinBox*>("exposure")->setValue(4);
                window.findChild<QCheckBox*>("shadows_enabled")->setChecked(false);
                advance();
                return;
            }
            case 5:
                require(difference(*state->shaded, viewport->grab_hdr()) < 1e-6,
                        "Scene lighting, shadows or exposure changed base color preview");
                mode(window, 0);
                advance();
                return;
            case 6:
                require(difference(*state->lit, viewport->grab_hdr()) > .1,
                        "Lighting mode ignored edited lights");
                mode(window, VulkanViewport::base_color_preview_mode);
                window.findChild<QDoubleSpinBox*>("exposure")->setValue(0);
                window.findChild<QCheckBox*>("shadows_enabled")->setChecked(true);
                occlusion(window, false);
                {
                    auto scene = load_scene(state->source / "studio-textured.json");
                    for (auto& material : scene.materials) {
                        material.texture_filter = TextureFilter::nearest;
                        if (material.id == "blue") {
                            material.kind  = MaterialKind::cloth;
                            material.cloth = make_cloth_preset("linen").appearance;
                        }
                    }
                    for (auto& object : scene.objects) {
                        if (object.id == "ground") {
                            object.id = "textured-floor";
                        }
                    }
                    save_scene(scene, native(directory + "/textured.json"));
                    window.load_scene_file(directory + "/textured.json");
                }
                advance();
                return;
            case 7:
                verify_colors(window, viewport->grab_hdr(), viewport->grab());
                capture_workbench_ui(window, directory + "/textured-preview.png");
                window.resize(960, 700);
                advance();
                return;
            case 8: {
                auto* selector = window.findChild<QComboBox*>("viewport_view_mode");
                require(selector->isVisibleTo(&window),
                        "Viewport mode selector is hidden in the narrow window");
            }
                verify_colors(window, viewport->grab_hdr(), viewport->grab());
                capture_workbench_ui(window, directory + "/preview-960.png");
                const auto before_fxaa = viewport->grab_hdr();
                window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(true);
                viewport->grab().save(directory + "/preview-fxaa.png");
                require(difference(before_fxaa, viewport->grab_hdr()) == 0,
                        "Preview anti-aliasing modified base color HDR data");
                verify_comparison(window, directory);
                timer->stop();
                qInfo() << "Preview smoke PASS: colors, textures, depth visibility, contact AO, "
                           "light isolation, display encoding, defaults and mode switching";
                QApplication::exit(0);
                return;
            }
        } catch (const std::exception& e) {
            timer->stop();
            qCritical() << "Preview smoke FAIL:" << e.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
