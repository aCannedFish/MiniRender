#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/render/path_tracer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <set>

using namespace minirender;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

// 测试网格保留共享位置；面角 UV 默认取局部 XY，法线占位值满足资产契约。
MeshData mesh_from(std::vector<glm::vec3> positions,
                   const std::vector<std::array<MeshIndex, 3>>& indices) {
    MeshData result;
    result.positions = std::move(positions);
    for (const auto& face : indices) {
        MeshTriangle triangle;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            triangle.corners[corner].position = face[corner];
            triangle.corners[corner].uv = glm::vec2(result.positions[face[corner]]);
        }
        result.triangles.push_back(triangle);
    }
    return result;
}

MeshData tetrahedron() {
    return mesh_from({{1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}},
                     {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
}

MeshData triangle() {
    return mesh_from({{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {{0, 1, 2}});
}

MeshData quad() {
    return mesh_from({{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}}, {{0, 1, 2}, {0, 2, 3}});
}

MeshData fan(MeshIndex count) {
    std::vector<glm::vec3> positions{{0.2F, -0.3F, 1.3F}};
    std::vector<std::array<MeshIndex, 3>> faces;
    for (MeshIndex i = 0; i < count; ++i) {
        const double angle = i * 2 * std::numbers::pi / count;
        positions.emplace_back(static_cast<float>(std::cos(angle) * 2 + 0.4),
                               static_cast<float>(std::sin(angle) * 3 - 0.7), 0);
        faces.push_back({0, i + 1, (i + 1) % count + 1});
    }
    return mesh_from(std::move(positions), faces);
}

MeshData grid(MeshIndex side) {
    std::vector<glm::vec3> positions;
    std::vector<std::array<MeshIndex, 3>> faces;
    for (MeshIndex y = 0; y <= side; ++y) {
        for (MeshIndex x = 0; x <= side; ++x) {
            positions.emplace_back(static_cast<float>(x), static_cast<float>(y), 0);
            if (x < side && y < side) {
                const auto a = y * (side + 1) + x;
                faces.push_back({a, a + 1, a + side + 2});
                faces.push_back({a, a + side + 2, a + side + 1});
            }
        }
    }
    return mesh_from(std::move(positions), faces);
}

void close(glm::vec3 actual, glm::dvec3 expected, double tolerance = 2e-6) {
    REQUIRE(glm::length(glm::dvec3(actual) - expected) < tolerance);
}

// 从外部遍历接口检查 twin、面环、边界环、顶点旋转环以及索引连续约定。
void topology_invariants(const HalfedgeMesh& mesh) {
    const auto hs = mesh.halfedges();
    for (std::size_t i = 0; i < hs.size(); ++i) {
        const auto& h = hs[i];
        REQUIRE(h.origin < mesh.vertices().size());
        REQUIRE(h.edge < mesh.edges().size());
        REQUIRE(h.twin < hs.size());
        REQUIRE(h.next < hs.size());
        REQUIRE(h.previous < hs.size());
        REQUIRE(hs[h.twin].twin == i);
        REQUIRE(hs[h.next].previous == i);
        REQUIRE(hs[h.previous].next == i);
        REQUIRE(hs[h.twin].edge == h.edge);
        REQUIRE(hs[h.next].origin == mesh.destination(static_cast<MeshIndex>(i)));
        REQUIRE(hs[h.next].face == h.face);
        if (!mesh.is_boundary(static_cast<MeshIndex>(i))) {
            REQUIRE(h.face == i / 3);
            REQUIRE(hs[hs[h.next].next].next == i);
        }
    }
    for (const auto& v : mesh.vertices()) {
        if (v.halfedge == invalid_mesh_index) {
            REQUIRE(v.valence == 0);
            continue;
        }
        std::set<MeshIndex> neighbors;
        auto current = v.halfedge;
        for (MeshIndex i = 0; i < v.valence; ++i) {
            REQUIRE(hs[current].origin == hs[v.halfedge].origin);
            REQUIRE(neighbors.insert(mesh.destination(current)).second);
            current = hs[hs[current].twin].next;
        }
        REQUIRE(current == v.halfedge);
    }
}

std::shared_ptr<const MeshData> immutable(MeshData mesh) {
    return std::make_shared<const MeshData>(std::move(mesh));
}

} // namespace

TEST_CASE("Halfedge topology covers closed open and disconnected surfaces",
          "[geometry][halfedge]") {
    SECTION("Closed tetrahedron") {
        const HalfedgeMesh mesh(tetrahedron());
        topology_invariants(mesh);
        const auto stats = mesh.statistics();
        REQUIRE(stats.referenced_vertices == 4);
        REQUIRE(stats.edges == 6);
        REQUIRE(stats.faces == 4);
        REQUIRE(stats.boundary_edges == 0);
        REQUIRE(stats.boundary_loops == 0);
        REQUIRE(stats.face_components == 1);
        REQUIRE(stats.euler_characteristic == 2);
        for (const auto& v : mesh.vertices()) {
            REQUIRE(v.valence == 3);
        }
    }
    SECTION("Open patch retains unused positions") {
        auto data = quad();
        data.positions.push_back({9, 8, 7});
        const HalfedgeMesh mesh(data);
        topology_invariants(mesh);
        const auto stats = mesh.statistics();
        REQUIRE(stats.referenced_vertices == 4);
        REQUIRE(stats.isolated_vertices == 1);
        REQUIRE(stats.edges == 5);
        REQUIRE(stats.faces == 2);
        REQUIRE(stats.boundary_edges == 4);
        REQUIRE(stats.boundary_loops == 1);
        REQUIRE(stats.euler_characteristic == 1);
        for (std::size_t vertex = 0; vertex < 4; ++vertex) {
            REQUIRE(mesh.is_boundary(mesh.vertices()[vertex].halfedge));
        }
    }
    SECTION("Coincident geometry with different indices stays disconnected") {
        auto data = triangle();
        data.positions.insert(data.positions.end(), {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}});
        data.triangles.push_back(data.triangles[0]);
        for (auto& corner : data.triangles.back().corners) {
            corner.position += 3;
        }
        const HalfedgeMesh mesh(data);
        topology_invariants(mesh);
        REQUIRE(mesh.statistics().face_components == 2);
        REQUIRE(mesh.statistics().boundary_loops == 2);
        REQUIRE(mesh.statistics().euler_characteristic == 2);
    }
    SECTION("Annulus has two boundary loops") {
        const auto data = mesh_from({{-2, -2, 0},
                                     {2, -2, 0},
                                     {2, 2, 0},
                                     {-2, 2, 0},
                                     {-1, -1, 0},
                                     {1, -1, 0},
                                     {1, 1, 0},
                                     {-1, 1, 0}},
                                    {{0, 1, 5},
                                     {0, 5, 4},
                                     {1, 2, 6},
                                     {1, 6, 5},
                                     {2, 3, 7},
                                     {2, 7, 6},
                                     {3, 0, 4},
                                     {3, 4, 7}});
        const auto result = subdivide_loop(immutable(data), {2});
        topology_invariants(HalfedgeMesh(*result.mesh));
        for (const auto& stats : result.levels) {
            REQUIRE(stats.euler_characteristic == 0);
            REQUIRE(stats.boundary_loops == 2);
            REQUIRE(stats.face_components == 1);
        }
    }
}

TEST_CASE("Halfedge topology rejects invalid data and nonmanifold fans", "[geometry][halfedge]") {
    SECTION("Invalid data") {
        REQUIRE_THROWS_AS(HalfedgeMesh(MeshData{}), std::invalid_argument);
        auto mesh = triangle();
        mesh.positions[0].x = std::numeric_limits<float>::infinity();
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("not finite"));
        mesh = triangle();
        mesh.triangles[0].corners[0].position = 9;
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("out of range"));
        mesh = triangle();
        mesh.triangles[0].corners[0].normal = glm::vec3(0);
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("normal or UV"));
        mesh = triangle();
        mesh.triangles[0].corners[0].uv.x = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("normal or UV"));
        mesh = triangle();
        mesh.positions[2] = mesh.positions[0];
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("degenerate"));
    }
    SECTION("Same edge direction") {
        auto mesh = quad();
        std::swap(mesh.triangles[1].corners[0], mesh.triangles[1].corners[1]);
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("inconsistent winding"));
    }
    SECTION("Three incident faces") {
        auto mesh = tetrahedron();
        mesh.positions.push_back({3, 3, 3});
        auto extra = mesh.triangles[0];
        extra.corners[2].position = 4;
        mesh.triangles.push_back(extra);
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("more than two"));
    }
    SECTION("Duplicate face with either direction") {
        auto mesh = triangle();
        mesh.triangles.push_back(mesh.triangles[0]);
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("duplicate face"));
        std::swap(mesh.triangles[1].corners[1], mesh.triangles[1].corners[2]);
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("duplicate face"));
    }
    SECTION("Two boundary fans share a vertex") {
        const auto mesh = mesh_from({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}},
                                    {{0, 1, 2}, {0, 3, 4}});
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("multiple boundary fans"));
    }
    SECTION("Two closed shells share a vertex") {
        auto mesh = tetrahedron();
        const auto other = tetrahedron();
        for (std::size_t i = 1; i < other.positions.size(); ++i) {
            mesh.positions.push_back(other.positions[i] + glm::vec3(5, 0, 0));
        }
        for (auto face : other.triangles) {
            for (auto& corner : face.corners) {
                if (corner.position != 0) {
                    corner.position += 3;
                }
            }
            mesh.triangles.push_back(face);
        }
        REQUIRE_THROWS_WITH(HalfedgeMesh(mesh), ContainsSubstring("disconnected incident fans"));
    }
}

