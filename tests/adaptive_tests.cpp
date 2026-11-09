#include "geometry/loop_hierarchy.hpp"
#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/adaptive.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/normals.hpp>
#include <minirender/geometry/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <set>

using namespace minirender;

namespace {

// 平面扇同时包含异常价数内部点和开放边界，闭合形状另用实际 OBJ 检查。
std::shared_ptr<const MeshData> fan(MeshIndex valence) {
    auto mesh = std::make_shared<MeshData>();
    mesh->positions.push_back({0, 0, 0.2F});
    for (MeshIndex i = 0; i < valence; ++i) {
        const double angle = 2 * std::numbers::pi * i / valence;
        mesh->positions.emplace_back(std::cos(angle), std::sin(angle), 0);
    }
    for (MeshIndex i = 0; i < valence; ++i) {
        MeshTriangle triangle;
        triangle.corners = {
            {{0, {0, 0, 1}, {}}, {i + 1, {0, 0, 1}, {}}, {(i + 1) % valence + 1, {0, 0, 1}, {}}}};
        mesh->triangles.push_back(triangle);
    }
    mesh->validate();
    return mesh;
}

using VirtualFace = std::array<MeshIndex, 3>;

// 独立均匀实现实际生成网格；测试将相同参数三角形的顶点映射到虚拟层级 ID。
void compare_uniform(const std::shared_ptr<const MeshData>& input) {
    const HalfedgeMesh topology(*input);
    detail::LoopHierarchy hierarchy(input, topology, 200000);
    std::vector<VirtualFace> faces;
    for (const auto& triangle : input->triangles) {
        faces.push_back({triangle.corners[0].position, triangle.corners[1].position,
                         triangle.corners[2].position});
    }
    for (std::uint32_t level = 0; level <= 4; ++level) {
        const auto uniform = subdivide_loop(input, {.levels = level}).mesh;
        const HalfedgeMesh uniform_topology(*uniform);
        const auto normals = smooth_vertex_normals(*uniform);
        REQUIRE(faces.size() == uniform->triangles.size());
        std::vector<MeshIndex> mapping(uniform->positions.size(), invalid_mesh_index);
        for (std::size_t face = 0; face < faces.size(); ++face) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto index = uniform->triangles[face].corners[corner].position;
                const auto id = faces[face][corner];
                if (mapping[index] != invalid_mesh_index) {
                    REQUIRE(mapping[index] == id);
                }
                mapping[index] = id;
                REQUIRE(glm::length(hierarchy.control_position(id, level) -
                                    glm::dvec3(uniform->positions[index])) < 4e-6);
            }
        }
        for (MeshIndex index = 0; index < mapping.size(); ++index) {
            const auto id = mapping[index];
            if (id == invalid_mesh_index) {
                continue;
            }
            INFO("level=" << level << " vertex=" << index);
            const auto& ring = hierarchy.ring(id, level);
            REQUIRE(ring.neighbors.size() == uniform_topology.vertices()[index].valence);
            REQUIRE(ring.boundary ==
                    uniform_topology.is_boundary(uniform_topology.vertices()[index].halfedge));
            REQUIRE(glm::length(hierarchy.smooth_normal(id, level) - glm::dvec3(normals[index])) <
                    2e-5);
            std::set<MeshIndex> expected;
            const auto hs = uniform_topology.halfedges();
            const auto start = uniform_topology.vertices()[index].halfedge;
            auto current = start;
            do {
                expected.insert(mapping[uniform_topology.destination(current)]);
                current = hs[hs[current].twin].next;
            } while (current != start);
            REQUIRE(std::set<MeshIndex>(ring.neighbors.begin(), ring.neighbors.end()) == expected);
        }
        if (level == 4) {
            break;
        }
        std::vector<VirtualFace> children;
        for (const auto& f : faces) {
            const auto ab = hierarchy.midpoint(f[0], f[1], level);
            const auto bc = hierarchy.midpoint(f[1], f[2], level);
            const auto ca = hierarchy.midpoint(f[2], f[0], level);
            children.insert(children.end(),
                            {{f[0], ab, ca}, {f[1], bc, ab}, {f[2], ca, bc}, {ab, bc, ca}});
        }
        faces = std::move(children);
    }
}

} // namespace

