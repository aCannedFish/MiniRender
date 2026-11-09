#include "geometry/ball_geometry.hpp"
#include <minirender/geometry/ball_pivot.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/point_normals.hpp>
#include <minirender/geometry/remesh.hpp>
#include <minirender/geometry/simplify.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace minirender;

namespace {

// Fibonacci 球面提供已知外法线，检查与 PCA 独立的重建几何和拓扑。
std::shared_ptr<PointCloud> sphere(std::size_t count, glm::vec3 translation = {}) {
    auto cloud = std::make_shared<PointCloud>();
    for (std::size_t i = 0; i < count; ++i) {
        const double z = 1 - 2 * (static_cast<double>(i) + .5) / static_cast<double>(count);
        const double angle = static_cast<double>(i) * std::numbers::pi * (3 - std::sqrt(5.0));
        const double radius = std::sqrt(1 - z * z);
        const glm::vec3 p{radius * std::cos(angle), radius * std::sin(angle), z};
        cloud->positions.push_back(p + translation);
        cloud->normals.push_back(p);
    }
    return cloud;
}

// 平面点保留明确正面；可选缺口用于检查滚球是否跨越采样空白。
std::shared_ptr<PointCloud> plane_grid(int side, bool hole = false) {
    auto result = std::make_shared<PointCloud>();
    for (int y = 0; y <= side; ++y) {
        for (int x = 0; x <= side; ++x) {
            if (hole && x >= 8 && x <= 12 && y >= 8 && y <= 12) {
                continue;
            }
            result->positions.emplace_back(x * .1, y * .1, 0);
            result->normals.emplace_back(0, 0, 1);
        }
    }
    return result;
}
void verify_reconstruction(const PointCloud& input, const BallPivotResult& result) {
    REQUIRE(result.mesh);
    REQUIRE(result.source_indices.size() == result.mesh->positions.size());
    REQUIRE(result.unique_points == result.used_points + result.unused_points);
    REQUIRE(result.mesh->positions.size() == result.used_points + result.split_vertices);
    REQUIRE(result.seed_triangles + result.pivot_triangles == result.mesh->triangles.size());
    for (std::size_t i = 0; i < result.source_indices.size(); ++i) {
        REQUIRE(result.mesh->positions[i] == input.positions[result.source_indices[i]]);
    }
    const HalfedgeMesh topology(*result.mesh);
    REQUIRE(topology.statistics().boundary_loops == result.topology.boundary_loops);
    result.mesh->validate();
}
} // namespace

TEST_CASE("Contact balls and pivot angles match analytic geometry", "[points][bpa]") {
    const glm::dvec3 a{0, 0, 0};
    const glm::dvec3 b{1, 0, 0};
    const glm::dvec3 c{0, 1, 0};
    const auto balls = detail::contact_balls(a, b, c, 1);
    REQUIRE(balls);
    REQUIRE(glm::length(balls->circumcenter - glm::dvec3(.5, .5, 0)) < 1e-14);
    REQUIRE(glm::length(balls->outward - glm::dvec3(.5, .5, std::sqrt(.5))) < 1e-14);
    REQUIRE(balls->normal == glm::dvec3(0, 0, 1));
    for (const auto center : {balls->outward, balls->inward}) {
        for (const auto point : {a, b, c}) {
            REQUIRE(std::abs(glm::length(center - point) - 1) < 1e-14);
        }
    }
    REQUIRE(!detail::contact_balls(a, b, c, .7));
    REQUIRE(!detail::contact_balls(a, b, {2, 0, 0}, 3));
    REQUIRE(detail::contact_angle({1, 0, 0}, {0, 0, 0}, {0, 0, 0}) == 0.0);
    REQUIRE(std::abs(detail::contact_angle({1, 0, 0}, {0, 1, 0}, {0, 0, 1}) -
                     std::numbers::pi / 2) < 1e-14);
    REQUIRE(std::abs(detail::contact_angle({1, 0, 0}, {0, 0, 1}, {0, 1, 0}) +
                     std::numbers::pi / 2) < 1e-14);
}

