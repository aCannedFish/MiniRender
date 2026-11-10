#include "point_cloud_smoke.hpp"
#include "geometry_smoke.hpp"
#include "point_cloud_editor_smoke.hpp"
#include "workbench.hpp"

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
bool close(QColor color, glm::vec3 expected) {
    // 与主场景法线视图相同，诊断 RGB 写入 sRGB 附件后由硬件编码。
    const auto encoded = [](float value) {
        return static_cast<int>(std::lround(minirender::linear_to_srgb(value) * 255));
    };
    return std::abs(color.red() - encoded(expected.r)) <= 2 &&
           std::abs(color.green() - encoded(expected.g)) <= 2 &&
           std::abs(color.blue() - encoded(expected.b)) <= 2;
}
glm::dvec3 project(const minirender::Camera& camera, glm::vec3 p, QSize extent) {
    const auto clip = glm::dmat4(camera.projection_matrix(static_cast<float>(extent.width()) /
                                                          static_cast<float>(extent.height()))) *
                      glm::dmat4(camera.view_matrix()) * glm::dvec4(p, 1);
    const auto ndc = glm::dvec3(clip) / clip.w;
    return {(ndc.x * .5 + .5) * extent.width(), (ndc.y * .5 + .5) * extent.height(), ndc.z};
}

// 软件投影每个小圆片并逐像素选择最近深度，独立检查 GPU 的颜色、覆盖及遮挡。
QImage verify_points(Workbench& window, const QString& directory, const QString& label) {
    auto* viewport = window.viewport();
    const auto image = viewport->grab();
    require(!image.isNull(), "Point preview capture is empty");
    require(image.save(directory + "/" + label + ".png"), "Cannot save point preview capture");
    const auto& data = *viewport->point_preview_data();
    const auto& settings = viewport->point_preview_settings();
    std::vector<glm::dvec3> screen;
    for (const auto& point : data.points) {
        screen.push_back(project(viewport->camera(), point.position, image.size()));
    }
    std::size_t samples = 0;
    std::size_t matched = 0;
    std::size_t point_samples = 0;
    for (int y = 1; y < image.height(); y += 2) {
        for (int x = 1; x < image.width(); x += 2) {
            double depth = 1;
            bool edge = false;
            glm::vec3 color{.08F, .09F, .11F};
            bool covered = false;
            for (std::size_t i = 0; i < screen.size(); ++i) {
                const auto p = screen[i];
                if (p.z < 0 || p.z > 1) {
                    continue;
                }
                const auto delta = glm::dvec2(x + .5, y + .5) - glm::dvec2(p);
                const double distance = glm::length(delta);
                edge = edge || std::abs(distance - settings.point_radius) < 1;
                if (distance < settings.point_radius && p.z <= depth) {
                    depth = p.z;
                    covered = true;
                    color = settings.normal_colors && data.has_normals
                                ? data.points[i].normal * .5F + .5F
                                : glm::vec3(.25F, .65F, .85F);
                }
            }
            if (!edge) {
                ++samples;
                point_samples += covered ? 1 : 0;
                matched += close(image.pixelColor(x, y), color) ? 1 : 0;
            }
        }
    }
    qInfo() << "Point preview oracle" << label << matched << "/" << samples << "point samples"
            << point_samples;
    require(samples > 1000 && point_samples > 60, "Point oracle has too few stable samples");
    require(matched == samples, "Point preview differs from independent disc/depth projection");
    return image;
}

// 线段采用独立世界位置插值，在预期像素附近检查诊断色，允许单像素线栅格差异。
void verify_lines(Workbench& window, glm::vec3 color, bool normals) {
    const auto image = window.viewport()->grab();
    const auto& data = *window.viewport()->point_preview_data();
    std::size_t colored = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            colored += close(image.pixelColor(x, y), color) ? 1 : 0;
        }
    }
    require(colored > 50, "Point preview diagnostic lines are missing");
    std::size_t probes = 0;
    std::size_t matched = 0;
    const auto count = normals ? data.normal_samples.size() : data.boundary_vertices.size() / 2;
    for (std::size_t i = 0; i < count; ++i) {
        const auto a =
            normals ? data.normal_samples[i].position : data.boundary_vertices[2 * i].position;
        const auto b = normals ? a + data.normal_samples[i].normal *
                                         window.viewport()->point_preview_settings().normal_length
                               : data.boundary_vertices[2 * i + 1].position;
        for (const float t : {.35F, .55F, .75F}) {
            const auto screen = project(window.viewport()->camera(), a + t * (b - a), image.size());
            if (screen.z < 0 || screen.z > 1 || screen.x < 3 || screen.y < 3 ||
                screen.x >= image.width() - 3 || screen.y >= image.height() - 3) {
                continue;
            }
            bool found = false;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    found = found || close(image.pixelColor(static_cast<int>(screen.x) + dx,
                                                            static_cast<int>(screen.y) + dy),
                                           color);
                }
            }
            ++probes;
            matched += found ? 1 : 0;
        }
    }
    qInfo() << "Point line probes" << (normals ? "normals" : "boundary") << matched << "/" << probes
            << "pixels" << colored;
    require(probes >= 6 && matched * 4 >= probes * 3, "Point line projection probes failed");
}