TEST_CASE("Lazy Loop controls normals and topology match uniform refinement", "[adaptive][loop]") {
    for (const MeshIndex valence : {3U, 4U, 5U, 6U, 9U}) {
        compare_uniform(fan(valence));
    }
    compare_uniform(std::make_shared<const MeshData>(
        load_obj(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/cube.obj")));
}

TEST_CASE("Lazy Loop evaluates isolated and high level support without a dense mesh",
          "[adaptive][loop]") {
    auto input = std::make_shared<MeshData>(*fan(5));
    input->positions.push_back({8, -7, 6});
    const HalfedgeMesh topology(*input);
    detail::LoopHierarchy hierarchy(input, topology, 100000);
    const auto isolated = static_cast<MeshIndex>(input->positions.size() - 1);
    REQUIRE(hierarchy.control_position(isolated, 8) == glm::dvec3(8, -7, 6));
    REQUIRE(hierarchy.smooth_normal(isolated, 8) == glm::dvec3(0));
    REQUIRE(hierarchy.point_count() == input->positions.size());
    REQUIRE(glm::length(hierarchy.control_position(0, 8)) < 1);
    REQUIRE(hierarchy.point_count() == input->positions.size());
    REQUIRE(glm::length(hierarchy.smooth_normal(0, 6)) > 0.99);
    INFO("evaluated virtual points=" << hierarchy.point_count());
    REQUIRE(hierarchy.point_count() < 3000);
}

TEST_CASE("Lazy Loop rejects invalid queries and honors evaluation budgets and cancellation",
          "[adaptive][loop]") {
    const auto input = fan(5);
    const HalfedgeMesh topology(*input);
    detail::LoopHierarchy hierarchy(input, topology, input->positions.size());
    REQUIRE_THROWS_AS(hierarchy.midpoint(0, 1, 0), std::length_error);
    REQUIRE_THROWS_AS(hierarchy.control_position(0, 9), std::invalid_argument);
    REQUIRE_THROWS_AS(hierarchy.midpoint(0, 0, 0), std::invalid_argument);
    CancellationSource cancellation;
    detail::LoopHierarchy cancellable(input, topology, 10000, cancellation.token());
    cancellation.request();
    REQUIRE_THROWS_AS(cancellable.control_position(0, 4), OperationCancelled);
    REQUIRE_THROWS_AS(cancellable.midpoint(0, 1, 0), OperationCancelled);
    REQUIRE_THROWS_AS(cancellable.ring(0, 2), OperationCancelled);
    REQUIRE(input->positions.size() == 6);
}

TEST_CASE("Adaptive Loop emits a conforming approximation with fewer triangles", "[adaptive]") {
    const auto input = std::make_shared<const MeshData>(
        load_obj(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/cube.obj"));
    const AdaptiveSettings settings{.maximum_level = 4, .tolerance = 0.025};
    const auto result = subdivide_adaptive(input, {}, settings);
    INFO("output=" << result.topology.faces << " transitions=" << result.transition_triangles
                   << " error=" << result.maximum_sampled_error
                   << " points=" << result.evaluation_points);
    REQUIRE(result.topology.faces > input->triangles.size());
    REQUIRE(result.topology.faces < input->triangles.size() * 256);
    REQUIRE(result.topology.euler_characteristic == 2);
    REQUIRE(result.topology.boundary_loops == 0);
    REQUIRE(result.maximum_sampled_error <= settings.tolerance);
    REQUIRE(result.triangle_levels.size() == result.topology.faces);
    REQUIRE(std::accumulate(result.triangles_per_level.begin(), result.triangles_per_level.end(),
                            std::size_t(0)) == result.topology.faces);
    result.mesh->validate();
    const auto repeated = subdivide_adaptive(input, {}, settings);
    REQUIRE(repeated.mesh->positions == result.mesh->positions);
    REQUIRE(repeated.triangle_levels == result.triangle_levels);
}

TEST_CASE("Adaptive uniform levels retain the original Loop target", "[adaptive]") {
    const auto input = fan(5);
    const auto result = subdivide_adaptive(input, {}, {.minimum_level = 2, .maximum_level = 2});
    const auto uniform = subdivide_loop(input, {.levels = 2}).mesh;
    REQUIRE(result.topology.faces == uniform->triangles.size());
    REQUIRE(result.transition_triangles == 0);
    REQUIRE(result.triangles_per_level[2] == result.topology.faces);
    REQUIRE(result.maximum_sampled_error < 1e-6);
    REQUIRE(result.mesh->positions.size() == uniform->positions.size());
    for (const auto p : uniform->positions) {
        double closest = std::numeric_limits<double>::infinity();
        for (const auto q : result.mesh->positions) {
            closest = std::min(closest, glm::length(glm::dvec3(p) - glm::dvec3(q)));
        }
        REQUIRE(closest < 4e-6);
    }
    REQUIRE(subdivide_adaptive(input, {}, {.maximum_level = 0}).mesh == input);
}

TEST_CASE("Adaptive budgets and cancellation preserve the source", "[adaptive]") {
    const auto input = fan(5);
    const auto original = *input;
    REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, {.tolerance = 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, {.maximum_level = 9}), std::invalid_argument);
    REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, {.maximum_triangles = 1}), std::length_error);
    REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, {.tolerance = 1e-6, .maximum_triangles = 5}),
                      std::length_error);
    for (const auto stage :
         {AdaptiveStage::topology, AdaptiveStage::sampling, AdaptiveStage::refinement,
          AdaptiveStage::balance, AdaptiveStage::transitions, AdaptiveStage::normals,
          AdaptiveStage::output, AdaptiveStage::complete}) {
        CancellationSource cancellation;
        bool reached = false;
        REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, {.maximum_level = 3, .tolerance = .01},
                                             cancellation.token(),
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
    REQUIRE(input->positions == original.positions);
}

namespace {
std::shared_ptr<const MeshData> adaptive_patch(MeshIndex side) {
    auto mesh = std::make_shared<MeshData>();
    for (MeshIndex y = 0; y <= side; ++y) {
        for (MeshIndex x = 0; x <= side; ++x) {
            mesh->positions.emplace_back(static_cast<float>(x) / static_cast<float>(side),
                                         static_cast<float>(y) / static_cast<float>(side), 0);
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
} // namespace

TEST_CASE("Adaptive displacement resolves a localized texture feature with conforming transitions",
          "[adaptive]") {
    const auto input = adaptive_patch(3);
    Image image(32, 32);
    for (std::size_t y = 0; y < 32; ++y) {
        for (std::size_t x = 0; x < 32; ++x) {
            const double u = (static_cast<double>(x) + .5) / 32;
            const double v = 1 - (static_cast<double>(y) + .5) / 32;
            image.at(x, y) = glm::vec3(static_cast<float>(
                .5 + .8 * std::exp(-((u - .68) * (u - .68) + (v - .62) * (v - .62)) / .008)));
        }
    }
    const auto texture = std::make_shared<const Texture>(image);
    AdaptiveSettings settings;
    settings.maximum_level = 6;
    settings.tolerance = .008;
    settings.displacement = DisplacementSettings{.scale = .3};
    const auto result = subdivide_adaptive(input, texture, settings);
    INFO("faces=" << result.topology.faces << " error=" << result.maximum_sampled_error
                  << " transitions=" << result.transition_triangles
                  << " points=" << result.evaluation_points);
    REQUIRE(result.topology.faces < input->triangles.size() * 256);
    REQUIRE(result.transition_triangles > 0);
    REQUIRE(result.topology.boundary_loops == 1);
    REQUIRE(result.topology.euler_characteristic == 1);
    REQUIRE(result.maximum_displacement > .15);
    REQUIRE(result.maximum_sampled_error <= settings.tolerance);
    std::size_t levels = 0;
    for (const auto count : result.triangles_per_level) {
        levels += count > 0 ? 1 : 0;
    }
    REQUIRE(levels > 1);
    REQUIRE(std::any_of(result.mesh->positions.begin(), result.mesh->positions.end(),
                        [](auto p) { return p.z > .15; }));
    const auto fine = subdivide_loop(input, {.levels = 6}).mesh;
    const auto reference = displace_mesh(fine, texture, *settings.displacement).mesh;
    const auto comparison = compare_surfaces(reference, result.mesh);
    INFO("independent dense maximum=" << comparison.sampled_maximum
                                      << " eval points=" << result.evaluation_points);
    REQUIRE(comparison.sampled_maximum < settings.tolerance * 1.5);
    REQUIRE(result.evaluation_points < reference->positions.size());
    const MeshSurface dense(reference);
    for (const auto p : result.mesh->positions) {
        REQUIRE(std::sqrt(dense.closest(glm::dvec3(p)).distance_squared) < 2e-5);
    }
    auto limited = settings;
    limited.maximum_level = 2;
    limited.tolerance = .0001;
    const auto coarse = subdivide_adaptive(input, texture, limited);
    REQUIRE(coarse.maximum_level_reached);
    REQUIRE(coarse.maximum_sampled_error > limited.tolerance);
}

TEST_CASE("Adaptive projected error uses the fixed viewport and validates settings", "[adaptive]") {
    const auto input = fan(5);
    AdaptiveSettings settings;
    settings.maximum_level = 3;
    settings.metric = AdaptiveMetric::screen_pixels;
    settings.tolerance = 2;
    REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, settings), std::invalid_argument);
    settings.projection = AdaptiveProjection{.viewport_pixels = {128, 128}};
    const auto small = subdivide_adaptive(input, {}, settings);
    settings.projection->viewport_pixels = {512, 512};
    const auto large = subdivide_adaptive(input, {}, settings);
    REQUIRE(large.topology.faces > small.topology.faces);
    REQUIRE(large.maximum_sampled_error <= settings.tolerance);
    REQUIRE(large.metric == AdaptiveMetric::screen_pixels);
    settings.projection->viewport_pixels.x = 0;
    REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, settings), std::invalid_argument);
}