TEST_CASE("Ball pivoting joins cocircular grid faces into one manifold patch", "[points][bpa]") {
    const auto cloud = plane_grid(8);
    const auto result = reconstruct_ball_pivot(cloud, {.radii = {.08}});
    verify_reconstruction(*cloud, result);
    REQUIRE(result.mesh->triangles.size() == 128);
    REQUIRE(result.topology.euler_characteristic == 1);
    REQUIRE(result.topology.boundary_loops == 1);
    REQUIRE(result.topology.face_components == 1);
    REQUIRE(result.unused_points == 0);
    REQUIRE(result.seed_triangles == 1);
    for (const auto& face : result.mesh->triangles) {
        const auto a = result.mesh->positions[face.corners[0].position];
        const auto b = result.mesh->positions[face.corners[1].position];
        const auto c = result.mesh->positions[face.corners[2].position];
        REQUIRE(glm::cross(b - a, c - a).z > 0.0F);
    }
}

TEST_CASE("Ball pivoting reconstructs closed spheres with outward normals", "[points][bpa]") {
    const auto cloud = sphere(700);
    const auto result = reconstruct_ball_pivot(cloud, {.radii = {.16}});
    verify_reconstruction(*cloud, result);
    INFO("faces=" << result.topology.faces << " boundaries=" << result.topology.boundary_edges
                  << " unused=" << result.unused_points << " tests=" << result.candidate_tests);
    REQUIRE(result.unused_points == 0);
    REQUIRE(result.topology.euler_characteristic == 2);
    REQUIRE(result.topology.boundary_edges == 0);
    REQUIRE(result.topology.face_components == 1);
    REQUIRE(result.mesh->triangles.size() == 1396);
    double maximum_interior_violation = 0;
    for (const auto& face : result.mesh->triangles) {
        const auto a = result.mesh->positions[face.corners[0].position];
        const auto b = result.mesh->positions[face.corners[1].position];
        const auto c = result.mesh->positions[face.corners[2].position];
        REQUIRE(glm::dot(glm::cross(b - a, c - a), a + b + c) > 0.0F);
        const auto balls = detail::contact_balls(glm::dvec3(a), glm::dvec3(b), glm::dvec3(c), .16);
        REQUIRE(balls);
        for (std::size_t point = 0; point < cloud->positions.size(); ++point) {
            bool corner = false;
            for (const auto& vertex : face.corners) {
                corner = corner || result.source_indices[vertex.position] == point;
            }
            if (!corner) {
                const auto delta = glm::dvec3(cloud->positions[point]) - balls->outward;
                maximum_interior_violation =
                    std::max(maximum_interior_violation, .16 * .16 - glm::dot(delta, delta));
            }
        }
    }
    REQUIRE(maximum_interior_violation < .16 * .16 * 1.1e-8);
    const auto repeat = reconstruct_ball_pivot(cloud, {.radii = {.16}});
    REQUIRE(repeat.mesh->positions == result.mesh->positions);
    for (std::size_t i = 0; i < repeat.mesh->triangles.size(); ++i) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            REQUIRE(repeat.mesh->triangles[i].corners[corner].position ==
                    result.mesh->triangles[i].corners[corner].position);
        }
    }
}

TEST_CASE("Ball pivoting retains unsupported holes instead of bridging them", "[points][bpa]") {
    const auto cloud = plane_grid(20, true);
    const auto result = reconstruct_ball_pivot(cloud, {.radii = {.08}});
    verify_reconstruction(*cloud, result);
    REQUIRE(result.topology.boundary_loops == 2);
    REQUIRE(result.topology.euler_characteristic == 0);
    REQUIRE(result.topology.face_components == 1);
    REQUIRE(result.split_vertices == 0);
    for (const auto& face : result.mesh->triangles) {
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto a = result.mesh->positions[face.corners[edge].position];
            const auto b = result.mesh->positions[face.corners[(edge + 1) % 3].position];
            REQUIRE(glm::length(a - b) < .160001F);
        }
    }
}

