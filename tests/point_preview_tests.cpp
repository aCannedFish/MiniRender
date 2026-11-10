#include <minirender/render/point_preview.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace minirender;

TEST_CASE("Point preview centers extreme coordinates and preserves source data",
          "[point-preview]") {
    PointCloud cloud;
    cloud.positions = {{1e30F, 1e30F, 0}, {1.1e30F, 1.05e30F, 0}, {1e30F, 1.1e30F, 0}};
    cloud.normals = {{0, 0, 2}, {0, 0, -3}, {0, 4, 0}};
    const auto original = cloud;
    const auto preview = prepare_point_preview(cloud, nullptr, {}, {}, 2);
    REQUIRE(preview->points.size() == 3);
    REQUIRE(preview->normal_samples.size() == 2);
    REQUIRE(preview->has_normals);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto expected =
            glm::vec3((glm::dvec3(cloud.positions[i]) - preview->center) * preview->scale);
        REQUIRE(preview->points[i].position == expected);
        REQUIRE(glm::all(glm::lessThanEqual(glm::abs(expected), glm::vec3(1.000001F))));
        REQUIRE(std::abs(glm::length(preview->points[i].normal) - 1) < 1e-6F);
    }
    REQUIRE(preview->normal_samples[0].position == preview->points[0].position);
    REQUIRE(preview->normal_samples[1].position == preview->points[1].position);
    REQUIRE(cloud.positions == original.positions);
    REQUIRE(cloud.normals == original.normals);
    cloud.positions = {{0, 0, 0}, {1e-40F, 0, 0}, {0, 1e-40F, 0}};
    REQUIRE(prepare_point_preview(cloud)->points[1].position == glm::vec3(1, -1, 0));
    cloud.positions = {{2, 3, 4}};
    cloud.normals.clear();
    const auto single = prepare_point_preview(cloud);
    REQUIRE(single->points[0].position == glm::vec3(0));
    REQUIRE(single->normal_samples.empty());
    REQUIRE(!single->has_normals);
}

TEST_CASE("Point preview extracts all boundary edges and honors cancellation", "[point-preview]") {
    PointCloud cloud;
    cloud.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    MeshData mesh;
    mesh.positions = cloud.positions;
    MeshTriangle face;
    for (std::uint32_t i = 0; i < 3; ++i) {
        face.corners[i] = {i, {0, 0, 1}, {0, 0}};
    }
    mesh.triangles.push_back(face);
    std::size_t completed = 0;
    const auto preview = prepare_point_preview(cloud, &mesh, {}, [&](auto done, auto total) {
        REQUIRE(done <= total);
        completed = done;
    });
    REQUIRE(completed == 9);
    REQUIRE(preview->boundary_edges == 3);
    REQUIRE(preview->boundary_vertices.size() == 6);
    const std::array<std::uint32_t, 6> endpoints{0, 1, 1, 2, 2, 0};
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        REQUIRE(preview->boundary_vertices[i].position == preview->points[endpoints[i]].position);
    }
    CancellationSource cancellation;
    REQUIRE_THROWS_AS(prepare_point_preview(cloud, &mesh, cancellation.token(),
                                            [&](auto, auto) { cancellation.request(); }),
                      OperationCancelled);
    PointPreviewSettings settings;
    settings.point_radius = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
}
