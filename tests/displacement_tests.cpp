#include <minirender/geometry/displacement.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/render/path_tracer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <limits>

using namespace minirender;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

// 面角法线刻意设为 +Y，检查位移使用由位置计算的 +Z 平滑方向。
std::shared_ptr<const MeshData> patch(std::size_t side = 1) {
    auto mesh = std::make_shared<MeshData>();
    for (std::size_t y = 0; y <= side; ++y) {
        for (std::size_t x = 0; x <= side; ++x) {
            mesh->positions.emplace_back(static_cast<float>(x), static_cast<float>(y), 0);
        }
    }
    for (std::size_t y = 0; y < side; ++y) {
        for (std::size_t x = 0; x < side; ++x) {
            const auto a = static_cast<MeshIndex>(y * (side + 1) + x);
            const auto b = a + 1, c = a + static_cast<MeshIndex>(side + 2),
                       d = a + static_cast<MeshIndex>(side + 1);
            for (const auto& indices : {std::array{a, b, c}, std::array{a, c, d}}) {
                MeshTriangle triangle;
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    triangle.corners[corner].position = indices[corner];
                    triangle.corners[corner].uv =
                        glm::vec2(mesh->positions[indices[corner]]) / static_cast<float>(side);
                }
                mesh->triangles.push_back(triangle);
            }
        }
    }
    return mesh;
}

std::shared_ptr<const Texture> constant_height(float value) {
    Image image(2, 2);
    for (std::size_t y = 0; y < 2; ++y) {
        for (std::size_t x = 0; x < 2; ++x) {
            image.at(x, y) = {value, 7, -3};
        }
    }
    return std::make_shared<const Texture>(image);
}

SceneDocument scene_for(std::shared_ptr<const MeshData> mesh) {
    SceneDocument scene;
    scene.environment = glm::vec3(1);
    scene.meshes.push_back({"mesh", {}, std::move(mesh)});
    scene.materials.push_back({"mat", "mat"});
    scene.materials[0].base_color = glm::vec3(0.7F);
    scene.objects.push_back({"object", "object", "mesh", "mat", {}, true});
    return scene;
}

} // namespace

TEST_CASE("Displacement moves real geometry along smooth normals using signed linear red",
          "[geometry][displacement]") {
    const auto source = patch();
    DisplacementSettings settings;
    settings.scale = 0.4;
    auto result = displace_mesh(source, constant_height(0.75F), settings);
    REQUIRE(result.mesh->positions.size() == 4);
    REQUIRE(result.mesh->triangles.size() == 2);
    REQUIRE(result.maximum_displacement == Approx(0.1));
    REQUIRE(result.topology.euler_characteristic == 1);
    REQUIRE(result.topology.boundary_edges == 4);
    for (std::size_t v = 0; v < source->positions.size(); ++v) {
        const auto p = result.mesh->positions[v];
        REQUIRE(p.x == source->positions[v].x);
        REQUIRE(p.y == source->positions[v].y);
        REQUIRE(p.z == Approx(0.1));
        REQUIRE(source->positions[v].z == 0);
    }
    for (const auto& triangle : result.mesh->triangles) {
        for (const auto& corner : triangle.corners) {
            REQUIRE(glm::length(corner.normal - glm::vec3(0, 0, 1)) < 1e-6F);
        }
    }
    settings.scale = -0.2;
    settings.midlevel = 0;
    result = displace_mesh(source, constant_height(-2), settings);
    REQUIRE(result.mesh->positions[0].z == Approx(0.4));
    settings.scale = 0;
    result = displace_mesh(source, constant_height(9), settings);
    REQUIRE(result.mesh == source);
    REQUIRE(result.mesh->positions == source->positions);
    REQUIRE(result.maximum_displacement == 0);
    result.mesh->validate();
}

TEST_CASE("Displacement sampling agrees with UV orientation repeat and bilinear texel centers",
          "[geometry][displacement]") {
    Image image(3, 2);
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            image.at(x, y) = {static_cast<float>(x + y * 3) / 5, 0, 0};
        }
    }
    const auto height = std::make_shared<const Texture>(image);
    DisplacementSettings settings;
    settings.scale = 1;
    settings.midlevel = 0;
    for (const auto uv : {glm::vec2(0), glm::vec2(1.0F / 6, 0.75F), glm::vec2(-0.3F, 2.8F),
                          glm::vec2(0.47F, 0.2F), glm::vec2(1, -1)}) {
        auto mesh = std::make_shared<MeshData>(*patch());
        for (auto& triangle : mesh->triangles) {
            for (auto& corner : triangle.corners) {
                corner.uv = uv;
            }
        }
        const auto result = displace_mesh(mesh, height, settings);
        REQUIRE(result.mesh->positions[0].z ==
                Approx(height->sample(uv, 0, TextureFilter::bilinear).r).margin(2e-7));
    }
}