TEST_CASE("Loop uses cubic boundary masks and preserves child winding", "[geometry][loop]") {
    const auto source = immutable(triangle());
    const auto result = subdivide_loop(source);
    REQUIRE(result.mesh->positions.size() == 6);
    REQUIRE(result.mesh->triangles.size() == 4);
    close(result.mesh->positions[0], {0.25, 0.25, 0});
    close(result.mesh->positions[1], {1.5, 0.25, 0});
    close(result.mesh->positions[2], {0.25, 1.5, 0});
    close(result.mesh->positions[3], {1, 0, 0});
    close(result.mesh->positions[4], {1, 1, 0});
    close(result.mesh->positions[5], {0, 1, 0});
    REQUIRE(result.levels[1].boundary_edges == 6);
    REQUIRE(result.levels[1].euler_characteristic == 1);
    for (const auto& face : result.mesh->triangles) {
        const auto a = result.mesh->positions[face.corners[0].position];
        const auto b = result.mesh->positions[face.corners[1].position];
        const auto c = result.mesh->positions[face.corners[2].position];
        REQUIRE(glm::cross(b - a, c - a).z > 0);
        for (const auto& corner : face.corners) {
            close(corner.normal, {0, 0, 1});
        }
    }
    close(source->positions[0], {0, 0, 0});
    REQUIRE(source->triangles.size() == 1);
    result.mesh->validate();
}