struct State {
    int stage = 0;
    int frame = 0;
    int ticks = 0;
    minirender::Camera camera;
    minirender::PointCloud cloud;
    std::uint64_t uploads = 0;
    QImage first;
};
} // namespace

void verify_point_cloud_probe(Workbench& window, const QString& directory, const QString& label) {
    auto* viewport = window.viewport();
    const auto image = viewport->grab();
    require(image.save(directory + "/" + label + ".png"), "Cannot save point probe image");
    const auto& data = *viewport->point_preview_data();
    const auto& settings = viewport->point_preview_settings();
    require(!settings.show_normals && !settings.show_boundaries,
            "Point probes require isolated points");
    std::vector<glm::dvec3> screen;
    for (const auto& p : data.points) {
        screen.push_back(project(viewport->camera(), p.position, image.size()));
    }
    std::size_t checked = 0;
    std::size_t matched = 0;
    const auto probes = std::min<std::size_t>(96, screen.size());
    for (std::size_t probe = 0; probe < probes; ++probe) {
        const auto center = screen[probe * screen.size() / probes];
        const int x = static_cast<int>(std::floor(center.x));
        const int y = static_cast<int>(std::floor(center.y));
        if (center.z < 0 || center.z > 1 || x < 0 || y < 0 || x >= image.width() ||
            y >= image.height()) {
            continue;
        }
        double depth = 1;
        bool stable = true;
        glm::vec3 color{.08F, .09F, .11F};
        for (std::size_t i = 0; i < screen.size(); ++i) {
            const auto p = screen[i];
            if (p.z < 0 || p.z > 1) {
                continue;
            }
            const double distance = glm::length(glm::dvec2(x + .5, y + .5) - glm::dvec2(p));
            stable = stable && std::abs(distance - settings.point_radius) > .75;
            if (distance < settings.point_radius && p.z <= depth) {
                depth = p.z;
                color = settings.normal_colors && data.has_normals
                            ? data.points[i].normal * .5F + .5F
                            : glm::vec3(.25F, .65F, .85F);
            }
        }
        if (stable) {
            ++checked;
            matched += close(image.pixelColor(x, y), color) ? 1 : 0;
        }
    }
    qInfo() << "Point source probes" << label << matched << "/" << checked;
    require(checked >= 8 && matched == checked, "Point source buffer differs from CPU projection");
}

