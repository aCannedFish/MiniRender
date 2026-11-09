#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/remesh.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using namespace minirender;
using Catch::Approx;

namespace {

// 有界扰动保持输入面朝向，边界坐标固定；UV 是原平面的 XY 参数。
std::shared_ptr<const MeshData> grid(MeshIndex side, double jitter = 0) {
    auto mesh = std::make_shared<MeshData>();
    for (MeshIndex y = 0; y <= side; ++y) {
        for (MeshIndex x = 0; x <= side; ++x) {
            const bool interior = x > 0 && y > 0 && x < side && y < side;
            const double dx = interior ? jitter * std::sin(x * 17.0 + y * 13.0) : 0;
            const double dy = interior ? jitter * std::cos(x * 11.0 - y * 19.0) : 0;
            mesh->positions.emplace_back((x + dx) / side, (y + dy) / side, 0);
        }
    }
    for (MeshIndex y = 0; y < side; ++y) {
        for (MeshIndex x = 0; x < side; ++x) {
            const auto a = y * (side + 1) + x;
            for (const auto ids :
                 {std::array{a, a + 1, a + side + 2}, std::array{a, a + side + 2, a + side + 1}}) {
                MeshTriangle face;
                for (std::size_t i = 0; i < 3; ++i) {
                    face.corners[i] = {ids[i], {0, 0, 1}, glm::vec2(mesh->positions[ids[i]])};
                }
                mesh->triangles.push_back(face);
            }
        }
    }
    return mesh;
}

void same_topology(const RemeshResult& result) {
    REQUIRE(result.topology.euler_characteristic == result.input_topology.euler_characteristic);
    REQUIRE(result.topology.boundary_loops == result.input_topology.boundary_loops);
    REQUIRE(result.topology.face_components == result.input_topology.face_components);
    REQUIRE(result.topology.isolated_vertices == result.input_topology.isolated_vertices);
    const HalfedgeMesh checked(*result.mesh);
    REQUIRE(checked.statistics().faces == result.topology.faces);
}

void affine_uv(const MeshData& mesh, double tolerance = 3e-6) {
    for (const auto& face : mesh.triangles) {
        for (const auto& c : face.corners) {
            REQUIRE(glm::length(glm::dvec2(c.uv) - glm::dvec2(mesh.positions[c.position])) <
                    tolerance);
        }
    }
}

void equal_mesh(const MeshData& a, const MeshData& b) {
    REQUIRE(a.positions == b.positions);
    REQUIRE(a.triangles.size() == b.triangles.size());
    for (std::size_t i = 0; i < a.triangles.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            REQUIRE(a.triangles[i].corners[j].position == b.triangles[i].corners[j].position);
            REQUIRE(a.triangles[i].corners[j].uv == b.triangles[i].corners[j].uv);
            REQUIRE(a.triangles[i].corners[j].normal == b.triangles[i].corners[j].normal);
        }
    }
}

} // namespace

TEST_CASE("Remeshing splits long edges into a conforming planar triangulation",
          "[geometry][remesh]") {
    const auto input = grid(2);
    const auto original = *input;
    const auto result = remesh_isotropic(
        input, {.target_edge_length = 0.1, .iterations = 3, .maximum_triangles = 3000});
    INFO("splits=" << result.splits << " collapses=" << result.collapses
                   << " flips=" << result.flips);
    REQUIRE(result.splits > 0);
    REQUIRE(result.topology.faces > result.input_topology.faces);
    REQUIRE(result.after.maximum_edge < result.before.maximum_edge / 2);
    REQUIRE(result.after.in_target_range * 100 > result.after.edges * 70);
    REQUIRE(result.error.sampled_maximum < 1e-6);
    same_topology(result);
    affine_uv(*result.mesh);
    for (const auto p : input->positions) {
        if (p.x == 0 || p.y == 0 || p.x == 1 || p.y == 1) {
            REQUIRE(std::find(result.mesh->positions.begin(), result.mesh->positions.end(), p) !=
                    result.mesh->positions.end());
        }
    }
    equal_mesh(*input, original);
}

TEST_CASE("Remeshing collapses dense input while preserving boundaries and UV",
          "[geometry][remesh]") {
    const auto input = grid(10);
    const auto result = remesh_isotropic(input, {.target_edge_length = 0.2, .iterations = 4});
    INFO("faces=" << result.topology.faces << " collapses=" << result.collapses);
    REQUIRE(result.collapses > 0);
    REQUIRE(result.topology.faces < input->triangles.size());
    REQUIRE(result.after.mean_edge > result.before.mean_edge);
    REQUIRE(result.error.sampled_maximum < 1e-6);
    same_topology(result);
    affine_uv(*result.mesh);
}

