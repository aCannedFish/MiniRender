#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/simplify.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>

using namespace minirender;
using Catch::Approx;

namespace {

// 连续 XY UV 与共享位置构成一个带边界的平面；wave 非零时增加平滑高度。
std::shared_ptr<const MeshData> grid(MeshIndex side, float wave = 0) {
    auto mesh = std::make_shared<MeshData>();
    for (MeshIndex y = 0; y <= side; ++y) {
        for (MeshIndex x = 0; x <= side; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(side);
            const float v = static_cast<float>(y) / static_cast<float>(side);
            mesh->positions.emplace_back(u, v, wave * std::sin(u * 5) * std::sin(v * 5));
        }
    }
    for (MeshIndex y = 0; y < side; ++y) {
        for (MeshIndex x = 0; x < side; ++x) {
            const auto a = y * (side + 1) + x;
            for (const auto ids :
                 {std::array{a, a + 1, a + side + 2}, std::array{a, a + side + 2, a + side + 1}}) {
                MeshTriangle face;
                for (std::size_t i = 0; i < 3; ++i) {
                    face.corners[i].position = ids[i];
                    face.corners[i].uv = glm::vec2(mesh->positions[ids[i]]);
                    face.corners[i].normal = {0, 0, 1};
                }
                mesh->triangles.push_back(face);
            }
        }
    }
    return mesh;
}

std::shared_ptr<const MeshData> tetrahedron() {
    auto mesh = std::make_shared<MeshData>();
    mesh->positions = {{1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}};
    for (const auto ids : {std::array<MeshIndex, 3>{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}) {
        MeshTriangle face;
        for (std::size_t i = 0; i < 3; ++i) {
            face.corners[i].position = ids[i];
        }
        mesh->triangles.push_back(face);
    }
    return mesh;
}

SimplifySettings unprotected(std::size_t target) {
    SimplifySettings settings;
    settings.target_triangles = target;
    settings.preserve_boundary = false;
    settings.preserve_uv_seams = false;
    settings.preserve_hard_normals = false;
    settings.preserve_creases = false;
    return settings;
}

void same_topology(const TopologyStatistics& a, const TopologyStatistics& b) {
    REQUIRE(a.euler_characteristic == b.euler_characteristic);
    REQUIRE(a.boundary_loops == b.boundary_loops);
    REQUIRE(a.face_components == b.face_components);
}

} // namespace

TEST_CASE("Mesh surface queries recover analytic face edge and vertex projections",
          "[geometry][surface]") {
    auto mesh = std::make_shared<MeshData>();
    mesh->positions = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
    MeshTriangle face;
    for (MeshIndex i = 0; i < 3; ++i) {
        face.corners[i].position = i;
    }
    mesh->triangles.push_back(face);
    const MeshSurface surface(mesh);
    const auto inside = surface.closest({0.5, 0.5, 2});
    REQUIRE(inside.distance_squared == Approx(4));
    REQUIRE(glm::length(inside.barycentric - glm::dvec3(0.5, 0.25, 0.25)) < 1e-14);
    const auto edge = surface.closest({2, 2, -1});
    REQUIRE(glm::length(edge.position - glm::dvec3(1, 1, 0)) < 1e-14);
    REQUIRE(edge.distance_squared == Approx(3));
    const auto vertex = surface.closest({-1, -2, 3});
    REQUIRE(vertex.distance_squared == Approx(14));
    REQUIRE(glm::length(vertex.position) == 0);
    REQUIRE(surface.closest({0, 0, 0}).triangle == 0);
    REQUIRE_THROWS_AS(surface.closest({std::numeric_limits<double>::quiet_NaN(), 0, 0}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(MeshSurface(nullptr), std::invalid_argument);
}

TEST_CASE("Mesh surface tree matches planar analytic distances and brute force triangles",
          "[geometry][surface]") {
    const auto mesh = grid(13, 0.2F);
    const MeshSurface accelerated(mesh);
    std::vector<std::unique_ptr<MeshSurface>> triangles;
    for (const auto& face : mesh->triangles) {
        auto single = std::make_shared<MeshData>();
        single->positions = mesh->positions;
        single->triangles = {face};
        triangles.push_back(std::make_unique<MeshSurface>(single));
    }
    std::mt19937 random(101);
    std::uniform_real_distribution<double> uniform(-0.5, 1.5);
    for (int i = 0; i < 100; ++i) {
        const glm::dvec3 point(uniform(random), uniform(random), uniform(random));
        double expected = std::numeric_limits<double>::infinity();
        for (const auto& surface : triangles) {
            expected = std::min(expected, surface->closest(point).distance_squared);
        }
        const auto hit = accelerated.closest(point);
        REQUIRE(hit.distance_squared == Approx(expected).margin(1e-14));
        REQUIRE(std::abs(glm::dot(hit.barycentric, glm::dvec3(1)) - 1) < 1e-13);
    }
    const MeshSurface plane(grid(16));
    REQUIRE(plane.closest({0.31, 0.72, 0.4}).distance_squared == Approx(0.16));
    REQUIRE(plane.closest({1.3, -0.4, 0.5}).distance_squared == Approx(0.5));
}

TEST_CASE("QEM reduces planar meshes with exact boundaries and continuous UV",
          "[geometry][simplify]") {
    const auto mesh = grid(12);
    const auto original_positions = mesh->positions;
    SimplifySettings settings;
    settings.target_triangles = 90;
    const auto result = simplify_qem(mesh, settings);
    REQUIRE(result.stop == SimplifyStop::target_reached);
    REQUIRE(result.topology.faces <= settings.target_triangles);
    REQUIRE(result.collapses > 0);
    REQUIRE(result.protected_vertices == 48);
    same_topology(result.input_topology, result.topology);
    REQUIRE(result.error.symmetric_rms < 1e-12);
    REQUIRE(result.error.sampled_maximum < 1e-12);
    REQUIRE(result.error.original_to_result.samples == 169 + 288);
    REQUIRE(mesh->positions == original_positions);
    for (const auto& p : original_positions) {
        if (p.x == 0 || p.y == 0 || p.x == 1 || p.y == 1) {
            REQUIRE(std::find(result.mesh->positions.begin(), result.mesh->positions.end(), p) !=
                    result.mesh->positions.end());
        }
    }
    for (const auto& face : result.mesh->triangles) {
        for (const auto& c : face.corners) {
            REQUIRE(glm::length(c.uv - glm::vec2(result.mesh->positions[c.position])) < 1e-6F);
            REQUIRE(c.normal.z == Approx(1));
        }
    }
    const auto repeat = simplify_qem(mesh, settings);
    REQUIRE(repeat.mesh->positions == result.mesh->positions);
    for (std::size_t i = 0; i < result.mesh->triangles.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            REQUIRE(repeat.mesh->triangles[i].corners[j].position ==
                    result.mesh->triangles[i].corners[j].position);
        }
    }
}

TEST_CASE("QEM preserves closed topology and reports measured curved surface error",
          "[geometry][simplify]") {
    const auto refined = subdivide_loop(tetrahedron(), {.levels = 3}).mesh;
    const auto result = simplify_qem(refined, unprotected(51));
    REQUIRE(result.stop == SimplifyStop::target_reached);
    REQUIRE(result.topology.faces == 50);
    REQUIRE(result.collapses == (256 - 50) / 2);
    same_topology(result.input_topology, result.topology);
    REQUIRE(result.error.symmetric_rms > 0.001);
    REQUIRE(result.error.sampled_maximum >= result.error.symmetric_rms);
    REQUIRE(result.error.sampled_maximum < 0.15);
    REQUIRE(result.maximum_quadric_error > 0);
    result.mesh->validate();
}

TEST_CASE("QEM rejects tetrahedron duplicate faces and boundary pinches", "[geometry][simplify]") {
    const auto tetra = tetrahedron();
    const auto blocked = simplify_qem(tetra, unprotected(1));
    REQUIRE(blocked.mesh == tetra);
    REQUIRE(blocked.stop == SimplifyStop::constrained);
    REQUIRE(blocked.collapses == 0);
    const auto disk = grid(1);
    const auto triangle = simplify_qem(disk, unprotected(1));
    REQUIRE(triangle.topology.faces == 1);
    same_topology(triangle.input_topology, triangle.topology);
    const auto pinned = simplify_qem(disk, {.target_triangles = 1});
    REQUIRE(pinned.mesh == disk);
    REQUIRE(pinned.stop == SimplifyStop::constrained);
}

TEST_CASE("QEM keeps UV seams and authored hard normals", "[geometry][simplify]") {
    const auto original = grid(8);
    auto seams = std::make_shared<MeshData>(*original);
    // 右半面片使用另一个 UV 图块与硬法线，接缝位置仍为共享索引。
    for (auto& face : seams->triangles) {
        const auto center = (seams->positions[face.corners[0].position] +
                             seams->positions[face.corners[1].position] +
                             seams->positions[face.corners[2].position]) /
                            3.0F;
        if (center.x > 0.5F) {
            for (auto& c : face.corners) {
                c.uv.x += 2;
                c.normal = glm::normalize(glm::vec3(0, 0.2F, 1));
            }
        }
    }
    SimplifySettings settings;
    settings.target_triangles = 70;
    const auto result = simplify_qem(seams, settings);
    REQUIRE(result.collapses > 0);
    for (const auto& face : seams->triangles) {
        for (const auto& c : face.corners) {
            if (seams->positions[c.position].x != 0.5F) {
                continue;
            }
            bool found = false;
            for (const auto& output : result.mesh->triangles) {
                for (const auto& out : output.corners) {
                    if (result.mesh->positions[out.position] == seams->positions[c.position] &&
                        out.uv == c.uv && out.normal == c.normal) {
                        found = true;
                    }
                }
            }
            REQUIRE(found);
        }
    }
}

TEST_CASE("QEM error limits no-op and cancellation preserve immutable inputs",
          "[geometry][simplify]") {
    const auto mesh = grid(9, 0.4F);
    const auto no_op = simplify_qem(mesh, {.target_triangles = 1000});
    REQUIRE(no_op.mesh == mesh);
    REQUIRE(no_op.stop == SimplifyStop::target_reached);
    auto settings = unprotected(20);
    settings.maximum_quadric_error = 0;
    const auto limited = simplify_qem(mesh, settings);
    REQUIRE(limited.stop == SimplifyStop::error_limit);
    REQUIRE(limited.maximum_quadric_error == 0);
    REQUIRE(limited.topology.faces > 20);
    for (const auto stage :
         {SimplifyStage::quadrics, SimplifyStage::candidates, SimplifyStage::collapse,
          SimplifyStage::output, SimplifyStage::error}) {
        CancellationSource source;
        REQUIRE_THROWS_AS(simplify_qem(mesh, unprotected(30), source.token(),
                                       [&](auto p) {
                                           REQUIRE(p.completed <= p.total);
                                           if (p.stage == stage) {
                                               source.request();
                                           }
                                       }),
                          OperationCancelled);
    }
    CancellationSource cancelled;
    cancelled.request();
    REQUIRE_THROWS_AS(MeshSurface(mesh, cancelled.token()), OperationCancelled);
    REQUIRE_THROWS_AS(simplify_qem(nullptr), std::invalid_argument);
    REQUIRE_THROWS_AS(simplify_qem(mesh, {.target_triangles = 0}), std::invalid_argument);
    settings.maximum_quadric_error = -1;
    REQUIRE_THROWS_AS(simplify_qem(mesh, settings), std::invalid_argument);
    settings.maximum_quadric_error = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS_AS(simplify_qem(mesh, settings), std::invalid_argument);
}

TEST_CASE("Surface comparison measures two directions and ignores isolated positions",
          "[geometry][surface]") {
    const auto mesh = grid(3);
    auto translated = std::make_shared<MeshData>(*mesh);
    for (auto& p : translated->positions) {
        p.z = 0.25F;
    }
    translated->positions.push_back({1000, 1000, 1000});
    const auto distance = compare_surfaces(mesh, translated);
    REQUIRE(distance.original_to_result.samples == 16 + 18);
    REQUIRE(distance.result_to_original.samples == 16 + 18);
    REQUIRE(distance.symmetric_rms == Approx(0.25));
    REQUIRE(distance.sampled_maximum == Approx(0.25));
}

TEST_CASE("QEM preserves handles disconnected components and isolated positions",
          "[geometry][simplify]") {
    auto mesh = std::make_shared<MeshData>();
    constexpr MeshIndex around = 16, tube = 10;
    for (MeshIndex i = 0; i < around; ++i) {
        for (MeshIndex j = 0; j < tube; ++j) {
            const auto u = i * 2 * std::numbers::pi / around;
            const auto v = j * 2 * std::numbers::pi / tube;
            mesh->positions.emplace_back((2 + 0.6 * std::cos(v)) * std::cos(u),
                                         (2 + 0.6 * std::cos(v)) * std::sin(u), 0.6 * std::sin(v));
        }
    }
    for (MeshIndex i = 0; i < around; ++i) {
        for (MeshIndex j = 0; j < tube; ++j) {
            const auto a = i * tube + j, b = ((i + 1) % around) * tube + j;
            const auto c = ((i + 1) % around) * tube + (j + 1) % tube;
            const auto d = i * tube + (j + 1) % tube;
            for (auto indices : {std::array{a, b, c}, std::array{a, c, d}}) {
                MeshTriangle face;
                for (std::size_t k = 0; k < 3; ++k) {
                    face.corners[k].position = indices[k];
                }
                mesh->triangles.push_back(face);
            }
        }
    }
    const auto tetra = tetrahedron();
    const auto offset = static_cast<MeshIndex>(mesh->positions.size());
    for (const auto p : tetra->positions) {
        mesh->positions.push_back(p + glm::vec3(8, 0, 0));
    }
    for (auto face : tetra->triangles) {
        for (auto& c : face.corners) {
            c.position += offset;
        }
        mesh->triangles.push_back(face);
    }
    const glm::vec3 isolated(100, 200, 300);
    mesh->positions.push_back(isolated);
    const auto result = simplify_qem(mesh, unprotected(104));
    REQUIRE(result.stop == SimplifyStop::target_reached);
    REQUIRE(result.input_topology.face_components == 2);
    REQUIRE(result.topology.euler_characteristic == 2); // torus:0，tetrahedron:2。
    same_topology(result.input_topology, result.topology);
    REQUIRE(result.topology.isolated_vertices == 1);
    REQUIRE(std::find(result.mesh->positions.begin(), result.mesh->positions.end(), isolated) !=
            result.mesh->positions.end());
}

TEST_CASE("QEM geometric crease protection is independent of normal attributes",
          "[geometry][simplify]") {
    auto mesh = std::make_shared<MeshData>(*subdivide_loop(tetrahedron(), {.levels = 1}).mesh);
    for (auto& face : mesh->triangles) {
        for (auto& c : face.corners) {
            c.normal = {0, 0, 1};
        }
    }
    auto settings = unprotected(8);
    settings.preserve_creases = true;
    settings.crease_angle_degrees = 1;
    const auto protected_result = simplify_qem(mesh, settings);
    REQUIRE(protected_result.stop == SimplifyStop::constrained);
    REQUIRE(protected_result.mesh == mesh);
    settings.preserve_creases = false;
    const auto reduced = simplify_qem(mesh, settings);
    REQUIRE(reduced.topology.faces == 8);
    settings.crease_angle_degrees = 0;
    REQUIRE_THROWS_AS(simplify_qem(mesh, settings), std::invalid_argument);
}

TEST_CASE("QEM remains stable after large translations and keeps heightfield face orientation",
          "[geometry][simplify]") {
    auto mesh = std::make_shared<MeshData>(*grid(12, 0.25F));
    for (auto& p : mesh->positions) {
        p = p * 16.0F + glm::vec3(100000, -100000, 300000);
    }
    SimplifySettings settings;
    settings.target_triangles = 80;
    settings.preserve_creases = false;
    const auto result = simplify_qem(mesh, settings);
    REQUIRE(result.stop == SimplifyStop::target_reached);
    REQUIRE(std::isfinite(result.error.symmetric_rms));
    REQUIRE(result.error.sampled_maximum < 1);
    for (const auto& face : result.mesh->triangles) {
        const glm::dvec3 a = result.mesh->positions[face.corners[0].position];
        const glm::dvec3 b = result.mesh->positions[face.corners[1].position];
        const glm::dvec3 c = result.mesh->positions[face.corners[2].position];
        REQUIRE(glm::cross(b - a, c - a).z > 0);
    }
}