TEST_CASE("Loop original extraordinary masks agree with analytic coordinates", "[geometry][loop]") {
    for (const MeshIndex valence : {3U, 4U, 5U, 6U, 7U, 11U}) {
        const auto data = immutable(fan(valence));
        const auto result = subdivide_loop(data);
        glm::dvec3 sum(0);
        for (std::size_t i = 1; i < data->positions.size(); ++i) {
            sum += glm::dvec3(data->positions[i]);
        }
        const double lambda = (3 + 2 * std::cos(2 * std::numbers::pi / valence)) / 8;
        const double alpha = 3.0 / 8.0 + lambda * lambda;
        const auto expected = alpha * glm::dvec3(data->positions[0]) +
                              (1 - alpha) * sum / static_cast<double>(valence);
        close(result.mesh->positions[0], expected);
        if (valence == 4) {
            // 原始 n=4 的中心权重为 33/64，区别于 3/(8n) 的简化权重。
            REQUIRE(result.mesh->positions[0].z == Approx(1.3 * 33 / 64));
        }
        if (valence == 6) {
            REQUIRE(result.mesh->positions[0].z == Approx(1.3 * 5 / 8));
        }
        const HalfedgeMesh topology(*data);
        for (std::size_t edge = 0; edge < topology.edges().size(); ++edge) {
            const auto h = topology.edges()[edge].halfedge;
            const auto a = topology.halfedges()[h].origin;
            const auto b = topology.destination(h);
            if (a != 0 && b != 0) {
                continue;
            }
            const auto twin = topology.halfedges()[h].twin;
            const auto c = topology.halfedges()[topology.halfedges()[h].previous].origin;
            const auto d = topology.halfedges()[topology.halfedges()[twin].previous].origin;
            close(result.mesh->positions[data->positions.size() + edge],
                  0.375 * (glm::dvec3(data->positions[a]) + glm::dvec3(data->positions[b])) +
                      0.125 * (glm::dvec3(data->positions[c]) + glm::dvec3(data->positions[d])));
        }
    }
}

