#include "cloth_smoke.hpp"
#include "cloth_material_editor_smoke.hpp"
#include "geometry_smoke.hpp"
#include "workbench.hpp"

#include <minirender/geometry/uv_tangents.hpp>
#include <minirender/render/path_tracer.hpp>
#include <minirender/simulation/cloth.hpp>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QTimer>

#include <cmath>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
minirender::SceneDocument fixture() {
    minirender::ClothGrid grid;
    grid.columns = grid.rows = 2;
    grid.origin = {-1, -1, 0};
    grid.span_u = {2, 0, 0};
    grid.span_v = {0, 2, 0};
    const auto frozen = minirender::ClothSimulation(grid).freeze();
    const auto preset = minirender::make_cloth_preset("silk");
    minirender::Material material;
    material.id = "fabric";
    material.kind = minirender::MaterialKind::cloth;
    material.base_color = glm::vec3(1);
    material.ior = static_cast<float>(preset.ior);
    material.cloth = preset.appearance;
    minirender::SceneDocument scene;
    scene.environment = glm::vec3(0);
    scene.camera.position = {0, 0, 3};
    scene.camera.target = {0, 0, 0};
    scene.materials.push_back(material);
    scene.meshes.push_back({"quad", {}, frozen.mesh});
    scene.objects.push_back({"cloth", "Cloth", "quad", "fabric", frozen.transform});
    scene.directional_lights.push_back({"sun", {-.4F, -.15F, -1}, glm::vec3(1), 2});
    return scene;
}

// 通过实际 CPU 求交获得法线与 UV 方向，调用完整 CPU 微圆柱响应核对浮点附件。
void verify(VulkanViewport& viewport, const QString& directory, const QString& label) {
    const auto image = viewport.grab_hdr();
    const auto scene = viewport.scene();
    const minirender::SceneBvh bvh(scene);
    std::size_t samples = 0, matched = 0;
    double maximum = 0;
    for (int gy = 0; gy < 70; ++gy) {
        for (int gx = 0; gx < 90; ++gx) {
            const int x = static_cast<int>((gx + .37) * double(image.width()) / 90);
            const int y = static_cast<int>((gy + .43) * double(image.height()) / 70);
            const auto ray = viewport.camera().generate_ray(
                {(x + .5) / double(image.width()), (y + .5) / double(image.height())},
                double(image.width()) / double(image.height()));
            const auto hit = bvh.intersect(ray);
            if (!hit ||
                std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < .025) {
                continue;
            }
            const auto& instance = scene->instances[hit->instance_index];
            auto normal = glm::normalize(glm::dvec3(hit->shading_normal));
            if (glm::dot(normal, -ray.direction) < 0) {
                normal = -normal;
            }
            const minirender::ShadingFrame frame(normal);
            const auto& face = instance.mesh->triangles[hit->triangle_index];
            std::array<glm::vec3, 3> positions;
            std::array<glm::vec2, 3> coordinates;
            for (std::size_t i = 0; i < 3; ++i) {
                positions[i] = instance.mesh->positions[face.corners[i].position];
                coordinates[i] = face.corners[i].uv;
            }
            const auto axes = minirender::uv_tangents(positions, coordinates);
            require(axes.has_value(), "Cloth fixture has invalid UV coordinates");
            const glm::dmat3 linear(instance.object_to_world);
            const auto texture = instance.base_color_texture
                                     ? instance.base_color_texture->sample(
                                           hit->uv, 0, instance.material.texture_filter)
                                     : glm::vec3(1);
            const auto dye = glm::dvec3(instance.material.base_color * texture);
            const minirender::ClothBsdf bsdf(
                instance.material.cloth, instance.material.ior, dye,
                {frame.to_local(linear * axes->u), frame.to_local(linear * axes->v)});
            glm::dvec3 expected(instance.material.emission);
            const auto& settings = instance.material.cloth->settings();
            const auto average = settings.coverage[0] * settings.threads[0].albedo +
                                 settings.coverage[1] * settings.threads[1].albedo;
            expected += glm::dvec3(scene->environment) * average * dye;
            for (const auto& light : scene->directional_lights) {
                const auto wi = frame.to_local(-glm::normalize(glm::dvec3(light.direction)));
                expected += bsdf.response(frame.to_local(-ray.direction), wi) *
                            glm::dvec3(light.color) * double(light.intensity);
            }
            const double error =
                glm::length(glm::dvec3(image.at(std::size_t(x), std::size_t(y))) - expected) /
                std::max(1.0, glm::length(expected));
            maximum = std::max(maximum, error);
            matched += error < .0005 ? 1 : 0;
            ++samples;
        }
    }
    qInfo() << "Cloth CPU/GPU" << label << matched << '/' << samples << "max relative RGB"
            << maximum;
    require(samples > 1000 && samples == matched, "GPU cloth response differs from CPU reference");
    image.write_exr(std::filesystem::path((directory + '/' + label + ".exr").toStdU16String()));
    require(viewport.grab().save(directory + '/' + label + ".png"), "Cannot save cloth preview");
}