TEST_CASE("Remeshing flips edges to improve fan valences", "[geometry][remesh]") {
    auto input = std::make_shared<MeshData>();
    input->positions.push_back({0, 0, 0});
    for (MeshIndex i = 0; i < 8; ++i) {
        const auto angle = i * 2 * std::numbers::pi / 8;
        input->positions.emplace_back(std::cos(angle), std::sin(angle), 0);
    }
    for (MeshIndex i = 0; i < 8; ++i) {
        MeshTriangle triangle;
        const std::array ids{MeshIndex{0}, i + 1, (i + 1) % 8 + 1};
        for (std::size_t j = 0; j < 3; ++j) {
            triangle.corners[j] = {ids[j], {0, 0, 1}, glm::vec2(input->positions[ids[j]])};
        }
        input->triangles.push_back(triangle);
    }
    RemeshSettings settings;
    settings.target_edge_length = 1;
    settings.iterations = 1;
    settings.smoothing_step = 0;
    const auto result = remesh_isotropic(input, settings);
    REQUIRE(result.splits == 0);
    REQUIRE(result.flips > 0);
    REQUIRE(result.topology.faces == input->triangles.size());
    REQUIRE(result.topology.boundary_edges == 8);
    same_topology(result);
    affine_uv(*result.mesh);
}

TEST_CASE("Remeshing improves edge distribution on an irregular plane", "[geometry][remesh]") {
    const auto input = grid(18, 0.23);
    const auto result = remesh_isotropic(input, {.target_edge_length = 1.0 / 18, .iterations = 5});
    INFO("before=" << result.before.edge_p10 << ',' << result.before.edge_p90
                   << " after=" << result.after.edge_p10 << ',' << result.after.edge_p90
                   << " angles=" << result.before.minimum_angle << ','
                   << result.after.minimum_angle);
    REQUIRE(result.vertex_updates > 0);
    REQUIRE(result.after.edge_p90 / result.after.edge_p10 <
            result.before.edge_p90 / result.before.edge_p10);
    REQUIRE(result.after.mean_angle_deviation < result.before.mean_angle_deviation);
    REQUIRE(result.after.in_target_range * 100 > result.after.edges * 70);
    REQUIRE(result.error.symmetric_rms < 1e-7);
    same_topology(result);
    affine_uv(*result.mesh);
    const auto repeat = remesh_isotropic(input, {.target_edge_length = 1.0 / 18, .iterations = 5});
    equal_mesh(*result.mesh, *repeat.mesh);
}

TEST_CASE("Remeshing retains UV and authored normal discontinuities", "[geometry][remesh]") {
    auto input = std::make_shared<MeshData>(*grid(8));
    for (auto& face : input->triangles) {
        const auto center = (input->positions[face.corners[0].position] +
                             input->positions[face.corners[1].position] +
                             input->positions[face.corners[2].position]) /
                            3.0F;
        if (center.x > 0.5F) {
            for (auto& c : face.corners) {
                c.uv.x += 2;
                c.normal = glm::normalize(glm::vec3(0, 0.2F, 1));
            }
        }
    }
    const auto result = remesh_isotropic(input, {.target_edge_length = 0.09, .iterations = 3});
    REQUIRE(result.splits > 0);
    REQUIRE(result.protected_vertices > 0);
    same_topology(result);
    for (const auto& face : input->triangles) {
        for (const auto& corner : face.corners) {
            const auto position = input->positions[corner.position];
            if (position.x != 0.5F) {
                continue;
            }
            bool found = false;
            for (const auto& after : result.mesh->triangles) {
                for (const auto& c : after.corners) {
                    found = found || (result.mesh->positions[c.position] == position &&
                                      c.uv == corner.uv && c.normal == corner.normal);
                }
            }
            REQUIRE(found);
        }
    }
    for (const auto& face : result.mesh->triangles) {
        const auto center_x = (result.mesh->positions[face.corners[0].position].x +
                               result.mesh->positions[face.corners[1].position].x +
                               result.mesh->positions[face.corners[2].position].x) /
                              3;
        for (const auto& c : face.corners) {
            const auto p = result.mesh->positions[c.position];
            REQUIRE(std::abs(c.uv.x - p.x - (center_x > 0.5F ? 2 : 0)) < 3e-6F);
        }
    }
}

