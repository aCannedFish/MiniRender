#include "sdf_smoke.hpp"
#include "geometry_smoke.hpp"
#include "sdf_editor_smoke.hpp"
#include "workbench.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QTimer>

#include <array>
#include <cmath>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

// 从二次方程直接求距离、法线及观察深度，不调用距离场的 CPU 追踪器。
void analytic_sphere(VulkanViewport& viewport, bool normals) {
    const auto image = viewport.grab_hdr();
    std::size_t hits = 0, misses = 0;
    for (std::size_t y = 2; y < image.height(); y += 5) {
        for (std::size_t x = 2; x < image.width(); x += 5) {
            const auto ray =
                viewport.camera().generate_ray({(double(x) + .5) / double(image.width()),
                                                (double(y) + .5) / double(image.height())},
                                               double(image.width()) / double(image.height()));
            const double b = glm::dot(ray.origin, ray.direction);
            const double discriminant = b * b - glm::dot(ray.origin, ray.origin) + 1;
            if (std::abs(discriminant) < .05) {
                continue;
            }
            glm::vec3 expected(0);
            if (discriminant > 0) {
                const double t = -b - std::sqrt(discriminant);
                const auto n = glm::normalize(ray.origin + ray.direction * t);
                expected = normals ? glm::vec3(n * .5 + .5)
                                   : glm::vec3(float(1 / (1 - t * ray.direction.z)));
                ++hits;
            } else {
                ++misses;
            }
            require(glm::length(image.at(x, y) - expected) < (normals ? .003F : .0003F),
                    "SDF GPU differs from analytic sphere");
        }
    }
    qInfo() << "SDF analytic" << (normals ? "normals" : "depth") << "hits" << hits << "misses"
            << misses;
    require(hits > 500 && misses > 1000, "Too few analytic sphere samples");
}

// CSG 棱边及阴影边界附近的差分法线会放大 float 误差。用 CPU 邻域选择稳定像素。
void compare_cpu(VulkanViewport& viewport, const QString& directory, const QString& label) {
    const auto actual = viewport.grab_hdr();
    auto document = *viewport.sdf_document();
    document.camera = viewport.camera();
    const auto reference = minirender::render_sdf(
        document, {actual.width(), actual.height(), viewport.sdf_view(), viewport.sdf_shadows()});
    require(reference.limited == 0, "CPU reference exhausted its step budget");
    require(reference.hits > 1000, "SDF reference has too few surface samples");
    std::size_t count = 0, matched = 0;
    float maximum = 0;
    const float tolerance = viewport.sdf_view() == minirender::SdfView::steps   ? .02F
                            : viewport.sdf_view() == minirender::SdfView::depth ? .00005F
                                                                                : .003F;
    for (std::size_t y = 2; y + 2 < actual.height(); y += 5) {
        for (std::size_t x = 2; x + 2 < actual.width(); x += 5) {
            const auto expected = reference.image.at(x, y);
            bool stable = true;
            for (const auto offset :
                 {glm::ivec2(2, 0), glm::ivec2(-2, 0), glm::ivec2(0, 2), glm::ivec2(0, -2)}) {
                const auto neighbor = reference.image.at(std::size_t(int(x) + offset.x),
                                                         std::size_t(int(y) + offset.y));
                stable = stable && glm::length(neighbor - expected) < .06F;
            }
            if (!stable) {
                continue;
            }
            const float error = glm::length(actual.at(x, y) - expected);
            maximum = std::max(maximum, error);
            matched += error < tolerance ? 1 : 0;
            ++count;
        }
    }
    qInfo() << "SDF CPU/GPU" << label << matched << '/' << count << "max_rgb_error" << maximum;
    require(count > 5000 && count == matched, "SDF GPU differs from stable CPU reference samples");
    actual.write_exr(std::filesystem::path((directory + '/' + label + ".exr").toStdString()));
    const auto display = viewport.grab();
    require(display.save(directory + '/' + label + ".png"), "Cannot save SDF GPU image");
    for (std::size_t y = 2; y < actual.height(); y += 13) {
        for (std::size_t x = 2; x < actual.width(); x += 13) {
            auto linear = actual.at(x, y);
            if (viewport.sdf_view() == minirender::SdfView::surface) {
                linear *= std::exp2(viewport.exposure());
                linear /= glm::vec3(1) + linear;
            }
            const auto pixel = display.pixelColor(int(x), int(y));
            const std::array channels{pixel.red(), pixel.green(), pixel.blue()};
            for (int channel = 0; channel < 3; ++channel) {
                const int expected =
                    minirender::encode_unorm8(minirender::linear_to_srgb(linear[channel]));
                require(std::abs(channels[std::size_t(channel)] - expected) <= 2,
                        "SDF display differs from its HDR display transform");
            }
        }
    }
}

struct State {
    int stage = 0;
    int frame = 0;
    int ticks = 0;
    minirender::Camera scene_camera;
    minirender::Camera sdf_camera;
    std::uint64_t uploads = 0;
};

} // namespace

void verify_sdf_gpu(Workbench& window, const QString& directory, const QString& label) {
    compare_cpu(*window.viewport(), directory, label);
}

