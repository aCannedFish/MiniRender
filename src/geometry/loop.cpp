#include "loop_rules.hpp"
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/normals.hpp>

#include <stdexcept>
#include <string>

namespace minirender {
namespace {

// 使用相对位移求加权和，减少平移较大时的舍入误差；最后存回场景统一的 float 位置。
glm::vec3 even_position(const MeshData& mesh, const HalfedgeMesh& topology, MeshIndex vertex,
                        const CancellationToken& cancellation) {
    const auto& v = topology.vertices()[vertex];
    const glm::dvec3 p = mesh.positions[vertex];
    if (v.halfedge == invalid_mesh_index) {
        return glm::vec3(p);
    }
    const auto halfedges = topology.halfedges();
    if (topology.is_boundary(v.halfedge)) {
        const glm::dvec3 a = mesh.positions[topology.destination(v.halfedge)];
        const glm::dvec3 b = mesh.positions[halfedges[halfedges[v.halfedge].previous].origin];
        return glm::vec3(p + ((a - p) + (b - p)) / 8.0);
    }
    const double beta = detail::loop_beta(v.valence);
    glm::dvec3 relative_sum(0);
    auto current = v.halfedge;
    for (MeshIndex index = 0; index < v.valence; ++index) {
        if (index % 1024 == 0) {
            cancellation.check();
        }
        relative_sum += glm::dvec3(mesh.positions[topology.destination(current)]) - p;
        current = halfedges[halfedges[current].twin].next;
    }
    return glm::vec3(p + beta * relative_sum);
}

glm::vec3 odd_position(const MeshData& mesh, const HalfedgeMesh& topology, MeshIndex edge) {
    const auto halfedges = topology.halfedges();
    const auto h = topology.edges()[edge].halfedge;
    const auto twin = halfedges[h].twin;
    const glm::dvec3 a = mesh.positions[halfedges[h].origin];
    const glm::dvec3 b = mesh.positions[halfedges[twin].origin];
    if (topology.is_boundary(twin)) {
        return glm::vec3(a + (b - a) / 2.0);
    }
    const glm::dvec3 c = mesh.positions[halfedges[halfedges[h].previous].origin];
    const glm::dvec3 d = mesh.positions[halfedges[halfedges[twin].previous].origin];
    return glm::vec3(a + 3.0 / 8.0 * (b - a) + 1.0 / 8.0 * ((c - a) + (d - a)));
}

// 一个面独立生成 UV 中点，各面仍引用相同的几何边位置。子面绕序与父面一致。
void split_face(const MeshTriangle& parent, const HalfedgeMesh& topology, std::size_t face,
                MeshIndex old_vertices, std::span<MeshTriangle, 4> children) {
    std::array<MeshCorner, 6> corners;
    for (std::size_t corner = 0; corner < 3; ++corner) {
        corners[corner] = parent.corners[corner];
        auto& midpoint = corners[corner + 3];
        midpoint.position = old_vertices + topology.halfedges()[face * 3 + corner].edge;
        midpoint.uv = glm::vec2((glm::dvec2(parent.corners[corner].uv) +
                                 glm::dvec2(parent.corners[(corner + 1) % 3].uv)) /
                                2.0);
    }
    constexpr std::array<std::array<std::size_t, 3>, 4> indices{
        {{0, 3, 5}, {1, 4, 3}, {2, 5, 4}, {3, 4, 5}}};
    for (std::size_t child = 0; child < 4; ++child) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            children[child].corners[corner] = corners[indices[child][corner]];
        }
    }
}

} // namespace

void LoopSettings::validate() const {
    if (levels > 8 || maximum_triangles == 0 || maximum_triangles > 16000000) {
        throw std::invalid_argument("Loop requires levels 0..8 and a triangle budget 1..16000000");
    }
}