TEST_CASE("Displacement reports inconsistent heights at shared vertices",
          "[geometry][displacement]") {
    auto mesh = std::make_shared<MeshData>(*patch());
    Image image(2, 2);
    image.at(0, 0) = glm::vec3(0);
    image.at(1, 0) = glm::vec3(1);
    image.at(0, 1) = glm::vec3(0);
    image.at(1, 1) = glm::vec3(1);
    mesh->triangles[1].corners[0].uv = {0.75F, 0.75F};
    const auto height = std::make_shared<const Texture>(image);
    REQUIRE_THROWS_WITH(displace_mesh(mesh, height), ContainsSubstring("at vertex"));
    REQUIRE(mesh->positions[0].z == 0);
}

TEST_CASE("Displacement checks edge interiors even when all endpoint heights match",
          "[geometry][displacement]") {
    auto mesh = std::make_shared<MeshData>(*patch());
    Image image(4, 4);
    image.at(1, 1) = glm::vec3(1);
    mesh->triangles[0].corners[0].uv = {0.125F, 0.875F};
    mesh->triangles[0].corners[1].uv = {0.5F, 0.875F};
    mesh->triangles[0].corners[2].uv = {0.875F, 0.875F};
    mesh->triangles[1].corners[0].uv = {0.125F, 0.625F};
    mesh->triangles[1].corners[1].uv = {0.875F, 0.625F};
    mesh->triangles[1].corners[2].uv = {0.125F, 0.125F};
    const auto height = std::make_shared<const Texture>(image);
    for (const auto& face : mesh->triangles) {
        for (const auto& corner : face.corners) {
            REQUIRE(height->sample(corner.uv, 0, TextureFilter::bilinear).r == 0);
        }
    }
    REQUIRE_THROWS_WITH(displace_mesh(mesh, height), ContainsSubstring("at edge"));
}

TEST_CASE("Displacement tests quadratic extrema between endpoints and midpoint",
          "[geometry][displacement]") {
    auto mesh = std::make_shared<MeshData>(*patch());
    Image image(2, 2);
    image.at(1, 1) = glm::vec3(1);
    // 在第一个面上 h(t)=(0.1+0.4t)*(0.7-0.4t)，t=0.75 时达到 0.16。
    // 另一个面恒为零。两端和中点均不超过0.15，只有内部极值越过0.155。
    for (auto& face : mesh->triangles) {
        for (auto& corner : face.corners) {
            corner.uv = {0.25F, 0.75F};
        }
    }
    mesh->triangles[0].corners[0].uv = {0.3F, 0.4F};
    mesh->triangles[0].corners[2].uv = {0.5F, 0.6F};
    const auto height = std::make_shared<const Texture>(image);
    DisplacementSettings settings;
    settings.scale = 1;
    settings.midlevel = 0;
    settings.seam_tolerance = 0.155;
    REQUIRE_THROWS_WITH(displace_mesh(mesh, height, settings), ContainsSubstring("at edge"));
    settings.seam_tolerance = 0.161;
    const auto result = displace_mesh(mesh, height, settings);
    REQUIRE(result.seam_edges == 1);
    REQUIRE(result.seam_segments == 1);
    REQUIRE(result.maximum_seam_difference == Approx(0.16).margin(2e-7));
    // 容差内仍选择第一次面角的高度，不把两侧的高度平均。
    REQUIRE(result.mesh->positions[0].z == Approx(0.07).margin(2e-7));
}

TEST_CASE("Displacement preserves periodic seams and bounds long UV scans",
          "[geometry][displacement]") {
    auto mesh = std::make_shared<MeshData>(*patch());
    for (auto& corner : mesh->triangles[1].corners) {
        corner.uv += glm::vec2(3, -2);
    }
    const auto height = constant_height(0.75F);
    const auto result = displace_mesh(mesh, height);
    REQUIRE(result.maximum_seam_difference == 0);
    REQUIRE(result.seam_segments == 0);
    mesh->triangles[1].corners[1].uv.x += 100000;
    DisplacementSettings settings;
    settings.maximum_seam_segments = 100;
    REQUIRE_THROWS_AS(displace_mesh(mesh, height, settings), std::length_error);
    settings.scale = 0;
    REQUIRE(displace_mesh(mesh, height, settings).mesh == mesh);
    settings.maximum_seam_segments = 1;
    REQUIRE_THROWS_AS(displace_mesh(mesh, height, settings), std::invalid_argument);
}