void start_sdf_smoke(Workbench& window, const QString& directory, bool enforce_response) {
    QDir().mkpath(directory);
    auto state = std::make_shared<State>();
    state->scene_camera = window.viewport()->camera();
    auto* timer = new QTimer(&window);
    timer->setInterval(70);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            auto* viewport = window.viewport();
            try {
                require(++state->ticks < 1500 && viewport->failure().isEmpty(),
                        "SDF regression timed out");
                if (viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto advance = [&] {
                    ++state->stage;
                    state->frame = viewport->rendered_frames();
                };
                switch (state->stage) {
                case 0: {
                    auto document = std::make_shared<minirender::SdfDocument>();
                    document->nodes.push_back(
                        {"sphere", "Sphere", minirender::SdfKind::sphere, {1, 0, 0}});
                    document->camera.position = {0, 0, 4};
                    document->camera.target = {0, 0, 0};
                    viewport->set_sdf_document(document, true);
                    viewport->set_sdf_view(minirender::SdfView::normals);
                    viewport->set_sdf_visible(true);
                    advance();
                    return;
                }
                case 1:
                    analytic_sphere(*viewport, true);
                    state->uploads = viewport->sdf_uploads();
                    viewport->set_sdf_view(minirender::SdfView::depth);
                    advance();
                    return;
                case 2:
                    analytic_sphere(*viewport, false);
                    require(viewport->sdf_uploads() == state->uploads,
                            "SDF view change rebuilt the graph");
                    viewport->set_sdf_document(
                        std::make_shared<minirender::SdfDocument>(minirender::make_sdf_example()),
                        true);
                    viewport->set_sdf_view(minirender::SdfView::surface);
                    advance();
                    return;
                case 3:
                    viewport->set_exposure(1);
                    compare_cpu(*viewport, directory, "sdf-surface");
                    viewport->set_exposure(0);
                    viewport->set_sdf_view(minirender::SdfView::normals);
                    advance();
                    return;
                case 4:
                    compare_cpu(*viewport, directory, "sdf-normals");
                    viewport->set_sdf_view(minirender::SdfView::depth);
                    advance();
                    return;
                case 5:
                    compare_cpu(*viewport, directory, "sdf-depth");
                    viewport->set_sdf_view(minirender::SdfView::steps);
                    advance();
                    return;
                case 6:
                    compare_cpu(*viewport, directory, "sdf-steps");
                    state->sdf_camera = viewport->camera();
                    state->uploads = viewport->sdf_uploads();
                    viewport->set_sdf_visible(false);
                    require(viewport->camera().position == state->scene_camera.position,
                            "SDF view lost the scene camera");
                    verify_geometry_gpu(window, directory, "sdf-return-scene");
                    viewport->set_sdf_visible(true);
                    require(viewport->camera().position == state->sdf_camera.position,
                            "SDF camera was not restored");
                    advance();
                    return;
                case 7:
                    require(viewport->sdf_uploads() == state->uploads,
                            "SDF display switch rebuilt the graph");
                    require(window.document().camera.position == state->scene_camera.position,
                            "SDF changed the scene document");
                    {
                        auto doc = std::make_shared<minirender::SdfDocument>();
                        doc->nodes = {{"sphere",
                                       "Sphere",
                                       minirender::SdfKind::sphere,
                                       {1, 0, 0},
                                       {-.3F, 0, 0}},
                                      {"box",
                                       "Box",
                                       minirender::SdfKind::box,
                                       {.75F, .75F, .75F},
                                       {.3F, 0, 0},
                                       {0, 30, 20},
                                       .9F},
                                      {"root",
                                       "Intersection",
                                       minirender::SdfKind::intersect,
                                       {},
                                       {},
                                       {},
                                       1,
                                       {},
                                       0,
                                       1}};
                        doc->root = 2;
                        doc->trace.maximum_steps = 1024;
                        doc->camera.position = {0, 0, 4};
                        doc->camera.target = {0, 0, 0};
                        viewport->set_sdf_document(doc, true);
                        viewport->set_sdf_view(minirender::SdfView::normals);
                    }
                    advance();
                    return;
                case 8:
                    compare_cpu(*viewport, directory, "sdf-intersection");
                    {
                        auto camera = viewport->camera();
                        camera.position = {0, 0, 0};
                        camera.target = {0, 0, -1};
                        viewport->set_camera(camera);
                    }
                    advance();
                    return;
                case 9:
                    compare_cpu(*viewport, directory, "sdf-inside");
                    {
                        auto doc =
                            std::make_shared<minirender::SdfDocument>(*viewport->sdf_document());
                        doc->trace.maximum_steps = 1;
                        viewport->set_sdf_document(doc);
                        viewport->set_sdf_view(minirender::SdfView::steps);
                    }
                    advance();
                    return;
                case 10: {
                    const auto image = viewport->grab_hdr();
                    std::size_t samples = 0;
                    for (std::size_t y = 3; y < image.height(); y += 17) {
                        for (std::size_t x = 3; x < image.width(); x += 17) {
                            require(image.at(x, y) == glm::vec3(1, 0, 1),
                                    "SDF exhausted ray is not magenta");
                            ++samples;
                        }
                    }
                    require(samples > 1000, "Too few exhausted-ray samples");
                    qInfo() << "SDF exhausted ray samples" << samples;
                }
                    viewport->set_sdf_document({});
                    timer->stop();
                    qInfo() << "SDF GPU PASS: analytic sphere, CSG, HDR, interior and limits";
                    start_sdf_editor_smoke(window, directory, enforce_response);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "SDF smoke FAIL stage" << state->stage << ':' << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