TEST_CASE("Loop retains closed topology and affine invariance across levels", "[geometry][loop]") {
    auto data = tetrahedron();
    const auto result = subdivide_loop(immutable(data), {4});
    std::size_t expected_faces = 4;
    for (const auto& stats : result.levels) {
        REQUIRE(stats.faces == expected_faces);
        REQUIRE(stats.edges == expected_faces * 3 / 2);
        REQUIRE(stats.referenced_vertices == expected_faces / 2 + 2);
        REQUIRE(stats.euler_characteristic == 2);
        REQUIRE(stats.boundary_edges == 0);
        REQUIRE(stats.face_components == 1);
        expected_faces *= 4;
    }
    topology_invariants(HalfedgeMesh(*result.mesh));
    const glm::dmat3 transform{{1.5, 0.2, 0}, {0.1, 0.9, 0.3}, {0, 0.2, 1.1}};
    const glm::dvec3 translation{300, -120, 45};
    for (auto& p : data.positions) {
        p = glm::vec3(transform * glm::dvec3(p) + translation);
    }
    const auto transformed = subdivide_loop(immutable(data), {4});
    for (std::size_t i = 0; i < result.mesh->positions.size(); ++i) {
        close(transformed.mesh->positions[i],
              transform * glm::dvec3(result.mesh->positions[i]) + translation, 5e-5);
    }
    for (const auto& face : result.mesh->triangles) {
        for (const auto& corner : face.corners) {
            REQUIRE(glm::dot(corner.normal, result.mesh->positions[corner.position]) > 0);
            REQUIRE(glm::length(corner.normal) == Approx(1).margin(1e-6));
        }
    }
}

