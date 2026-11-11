#include <minirender/render/sppm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <iostream>
#include <numbers>

using namespace minirender;
namespace {

// 六面均匀发射且反射的封闭房间：每次反射都乘 rho，有限深度有独立几何级数解。
SceneDocument uniform_room() {
    SceneDocument scene;
    scene.camera.position = {0, 0, 0};
    scene.camera.target = {0, 0, 1};
    scene.camera.vertical_fov_degrees = 1;
    scene.environment = glm::vec3(0);
    auto mesh = std::make_shared<MeshData>();
    for (const auto n : {glm::dvec3(1, 0, 0), glm::dvec3(-1, 0, 0), glm::dvec3(0, 1, 0),
                         glm::dvec3(0, -1, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, 0, -1)}) {
        const ShadingFrame frame(n);
        const auto center = -n;
        const auto start = std::uint32_t(mesh->positions.size());
        for (const auto uv :
             {glm::dvec2(-1, -1), glm::dvec2(1, -1), glm::dvec2(1, 1), glm::dvec2(-1, 1)}) {
            mesh->positions.push_back(glm::vec3(center + frame.to_world({uv.x, uv.y, 0})));
        }
        for (const auto indices : {std::array{0U, 1U, 2U}, std::array{0U, 2U, 3U}}) {
            MeshTriangle triangle;
            for (std::size_t i = 0; i < 3; ++i) {
                triangle.corners[i] = {start + indices[i], glm::vec3(n), {}};
            }
            mesh->triangles.push_back(triangle);
        }
    }
    scene.meshes.push_back({"room", {}, mesh});
    Material material;
    material.id = "room";
    material.base_color = glm::vec3(.5F);
    material.emission = glm::vec3(.1F);
    material.double_sided = false;
    scene.materials.push_back(material);
    scene.objects.push_back({"room", "room", "room", "room", {}});
    return scene;
}
} // namespace

TEST_CASE("SPPM indirect density matches a closed room geometric series", "[sppm][energy]") {
    const auto snapshot = make_snapshot(uniform_room());
    for (const std::uint32_t depth : {2U, 3U}) {
        double mean = 0, m2 = 0;
        std::size_t count = 0;
        for (const std::uint64_t seed : {3ULL, 7ULL, 11ULL, 17ULL, 23ULL}) {
            IntegratorSettings settings;
            settings.kind = IntegratorKind::sppm;
            settings.maximum_depth = depth;
            settings.russian_roulette = false;
            settings.sppm.photons_per_iteration = 10000;
            settings.sppm.initial_radius = .4;
            SppmIntegrator renderer(snapshot, {1, 1, 64, seed}, settings);
            for (int i = 0; i < 64; ++i) {
                renderer.advance();
            }
            const auto& pixel = renderer.pixels()[0];
            const double indirect = pixel.tau.x / (double(renderer.statistics().emitted_photons) *
                                                   std::numbers::pi * pixel.radius * pixel.radius);
            const double delta = indirect - mean;
            mean += delta / double(++count);
            m2 += delta * (indirect - mean);
            REQUIRE(renderer.statistics().gathered_contributions > 0);
            REQUIRE(pixel.radius < .4);
        }
        const double expected = double(.1F) * (.25 + (depth == 3 ? .125 : 0));
        const double error = std::sqrt(m2 / double(count - 1) / double(count));
        std::cout << "SPPM energy depth=" << depth << " expected=" << expected << " mean=" << mean
                  << " stderr=" << error << '\n';
        INFO("mean=" << mean << " expected=" << expected << " stderr=" << error);
        REQUIRE(std::abs(mean - expected) < 5 * error + .001);
    }
}