LoopResult subdivide_loop(std::shared_ptr<const MeshData> input, LoopSettings settings,
                          CancellationToken cancellation, LoopProgressCallback progress) {
    cancellation.check();
    settings.validate();
    if (!input) {
        throw std::invalid_argument("Loop input mesh is missing");
    }
    auto predicted = input->triangles.size();
    if (predicted > settings.maximum_triangles) {
        throw std::length_error("Loop input exceeds the triangle budget");
    }
    for (std::uint32_t level = 0; level < settings.levels; ++level) {
        if (predicted > settings.maximum_triangles / 4) {
            throw std::length_error("Loop result exceeds the triangle budget");
        }
        predicted *= 4;
    }
    // 回调前后都检查取消，支持控制器在回调中取消；异常直接交给调用方处理。
    const auto report = [&](LoopStage stage, std::uint32_t level, std::size_t completed,
                            std::size_t total) {
        cancellation.check();
        if (progress) {
            progress({stage, level, completed, total});
        }
        cancellation.check();
    };
    report(LoopStage::input_topology, 0, 0, 1);
    HalfedgeMesh topology(*input, cancellation);
    report(LoopStage::input_topology, 0, 1, 1);
    LoopResult result;
    result.mesh = std::move(input);
    result.levels.push_back(topology.statistics());
    for (std::uint32_t level = 1; level <= settings.levels; ++level) {
        const auto& source = *result.mesh;
        if (topology.edges().size() > invalid_mesh_index - source.positions.size()) {
            throw std::length_error("Loop positions exceed 32-bit index capacity");
        }
        auto next = std::make_shared<MeshData>();
        report(LoopStage::vertices, level, 0, source.positions.size());
        next->positions.resize(source.positions.size() + topology.edges().size());
        for (std::size_t vertex = 0; vertex < source.positions.size(); ++vertex) {
            next->positions[vertex] =
                even_position(source, topology, static_cast<MeshIndex>(vertex), cancellation);
            if ((vertex + 1) % 1024 == 0) {
                report(LoopStage::vertices, level, vertex + 1, source.positions.size());
            }
        }
        report(LoopStage::vertices, level, source.positions.size(), source.positions.size());
        report(LoopStage::edges, level, 0, topology.edges().size());
        for (std::size_t edge = 0; edge < topology.edges().size(); ++edge) {
            next->positions[source.positions.size() + edge] =
                odd_position(source, topology, static_cast<MeshIndex>(edge));
            if ((edge + 1) % 1024 == 0) {
                report(LoopStage::edges, level, edge + 1, topology.edges().size());
            }
        }
        report(LoopStage::edges, level, topology.edges().size(), topology.edges().size());
        report(LoopStage::faces, level, 0, source.triangles.size());
        next->triangles.resize(source.triangles.size() * 4);
        for (std::size_t face = 0; face < source.triangles.size(); ++face) {
            split_face(source.triangles[face], topology, face,
                       static_cast<MeshIndex>(source.positions.size()),
                       std::span<MeshTriangle, 4>(next->triangles.data() + face * 4, 4));
            const auto& parent = source.triangles[face].corners;
            const glm::dvec3 a = source.positions[parent[0].position];
            const glm::dvec3 b = source.positions[parent[1].position];
            const glm::dvec3 c = source.positions[parent[2].position];
            const auto parent_area = glm::cross(b - a, c - a);
            for (std::size_t child = face * 4; child < face * 4 + 4; ++child) {
                const auto& corners = next->triangles[child].corners;
                const glm::dvec3 x = next->positions[corners[0].position];
                const glm::dvec3 y = next->positions[corners[1].position];
                const glm::dvec3 z = next->positions[corners[2].position];
                const auto area = glm::cross(y - x, z - x);
                if (glm::dot(area, area) <= 1e-20) {
                    throw std::invalid_argument("Loop level " + std::to_string(level) + " face " +
                                                std::to_string(child) + " is degenerate");
                }
                // 要求子面法线在父面法线上的投影为正，不检测全局自交。
                if (glm::dot(area, parent_area) <= 0) {
                    throw std::invalid_argument("Loop level " + std::to_string(level) + " face " +
                                                std::to_string(child) +
                                                " reverses relative to its parent face");
                }
            }
            if ((face + 1) % 1024 == 0) {
                report(LoopStage::faces, level, face + 1, source.triangles.size());
            }
        }
        report(LoopStage::faces, level, source.triangles.size(), source.triangles.size());

        // 与真实位移共用面积加权法线，跨 UV 接缝共享方向，丢弃原有硬法线。
        recompute_smooth_normals(*next, cancellation, [&](auto done, auto total) {
            report(LoopStage::normals, level, done, total);
        });
        report(LoopStage::output_topology, level, 0, 1);
        HalfedgeMesh next_topology(*next, cancellation);
        report(LoopStage::output_topology, level, 1, 1);
        result.levels.push_back(next_topology.statistics());
        result.mesh = std::move(next);
        topology = std::move(next_topology);
    }
    report(LoopStage::complete, settings.levels, 1, 1);
    return result;
}

} // namespace minirender