TEST_CASE("Loop preserves face-varying UV seams on shared geometric edges", "[geometry][loop]") {
    auto data = quad();
    for (auto& corner : data.triangles[1].corners) {
        corner.uv += glm::vec2(10, 20);
        corner.normal = {0, 0, -1};
    }
    const auto result = subdivide_loop(immutable(data), {2});
    REQUIRE(result.mesh->positions.size() == 25);
    REQUIRE(result.mesh->triangles.size() == 32);
    const HalfedgeMesh topology(*result.mesh);
    topology_invariants(topology);
    REQUIRE(topology.statistics().boundary_edges == 16);
    REQUIRE(topology.statistics().boundary_loops == 1);
    // 两层后各原面有 16 个后代。接缝端点、边中点仍共享几何位置，UV 保持两侧差值。
    std::set<MeshIndex> first_positions;
    std::set<MeshIndex> second_positions;
    for (std::size_t face = 0; face < result.mesh->triangles.size(); ++face) {
        for (const auto& corner : result.mesh->triangles[face].corners) {
            (face < 16 ? first_positions : second_positions).insert(corner.position);
            REQUIRE(corner.uv.x >= (face < 16 ? 0 : 10));
            REQUIRE(corner.uv.x <= (face < 16 ? 2 : 12));
            close(corner.normal, {0, 0, 1});
        }
    }
    std::vector<MeshIndex> seam;
    std::set_intersection(first_positions.begin(), first_positions.end(), second_positions.begin(),
                          second_positions.end(), std::back_inserter(seam));
    REQUIRE(seam.size() == 5);
    for (const auto position : seam) {
        std::array<glm::vec2, 2> uv{};
        for (std::size_t face = 0; face < result.mesh->triangles.size(); ++face) {
            for (const auto& corner : result.mesh->triangles[face].corners) {
                if (corner.position == position) {
                    uv[face < 16 ? 0 : 1] = corner.uv;
                }
            }
        }
        REQUIRE(glm::length(uv[1] - uv[0] - glm::vec2(10, 20)) < 1e-6F);
    }
}