TEST_CASE("Ball pivoting closes a torus through front joins", "[points][bpa]") {
    auto cloud = std::make_shared<PointCloud>();
    for (int u = 0; u < 40; ++u) {
        for (int v = 0; v < 20; ++v) {
            const double theta = 2 * std::numbers::pi * u / 40;
            const double phi = 2 * std::numbers::pi * v / 20;
            const double radius = 1 + .35 * std::cos(phi);
            cloud->positions.emplace_back(radius * std::cos(theta), radius * std::sin(theta),
                                          .35 * std::sin(phi));
            cloud->normals.emplace_back(std::cos(phi) * std::cos(theta),
                                        std::cos(phi) * std::sin(theta), std::sin(phi));
        }
    }
    const auto result = reconstruct_ball_pivot(cloud, {.radii = {.18}});
    verify_reconstruction(*cloud, result);
    INFO("faces=" << result.topology.faces << " boundary=" << result.topology.boundary_edges
                  << " unused=" << result.unused_points << " split=" << result.split_vertices);
    REQUIRE(result.unused_points == 0);
    REQUIRE(result.split_vertices == 0);
    REQUIRE(result.topology.euler_characteristic == 0);
    REQUIRE(result.topology.boundary_edges == 0);
    REQUIRE(result.topology.face_components == 1);
    REQUIRE(result.topology.faces == 1600);
}

TEST_CASE("Unfinished front fans receive separate output vertices", "[points][bpa]") {
    auto cloud = std::make_shared<PointCloud>();
    cloud->positions.push_back({0, 0, 0});
    cloud->normals.push_back({0, 0, 1});
    for (const double sign : {-1.0, 1.0}) {
        for (const auto point :
             {glm::dvec3(.1, -.02, 0), glm::dvec3(.1, .02, 0), glm::dvec3(.2, -.04, 0),
              glm::dvec3(.2, .04, 0), glm::dvec3(.2, 0, 0)}) {
            cloud->positions.emplace_back(sign * point);
            cloud->normals.emplace_back(0, 0, 1);
        }
    }
    const auto result = reconstruct_ball_pivot(cloud, {.radii = {.06}});
    verify_reconstruction(*cloud, result);
    INFO("components=" << result.topology.face_components << " split=" << result.split_vertices
                       << " unused=" << result.unused_points);
    REQUIRE(result.split_vertices == 1);
    REQUIRE(result.topology.face_components == 2);
    REQUIRE(result.topology.boundary_loops == 2);
    REQUIRE(result.topology.euler_characteristic == 2);
    REQUIRE(result.unused_points == 0);
    REQUIRE(std::count(result.source_indices.begin(), result.source_indices.end(), 0U) == 2);
}

TEST_CASE("Multiple ball radii reactivate the front across uneven sample density",
          "[points][bpa]") {
    auto cloud = std::make_shared<PointCloud>();
    for (int y = 0; y <= 12; ++y) {
        for (int x = 0; x <= 15; ++x) {
            const double u = x <= 10 ? x * .1 : 1 + (x - 10) * .2;
            cloud->positions.emplace_back(u, y * .1, 0);
            cloud->normals.emplace_back(0, 0, 1);
        }
    }
    const auto small = reconstruct_ball_pivot(cloud, {.radii = {.08}});
    const auto multi = reconstruct_ball_pivot(cloud, {.radii = {.08, .16}});
    verify_reconstruction(*cloud, multi);
    REQUIRE(multi.mesh->triangles.size() > small.mesh->triangles.size());
    REQUIRE(multi.unused_points < small.unused_points);
    REQUIRE(multi.passes.size() == 2);
    REQUIRE(multi.passes[1].restarted_edges > 0);
    REQUIRE(multi.passes[1].new_triangles > 0);
    REQUIRE(multi.topology.boundary_loops == 1);
    REQUIRE(multi.topology.face_components == 1);
    REQUIRE(multi.unused_points == 0);
}