TEST_CASE("Displacement validates settings rejects reversal and avoids position overflow",
          "[geometry][displacement]") {
    const auto source = patch();
    const auto height = constant_height(1);
    REQUIRE_THROWS_AS(displace_mesh(nullptr, height), std::invalid_argument);
    REQUIRE_THROWS_AS(displace_mesh(source, nullptr), std::invalid_argument);
    DisplacementSettings settings;
    settings.midlevel = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS_AS(displace_mesh(source, height, settings), std::invalid_argument);
    settings = {};
    settings.seam_tolerance = -1;
    REQUIRE_THROWS_AS(displace_mesh(source, height, settings), std::invalid_argument);
    settings = {};
    settings.scale = std::numeric_limits<double>::max();
    REQUIRE_THROWS_AS(displace_mesh(source, height, settings), std::invalid_argument);

    auto tetra = std::make_shared<MeshData>();
    tetra->positions = {{1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}};
    for (const auto& indices : {std::array{0U, 2U, 1U}, std::array{0U, 1U, 3U},
                                std::array{0U, 3U, 2U}, std::array{1U, 2U, 3U}}) {
        MeshTriangle face;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            face.corners[corner].position = indices[corner];
            face.corners[corner].uv = {(static_cast<float>(indices[corner]) + 0.5F) / 4, 0.5F};
        }
        tetra->triangles.push_back(face);
    }
    Image asymmetric(4, 1);
    asymmetric.at(0, 0) = glm::vec3(1);
    settings = {};
    settings.scale = -8;
    settings.midlevel = 0;
    REQUIRE_THROWS_WITH(displace_mesh(tetra, std::make_shared<const Texture>(asymmetric), settings),
                        ContainsSubstring("reverses face"));
}

TEST_CASE("Displacement cancellation progress and previous immutable results survive all stages",
          "[geometry][displacement]") {
    const auto source = patch(32);
    const auto height = constant_height(0.6F);
    const auto previous = displace_mesh(source, height);
    const auto previous_position = previous.mesh->positions[0];
    std::vector<DisplacementProgress> progress;
    displace_mesh(source, height, {}, {}, [&](auto p) { progress.push_back(p); });
    REQUIRE(progress.front().stage == DisplacementStage::topology);
    REQUIRE(progress.back().stage == DisplacementStage::complete);
    for (std::size_t i = 0; i < progress.size(); ++i) {
        REQUIRE(progress[i].completed <= progress[i].total);
        if (i && progress[i].stage == progress[i - 1].stage) {
            REQUIRE(progress[i].completed >= progress[i - 1].completed);
        }
    }
    for (const auto stage :
         {DisplacementStage::topology, DisplacementStage::directions, DisplacementStage::samples,
          DisplacementStage::seams, DisplacementStage::positions, DisplacementStage::normals,
          DisplacementStage::complete}) {
        CancellationSource cancellation;
        bool reached = false;
        const bool scan =
            stage != DisplacementStage::topology && stage != DisplacementStage::complete;
        REQUIRE_THROWS_AS(displace_mesh(source, height, {}, cancellation.token(),
                                        [&](auto p) {
                                            if (p.stage == stage &&
                                                (!scan || p.completed >= 1024)) {
                                                reached = true;
                                                cancellation.request();
                                            }
                                        }),
                          OperationCancelled);
        REQUIRE(reached);
        REQUIRE(previous.mesh->positions[0] == previous_position);
        REQUIRE(source->positions[0].z == 0);
    }
}

TEST_CASE("Displacement changes existing BVH intersections shadow occlusion and paths",
          "[geometry][displacement]") {
    const auto source = patch();
    const auto result = displace_mesh(source, constant_height(0.75F), {0.4});
    const auto original = make_snapshot(scene_for(source));
    const auto displaced = make_snapshot(scene_for(result.mesh));
    const SceneBvh before(original), after(displaced);
    const Ray ray{{0.2, 0.2, 2}, {0, 0, -1}};
    REQUIRE(before.intersect(ray)->distance == Approx(2));
    REQUIRE(after.intersect(ray)->distance == Approx(1.9));
    const Ray shadow{{0.2, 0.2, 0.05}, {0, 0, 1}};
    REQUIRE_FALSE(before.occluded(shadow, {0, 0.1}));
    REQUIRE(after.occluded(shadow, {0, 0.1}));
    IntegratorSettings settings;
    settings.mode = SamplingMode::bsdf;
    settings.maximum_depth = 2;
    settings.russian_roulette = false;
    const PathTracer tracer(displaced, settings);
    Sampler sampler(101);
    const auto value = tracer.trace(ray, sampler);
    REQUIRE(glm::length(value - glm::dvec3(0.7)) < 1e-6);
}