TEST_CASE("Remeshing preserves a hard cube and projects curved output to its input",
          "[geometry][remesh]") {
    const auto cube = std::make_shared<const MeshData>(
        load_obj(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/cube.obj"));
    SECTION("Protected cube") {
        const auto result = remesh_isotropic(cube, {.target_edge_length = 0.3, .iterations = 3});
        REQUIRE(result.splits > 0);
        REQUIRE(result.topology.boundary_edges == 0);
        REQUIRE(result.topology.euler_characteristic == 2);
        REQUIRE(result.error.sampled_maximum < 1e-6);
        same_topology(result);
    }
    SECTION("Smooth closed surface") {
        auto input = std::make_shared<MeshData>(*subdivide_loop(cube, {.levels = 3}).mesh);
        for (auto& face : input->triangles) {
            for (auto& c : face.corners) {
                c.uv = {0, 0};
            }
        }
        const auto result = remesh_isotropic(input, {.target_edge_length = 0.16, .iterations = 4});
        REQUIRE(result.collapses > 0);
        REQUIRE(result.topology.boundary_edges == 0);
        same_topology(result);
        const MeshSurface surface(input);
        double maximum = 0;
        for (const auto p : result.mesh->positions) {
            maximum = std::max(maximum, std::sqrt(surface.closest(glm::dvec3(p)).distance_squared));
        }
        REQUIRE(maximum < 1e-6);
        REQUIRE(result.error.sampled_maximum < 0.06);
    }
}

TEST_CASE("Remeshing keeps separate nearby surfaces and isolated positions", "[geometry][remesh]") {
    auto input = std::make_shared<MeshData>(*grid(5));
    const auto second = grid(5);
    const auto offset = static_cast<MeshIndex>(input->positions.size());
    for (const auto p : second->positions) {
        input->positions.push_back(p + glm::vec3(0, 0, 0.0001F));
    }
    for (auto face : second->triangles) {
        for (auto& c : face.corners) {
            c.position += offset;
            c.uv.x += 4;
        }
        input->triangles.push_back(face);
    }
    const glm::vec3 isolated(100, 200, 300);
    input->positions.push_back(isolated);
    const auto result = remesh_isotropic(input, {.target_edge_length = 0.16, .iterations = 3});
    REQUIRE(result.topology.face_components == 2);
    REQUIRE(result.topology.isolated_vertices == 1);
    REQUIRE(std::find(result.mesh->positions.begin(), result.mesh->positions.end(), isolated) !=
            result.mesh->positions.end());
    same_topology(result);
    for (const auto& face : result.mesh->triangles) {
        for (const auto& c : face.corners) {
            const auto p = result.mesh->positions[c.position];
            REQUIRE((std::abs(p.z) < 1e-8F || std::abs(p.z - 0.0001F) < 1e-8F));
            REQUIRE(std::abs(c.uv.x - p.x - (p.z > 0.00005F ? 4 : 0)) < 3e-6F);
        }
    }
}

TEST_CASE("Remeshing no-op settings budgets and cancellation preserve the source",
          "[geometry][remesh]") {
    const auto input = grid(6, 0.2);
    const auto original = *input;
    const auto unchanged = remesh_isotropic(input, {.iterations = 0});
    REQUIRE(unchanged.mesh == input);
    REQUIRE(unchanged.target_edge_length == Approx(0.1));
    REQUIRE(unchanged.iterations_completed == 0);
    REQUIRE(unchanged.protected_vertices == 24);
    REQUIRE(unchanged.before.edges == unchanged.after.edges);
    REQUIRE_THROWS_AS(remesh_isotropic(nullptr), std::invalid_argument);
    REQUIRE_THROWS_AS(remesh_isotropic(input, {.target_edge_length = 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(remesh_isotropic(input, {.iterations = 21}), std::invalid_argument);
    REQUIRE_THROWS_AS(remesh_isotropic(input, {.maximum_triangles = 1}), std::length_error);
    REQUIRE_THROWS_AS(
        remesh_isotropic(input, {.target_edge_length = 0.001, .maximum_triangles = 100}),
        std::length_error);
    for (const auto stage :
         {RemeshStage::input_topology, RemeshStage::reference_surface, RemeshStage::edge_queue,
          RemeshStage::split, RemeshStage::collapse, RemeshStage::flip, RemeshStage::smooth,
          RemeshStage::projection, RemeshStage::output, RemeshStage::quality, RemeshStage::error,
          RemeshStage::complete}) {
        CancellationSource source;
        REQUIRE_THROWS_AS(remesh_isotropic(input, {.target_edge_length = 0.13, .iterations = 2},
                                           source.token(),
                                           [&](const auto& p) {
                                               REQUIRE(p.completed <= p.total);
                                               if (p.stage == stage) {
                                                   source.request();
                                               }
                                           }),
                          OperationCancelled);
    }
    equal_mesh(*input, original);
}