struct State {
    minirender::SceneDocument scene = fixture();
    int stage = 0;
    int frame = 0;
    int ticks = 0;
    std::uint64_t uploads = 0, descriptors = 0;
};

} // namespace

void verify_cloth_gpu(Workbench& window, const QString& directory, const QString& label) {
    verify(*window.viewport(), directory, label);
}

void start_cloth_smoke(Workbench& window, const QString& directory, bool enforce_response) {
    QDir().mkpath(directory);
    auto state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(70);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            auto* viewport = window.viewport();
            try {
                require(++state->ticks < 1000 && viewport->failure().isEmpty(),
                        "Cloth smoke timed out");
                if (viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto advance = [&] {
                    viewport->set_scene(minirender::make_snapshot(state->scene));
                    state->frame = viewport->rendered_frames();
                    ++state->stage;
                };
                switch (state->stage) {
                case 0: {
                    viewport->set_shadows_enabled(false);
                    viewport->set_fxaa_enabled(false);
                    auto settings = viewport->ssao_settings();
                    settings.enabled = false;
                    viewport->set_ssao_settings(settings);
                    advance();
                    return;
                }
                case 1: {
                    verify(*viewport, directory, "cloth-silk");
                    state->uploads = viewport->upload_statistics().mesh_uploads;
                    state->descriptors = viewport->upload_statistics().descriptor_rebuilds;
                    auto settings = state->scene.materials[0].cloth->settings();
                    settings.rotation_degrees = 47;
                    state->scene.materials[0].cloth =
                        std::make_shared<const minirender::ClothAppearance>(settings);
                    advance();
                    return;
                }
                case 2: {
                    verify(*viewport, directory, "cloth-rotated");
                    require(viewport->upload_statistics().mesh_uploads == state->uploads &&
                                viewport->upload_statistics().descriptor_rebuilds ==
                                    state->descriptors,
                            "Cloth parameter edit rebuilt mesh or texture descriptors");
                    const auto preset = minirender::make_cloth_preset("shot");
                    state->scene.materials[0].cloth = preset.appearance;
                    state->scene.materials[0].ior = static_cast<float>(preset.ior);
                    advance();
                    return;
                }
                case 3: {
                    verify(*viewport, directory, "cloth-shot");
                    auto& material = state->scene.materials[0];
                    material.base_color = {.3F, .6F, .9F};
                    material.base_color_texture = "tint";
                    minirender::Image image(2, 2);
                    for (std::size_t y = 0; y < 2; ++y) {
                        for (std::size_t x = 0; x < 2; ++x) {
                            image.at(x, y) = {.5F, .25F, .75F};
                        }
                    }
                    state->scene.textures.push_back(
                        {"tint",
                         {},
                         minirender::ColorEncoding::linear,
                         std::make_shared<const minirender::Texture>(image)});
                    advance();
                    return;
                }
                case 4:
                    verify(*viewport, directory, "cloth-tint");
                    state->scene.objects[0].transform.rotation_degrees = {0, 15, 25};
                    state->scene.objects[0].transform.scale = {-1.2F, .8F, 1};
                    state->scene.objects[0].transform.translation =
                        -glm::mat3(state->scene.objects[0].transform.matrix()) * glm::vec3(1, 1, 0);
                    advance();
                    return;
                case 5:
                    verify(*viewport, directory, "cloth-mirrored");
                    state->scene.camera.position = {0, 0, -3};
                    state->scene.directional_lights[0].direction.z = 1;
                    advance();
                    return;
                case 6:
                    verify(*viewport, directory, "cloth-backface");
                    state->scene.environment = {.15F, .2F, .3F};
                    advance();
                    return;
                case 7:
                    verify(*viewport, directory, "cloth-ambient");
                    window.resize(1120, 760);
                    state->frame = viewport->rendered_frames();
                    ++state->stage;
                    return;
                case 8:
                    verify(*viewport, directory, "cloth-resized");
                    {
                        minirender::ClothEnvironment environment;
                        environment.spheres.push_back({});
                        environment.planes.push_back({});
                        minirender::ClothSimulation simulation({}, {}, environment);
                        simulation.advance(90);
                        const auto frozen = simulation.freeze();
                        state->scene = fixture();
                        state->scene.meshes[0].data = frozen.mesh;
                        state->scene.objects[0].transform = frozen.transform;
                        state->scene.camera.position = {1.9F, 1.8F, 2.3F};
                        state->scene.camera.target = {0, .7F, 0};
                        state->scene.camera.vertical_fov_degrees = 35;
                        const auto preset = minirender::make_cloth_preset("satin");
                        state->scene.materials[0].ior = static_cast<float>(preset.ior);
                        state->scene.materials[0].cloth = preset.appearance;
                    }
                    advance();
                    return;
                case 9:
                    verify(*viewport, directory, "cloth-draped-satin");
                    timer->stop();
                    qInfo()
                        << "Cloth GPU PASS: response, UV, transform, texture, backface and updates";
                    start_cloth_material_editor_smoke(window, directory, state->scene,
                                                      enforce_response);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Cloth smoke FAIL stage" << state->stage << ':' << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