TEST_CASE("Ball pivoting reports unused duplicates and disconnected components", "[points][bpa]") {
    auto cloud = sphere(150);
    const auto other = sphere(150, {5, 0, 0});
    cloud->positions.insert(cloud->positions.end(), other->positions.begin(),
                            other->positions.end());
    cloud->normals.insert(cloud->normals.end(), other->normals.begin(), other->normals.end());
    cloud->positions.push_back(cloud->positions[0]);
    cloud->normals.push_back(cloud->normals[0]);
    cloud->positions.emplace_back(20, 20, 20);
    cloud->normals.emplace_back(0, 0, 1);
    const auto result = reconstruct_ball_pivot(cloud, {.radii = {.35}});
    verify_reconstruction(*cloud, result);
    REQUIRE(result.topology.face_components == 2);
    REQUIRE(result.topology.euler_characteristic == 4);
    REQUIRE(result.duplicate_points == 1);
    REQUIRE(result.unused_points == 1);
    REQUIRE(result.topology.boundary_edges == 0);
    REQUIRE(std::none_of(result.source_indices.begin(), result.source_indices.end(),
                         [](auto index) { return index >= 300; }));
}

TEST_CASE("Ball pivot budgets and each-stage cancellation preserve the point input",
          "[points][bpa]") {
    const auto cloud = plane_grid(8);
    const auto original = *cloud;
    REQUIRE_THROWS_AS(reconstruct_ball_pivot(cloud, {.radii = {.1, .05}}), std::invalid_argument);
    REQUIRE_THROWS_AS(reconstruct_ball_pivot(cloud, {.radii = {}}), std::invalid_argument);
    REQUIRE_THROWS_AS(reconstruct_ball_pivot(cloud, {.radii = {.08}, .maximum_triangles = 1}),
                      std::length_error);
    REQUIRE_THROWS_AS(reconstruct_ball_pivot(cloud, {.radii = {.08}, .maximum_candidate_tests = 2}),
                      std::length_error);
    REQUIRE_THROWS_AS(reconstruct_ball_pivot(cloud, {.radii = {.08}, .maximum_neighbors = 3}),
                      std::length_error);
    const auto unsupported = reconstruct_ball_pivot(cloud, {.radii = {.001}});
    REQUIRE(!unsupported.mesh);
    REQUIRE(unsupported.unused_points == cloud->positions.size());
    for (const auto stage :
         {BallPivotStage::validation, BallPivotStage::duplicates, BallPivotStage::index,
          BallPivotStage::reactivation, BallPivotStage::seeds, BallPivotStage::pivoting,
          BallPivotStage::output, BallPivotStage::complete}) {
        CancellationSource cancellation;
        bool reached = false;
        REQUIRE_THROWS_AS(reconstruct_ball_pivot(cloud, {.radii = {.08, .16}}, cancellation.token(),
                                                 [&](auto p) {
                                                     REQUIRE(p.completed <= p.total);
                                                     if (p.stage == stage) {
                                                         reached = true;
                                                         cancellation.request();
                                                     }
                                                 }),
                          OperationCancelled);
        REQUIRE(reached);
    }
    REQUIRE(cloud->positions == original.positions);
    REQUIRE(cloud->normals == original.normals);
}

TEST_CASE("Estimated point normals reconstruct assets accepted by the geometry pipeline",
          "[points][bpa]") {
    auto cloud = sphere(160);
    cloud->normals.clear();
    const auto prepared = prepare_point_normals(cloud, {.neighbors = 12});
    const auto reconstruction = reconstruct_ball_pivot(prepared.cloud, {.radii = {.35}});
    REQUIRE(reconstruction.mesh);
    REQUIRE(reconstruction.topology.boundary_edges == 0);
    const auto loop = subdivide_loop(reconstruction.mesh, {.levels = 1});
    REQUIRE(loop.mesh->triangles.size() == reconstruction.mesh->triangles.size() * 4);
    const auto simplified = simplify_qem(loop.mesh, {.target_triangles = 300});
    REQUIRE(simplified.collapses > 0);
    REQUIRE(simplified.topology.euler_characteristic == 2);
    const auto remeshed = remesh_isotropic(
        simplified.mesh, {.target_edge_length = .25, .iterations = 2, .maximum_triangles = 3000});
    REQUIRE(remeshed.topology.euler_characteristic == 2);
    REQUIRE(remeshed.topology.boundary_edges == 0);
    REQUIRE(remeshed.splits + remeshed.collapses + remeshed.flips > 0);
}