TEST_CASE("Loop validates budgets keeps zero levels and reports actual work", "[geometry][loop]") {
    auto data = quad();
    data.positions.push_back({8, 9, 10});
    const auto source = immutable(data);
    const auto same = subdivide_loop(source, {0});
    REQUIRE(same.mesh == source);
    REQUIRE(same.levels.size() == 1);
    const auto result = subdivide_loop(source);
    REQUIRE(result.mesh->positions[4] == data.positions[4]);
    REQUIRE(result.levels[1].isolated_vertices == 1);
    REQUIRE_THROWS_AS(subdivide_loop(nullptr), std::invalid_argument);
    REQUIRE_THROWS_AS(subdivide_loop(source, {9}), std::invalid_argument);
    REQUIRE_THROWS_AS(subdivide_loop(source, {1, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(subdivide_loop(source, {1, 16000001}), std::invalid_argument);
    REQUIRE_THROWS_AS(subdivide_loop(source, {1, 1}), std::length_error);
    REQUIRE_THROWS_AS(subdivide_loop(source, {8, 1000}), std::length_error);
    std::vector<LoopProgress> progress;
    subdivide_loop(source, {2}, {}, [&](const auto& p) { progress.push_back(p); });
    REQUIRE(progress.front().stage == LoopStage::input_topology);
    REQUIRE(progress.back().stage == LoopStage::complete);
    REQUIRE(progress.back().level == 2);
    for (std::size_t i = 0; i < progress.size(); ++i) {
        const auto& p = progress[i];
        REQUIRE(p.completed <= p.total);
        REQUIRE(p.total > 0);
        if (i && p.stage == progress[i - 1].stage && p.level == progress[i - 1].level) {
            REQUIRE(p.total == progress[i - 1].total);
            REQUIRE(p.completed >= progress[i - 1].completed);
        }
    }
}

TEST_CASE("Loop cancels stages without modifying input or prior results", "[geometry][loop]") {
    const auto source = immutable(grid(32));
    const auto previous = subdivide_loop(source);
    const auto previous_position = previous.mesh->positions[100];
    CancellationSource early;
    early.request();
    REQUIRE_THROWS_AS(HalfedgeMesh(*source, early.token()), OperationCancelled);
    REQUIRE_THROWS_AS(subdivide_loop(source, {}, early.token()), OperationCancelled);
    for (const auto stage :
         {LoopStage::input_topology, LoopStage::vertices, LoopStage::edges, LoopStage::faces,
          LoopStage::normals, LoopStage::output_topology, LoopStage::complete}) {
        CancellationSource cancellation;
        bool reached = false;
        const bool scan = stage == LoopStage::vertices || stage == LoopStage::edges ||
                          stage == LoopStage::faces || stage == LoopStage::normals;
        REQUIRE_THROWS_AS(subdivide_loop(source, {}, cancellation.token(),
                                         [&](const LoopProgress& p) {
                                             if (p.stage == stage &&
                                                 (!scan || p.completed >= 1024)) {
                                                 reached = true;
                                                 cancellation.request();
                                             }
                                         }),
                          OperationCancelled);
        REQUIRE(reached);
        REQUIRE(source->triangles.size() == 2048);
        REQUIRE(previous.mesh->positions[100] == previous_position);
    }
}

TEST_CASE("Loop rejects degenerate reversed and undefined-normal results", "[geometry][loop]") {
    SECTION("Subdivision falls below the mesh area threshold") {
        auto data = triangle();
        for (auto& p : data.positions) {
            p *= 1e-5F;
        }
        const auto source = immutable(data);
        REQUIRE_NOTHROW(HalfedgeMesh(*source));
        REQUIRE_THROWS_WITH(subdivide_loop(source), ContainsSubstring("is degenerate"));
    }
    SECTION("A narrow planar fan folds back after subdivision") {
        const auto data = mesh_from({{0, 0, 0},
                                     {0.2574F, 0, 0},
                                     {2.1718F, 0.1429F, 0},
                                     {-0.3983F, 0.2093F, 0},
                                     {-2.2815F, -1.2902F, 0},
                                     {0.2358F, -0.102F, 0}},
                                    {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 1}});
        REQUIRE_NOTHROW(HalfedgeMesh(data));
        REQUIRE_THROWS_WITH(subdivide_loop(immutable(data)), ContainsSubstring("reverses"));
    }
    SECTION("Overlapping folded faces cancel the smooth normal") {
        const auto data =
            mesh_from({{0, 0, 0}, {2, 0, 0}, {1, 1, 0}, {1, 1, 0}}, {{0, 1, 2}, {1, 0, 3}});
        REQUIRE_NOTHROW(HalfedgeMesh(data));
        REQUIRE_THROWS_WITH(subdivide_loop(immutable(data)), ContainsSubstring("smooth normal"));
    }
}

TEST_CASE("Loop output enters existing snapshots BVH and path tracing", "[geometry][loop]") {
    const auto source = immutable(tetrahedron());
    const auto result = subdivide_loop(source, {3});
    SceneDocument document;
    document.environment = glm::vec3(1);
    document.meshes.push_back({"shape", {}, result.mesh});
    document.materials.push_back({"mat", "mat"});
    document.materials[0].base_color = glm::vec3(0.6F);
    document.objects.push_back({"shape", "shape", "shape", "mat", {}, true});
    const auto scene = make_snapshot(document);
    const SceneBvh bvh(scene);
    REQUIRE(bvh.statistics().triangles == 256);
    const auto hit = bvh.intersect({{0, 0, 4}, {0, 0, -1}});
    REQUIRE(hit);
    REQUIRE(hit->front_face);
    REQUIRE(hit->position.z < 1);
    REQUIRE(hit->position.z > 0);
    REQUIRE(glm::length(hit->shading_normal) == Approx(1).margin(1e-6));
    IntegratorSettings settings;
    settings.mode = SamplingMode::bsdf;
    settings.maximum_depth = 2;
    settings.russian_roulette = false;
    const PathTracer tracer(scene, settings);
    Sampler random(71);
    const auto radiance = tracer.trace({{0, 0, 4}, {0, 0, -1}}, random);
    REQUIRE(glm::length(radiance - glm::dvec3(0.6)) < 1e-6);
    // 真实导入资产同样按原始共享索引处理，OBJ 的硬法线/UV 不拆开位置拓扑。
    const auto cube =
        load_obj(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/cube.obj");
    const auto cube_result = subdivide_loop(immutable(cube), {2});
    REQUIRE(cube_result.levels.front().referenced_vertices == 8);
    REQUIRE(cube_result.levels.back().faces == 192);
    REQUIRE(cube_result.levels.back().euler_characteristic == 2);
}