TEST_CASE("Adaptive displacement checks seam interiors before refinement", "[adaptive]") {
    auto mesh = std::make_shared<MeshData>(*adaptive_patch(1));
    Image image(4, 4);
    image.at(1, 1) = glm::vec3(1);
    mesh->triangles[0].corners[0].uv = {.125F, .875F};
    mesh->triangles[0].corners[1].uv = {.5F, .875F};
    mesh->triangles[0].corners[2].uv = {.875F, .875F};
    mesh->triangles[1].corners[0].uv = {.125F, .625F};
    mesh->triangles[1].corners[1].uv = {.875F, .625F};
    mesh->triangles[1].corners[2].uv = {.125F, .125F};
    AdaptiveSettings settings;
    settings.displacement = DisplacementSettings{};
    const auto height = std::make_shared<const Texture>(image);
    REQUIRE_THROWS_AS(subdivide_adaptive(mesh, height, settings), std::invalid_argument);
    REQUIRE(mesh->positions[0].z == 0.0F);
}

TEST_CASE("Adaptive projection handles perspective mirrored transforms and near camera geometry",
          "[adaptive]") {
    const auto input = fan(5);
    Camera camera;
    camera.position = {2, 1.5F, 3};
    camera.target = {0, 0, 0};
    AdaptiveSettings settings;
    settings.maximum_level = 3;
    settings.metric = AdaptiveMetric::screen_pixels;
    settings.tolerance = 1.5;
    const auto clip = glm::dmat4(camera.projection_matrix(1)) * glm::dmat4(camera.view_matrix());
    settings.projection = AdaptiveProjection{clip, {256, 256}};
    const auto normal = subdivide_adaptive(input, {}, settings);
    REQUIRE(normal.maximum_sampled_error <= settings.tolerance);
    glm::dmat4 mirror(1);
    mirror[0][0] = -1;
    settings.projection->clip_from_object = clip * mirror;
    const auto mirrored = subdivide_adaptive(input, {}, settings);
    REQUIRE(mirrored.topology.euler_characteristic == 1);
    REQUIRE(mirrored.maximum_sampled_error <= settings.tolerance);
    camera.position = {0, 0, .03F};
    settings.projection->clip_from_object =
        glm::dmat4(camera.projection_matrix(1)) * glm::dmat4(camera.view_matrix());
    const auto near = subdivide_adaptive(input, {}, settings);
    REQUIRE(near.topology.boundary_loops == 1);
    near.mesh->validate();
    settings.projection->clip_from_object[0][0] = std::numeric_limits<double>::infinity();
    REQUIRE_THROWS_AS(subdivide_adaptive(input, {}, settings), std::invalid_argument);
}

TEST_CASE("Adaptive height scanning has explicit budgets and cancellation", "[adaptive]") {
    auto input = std::make_shared<MeshData>(*adaptive_patch(1));
    Image image(2, 2);
    image.at(1, 1) = glm::vec3(1);
    const auto height = std::make_shared<const Texture>(image);
    AdaptiveSettings settings;
    settings.maximum_level = 3;
    settings.displacement = DisplacementSettings{};
    CancellationSource source;
    bool reached = false;
    REQUIRE_THROWS_AS(subdivide_adaptive(input, height, settings, source.token(),
                                         [&](auto p) {
                                             if (p.stage == AdaptiveStage::seams) {
                                                 reached = true;
                                                 source.request();
                                             }
                                         }),
                      OperationCancelled);
    REQUIRE(reached);
    settings.maximum_evaluation_points = input->positions.size();
    REQUIRE_THROWS_AS(subdivide_adaptive(input, height, settings), std::length_error);
    for (auto& face : input->triangles) {
        for (auto& corner : face.corners) {
            corner.uv *= 1000000.0F;
        }
    }
    settings.maximum_evaluation_points = 1000;
    REQUIRE_THROWS_AS(subdivide_adaptive(input, height, settings), std::length_error);
}