void start_point_cloud_smoke(Workbench& window, const QString& directory,
                             bool measure_responsiveness) {
    require(QDir().mkpath(directory), "Cannot create point test output directory");
    const auto state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(60);
    QObject::connect(
        timer, &QTimer::timeout, &window,
        [state, timer, &window, directory, measure_responsiveness] {
            try {
                auto* viewport = window.viewport();
                require(++state->ticks < 2000 && viewport->failure().isEmpty(),
                        "Point preview timed out or Vulkan failed");
                if (!viewport->isValid() || viewport->rendered_frames() < state->frame + 4) {
                    return;
                }
                const auto advance = [&] {
                    ++state->stage;
                    state->frame = viewport->rendered_frames();
                };
                switch (state->stage) {
                case 0: {
                    state->camera = window.document().camera;
                    state->cloud.positions = {{-.75F, -.65F, 0}, {.75F, -.65F, 0}, {0, 0, .25F},
                                              {0, 0, -.25F},     {-1, 1, 0},       {1, 1, 0}};
                    state->cloud.normals = {{0, 0, 1}, {0, 1, 0},  {1, 0, 0},
                                            {0, 1, 0}, {0, 0, -1}, {0, -1, 0}};
                    minirender::MeshData mesh;
                    mesh.positions = state->cloud.positions;
                    minirender::MeshTriangle triangle;
                    triangle.corners = {
                        {{0, {0, 0, 1}, {}}, {1, {0, 0, 1}, {}}, {4, {0, 0, 1}, {}}}};
                    mesh.triangles.push_back(triangle);
                    viewport->set_point_preview_data(
                        minirender::prepare_point_preview(state->cloud, &mesh));
                    viewport->set_point_preview_settings({6, .25F, true, false, false});
                    viewport->set_point_preview_visible(true);
                    auto camera = viewport->camera();
                    camera.target = {0, -.175F, 0};
                    camera.position = {0, -.175F, 4};
                    viewport->set_camera(camera);
                    advance();
                    return;
                }
                case 1: {
                    state->first = verify_points(window, directory, "points-normal-colors");
                    require(close(state->first.pixelColor(state->first.width() / 2,
                                                          state->first.height() / 2),
                                  {1, .5F, .5F}),
                            "Far point overwrote the front point");
                    state->uploads = viewport->point_uploads();
                    const auto camera = viewport->camera();
                    viewport->set_scene(minirender::make_snapshot(window.document()));
                    require(viewport->camera().position == camera.position,
                            "Scene refresh reset the point camera");
                    viewport->set_point_preview_settings({9, .25F, false, false, false});
                    advance();
                    return;
                }
                case 2:
                    verify_points(window, directory, "points-constant-color");
                    require(viewport->point_uploads() == state->uploads,
                            "Point size/color reuploaded source data");
                    viewport->set_point_preview_settings({6, .25F, true, true, false});
                    advance();
                    return;
                case 3:
                    verify_lines(window, {1, .72F, .16F}, true);
                    require(viewport->grab().save(directory + "/points-normals.png"),
                            "Cannot save point normals");
                    viewport->set_point_preview_settings({6, .25F, true, false, true});
                    advance();
                    return;
                case 4:
                    verify_lines(window, {1, .27F, .16F}, false);
                    require(viewport->grab().save(directory + "/points-boundary.png"),
                            "Cannot save point boundary");
                    require(window.document().camera.position == state->camera.position &&
                                window.document().camera.target == state->camera.target,
                            "Point navigation modified the scene camera");
                    viewport->set_offline_visible(true);
                    require(!viewport->point_preview_visible(),
                            "Offline display did not close point preview");
                    require(viewport->camera().position == state->camera.position,
                            "Point preview did not restore scene camera");
                    viewport->set_offline_visible(false);
                    viewport->set_point_preview_visible(true);
                    viewport->set_point_preview_settings({6, .25F, true, false, false});
                    advance();
                    return;
                case 5:
                    require(verify_points(window, directory, "points-restored") == state->first,
                            "Returning to point preview changed its camera or image");
                    require(viewport->point_uploads() == state->uploads,
                            "Display switches reuploaded point buffers");
                    {
                        auto camera = viewport->camera();
                        camera.position.x = .3F;
                        viewport->set_camera(camera);
                    }
                    advance();
                    return;
                case 6:
                    verify_points(window, directory, "points-camera");
                    for (auto& p : state->cloud.positions) {
                        p = p * 1e20F + glm::vec3(3e20F, -5e20F, 2e20F);
                    }
                    viewport->set_point_preview_data(
                        minirender::prepare_point_preview(state->cloud));
                    advance();
                    return;
                case 7:
                    verify_points(window, directory, "points-rebased");
                    require(viewport->point_uploads() == state->uploads + 1,
                            "Point replacement did not update buffers exactly once");
                    window.resize(window.width() + 100, window.height() + 80);
                    advance();
                    return;
                case 8:
                    verify_points(window, directory, "points-resized");
                    viewport->set_point_preview_data({});
                    require(!viewport->point_preview_visible(),
                            "Clearing point data did not restore the scene");
                    verify_geometry_gpu(window, directory, "points-return-scene");
                    qInfo() << "Points preview PASS: independent preview, depth/color, "
                               "normals, boundary, buffers, camera and scene isolation";
                    timer->stop();
                    start_point_cloud_editor_smoke(window, directory, measure_responsiveness);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Points smoke FAIL stage" << state->stage << ':' << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
