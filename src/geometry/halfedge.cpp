#include <minirender/geometry/halfedge.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace minirender {
namespace {

// 所有扫描与邻接遍历均定期检查取消，包括具有大量邻点的单个顶点。
void check_batch(const CancellationToken& cancellation, std::size_t index) {
    if (index % 1024 == 0) {
        cancellation.check();
    }
}

bool finite(glm::vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[noreturn]] void invalid_face(std::size_t face, const char* message) {
    throw std::invalid_argument("Face " + std::to_string(face) + ": " + message);
}

// 两端位置索引排序后编码为唯一键，映射结果不依赖哈希表的遍历次序。
std::uint64_t edge_key(MeshIndex a, MeshIndex b) {
    return (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
}

} // namespace

HalfedgeMesh::HalfedgeMesh(const MeshData& mesh, CancellationToken cancellation) {
    cancellation.check();
    if (mesh.positions.empty() || mesh.triangles.empty()) {
        throw std::invalid_argument("Mesh must contain positions and triangles");
    }
    // 最坏情况每个面有三个独立边界半边；预先保证全部索引可表示。
    if (mesh.positions.size() > invalid_mesh_index ||
        mesh.triangles.size() > invalid_mesh_index / 6) {
        throw std::length_error("Halfedge mesh exceeds 32-bit index capacity");
    }
    vertices_.resize(mesh.positions.size());
    faces_.resize(mesh.triangles.size());
    const auto interior_count = mesh.triangles.size() * 3;
    halfedges_.reserve(interior_count * 2);
    edges_.reserve(interior_count);
    std::unordered_map<std::uint64_t, MeshIndex> edge_lookup;
    edge_lookup.reserve(interior_count);
    for (std::size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
        check_batch(cancellation, vertex);
        if (!finite(mesh.positions[vertex])) {
            throw std::invalid_argument("Position " + std::to_string(vertex) + " is not finite");
        }
    }
    for (std::size_t face = 0; face < mesh.triangles.size(); ++face) {
        check_batch(cancellation, face);
        const auto& corners = mesh.triangles[face].corners;
        for (const auto& corner : corners) {
            if (corner.position >= mesh.positions.size()) {
                invalid_face(face, "position index is out of range");
            }
            if (!finite(corner.normal) ||
                glm::dot(glm::dvec3(corner.normal), glm::dvec3(corner.normal)) <= 1e-12 ||
                !std::isfinite(corner.uv.x) || !std::isfinite(corner.uv.y)) {
                invalid_face(face, "corner normal or UV is invalid");
            }
        }
        const glm::dvec3 a = mesh.positions[corners[0].position];
        const glm::dvec3 b = mesh.positions[corners[1].position];
        const glm::dvec3 c = mesh.positions[corners[2].position];
        const auto area = glm::cross(b - a, c - a);
        // 与 MeshData 的面积门槛相同，以 double 避免大坐标下 cross/dot 溢出。
        if (glm::dot(area, area) <= 1e-20) {
            invalid_face(face, "triangle is degenerate");
        }
        const auto first = static_cast<MeshIndex>(face * 3);
        faces_[face].halfedge = first;
        for (MeshIndex corner = 0; corner < 3; ++corner) {
            const auto origin = corners[corner].position;
            const auto end = corners[(corner + 1) % 3].position;
            const auto h = first + corner;
            Halfedge halfedge;
            halfedge.origin = origin;
            halfedge.next = first + (corner + 1) % 3;
            halfedge.previous = first + (corner + 2) % 3;
            halfedge.face = static_cast<MeshIndex>(face);
            const auto [entry, inserted] = edge_lookup.emplace(edge_key(origin, end), h);
            if (inserted) {
                halfedge.edge = static_cast<MeshIndex>(edges_.size());
                edges_.push_back({h});
            } else {
                auto& twin = halfedges_[entry->second];
                if (twin.twin != invalid_mesh_index) {
                    invalid_face(face, "edge has more than two incident faces");
                }
                if (twin.origin == origin) {
                    invalid_face(face, "shared edge has inconsistent winding or a duplicate face");
                }
                halfedge.edge = twin.edge;
                halfedge.twin = entry->second;
                twin.twin = h;
            }
            halfedges_.push_back(halfedge);
            vertices_[origin].halfedge = h;
            ++vertices_[origin].valence;
        }
    }
    // 反向重叠的两个三角形通过边方向检查，却是重复面，须在补边界前拒绝。
    for (std::size_t face = 0; face < faces_.size(); ++face) {
        check_batch(cancellation, face);
        const auto h = faces_[face].halfedge;
        const auto a = halfedges_[h].twin;
        const auto b = halfedges_[h + 1].twin;
        const auto c = halfedges_[h + 2].twin;
        if (a != invalid_mesh_index && b != invalid_mesh_index && c != invalid_mesh_index &&
            halfedges_[a].face == halfedges_[b].face && halfedges_[a].face == halfedges_[c].face) {
            invalid_face(face, "duplicate face with reversed winding");
        }
    }
    edge_lookup.clear();
    edge_lookup.rehash(0);

    // 显式补出外侧半边。流形边界上，每个顶点恰有一条边界出边和一条入边。
    std::vector<MeshIndex> boundary_outgoing(vertices_.size(), invalid_mesh_index);
    for (std::size_t index = 0; index < interior_count; ++index) {
        check_batch(cancellation, index);
        if (halfedges_[index].twin != invalid_mesh_index) {
            continue;
        }
        const auto origin = halfedges_[halfedges_[index].next].origin;
        const auto h = static_cast<MeshIndex>(halfedges_.size());
        if (boundary_outgoing[origin] != invalid_mesh_index) {
            throw std::invalid_argument("Vertex " + std::to_string(origin) +
                                        " has multiple boundary fans");
        }
        Halfedge boundary;
        boundary.origin = origin;
        boundary.twin = static_cast<MeshIndex>(index);
        boundary.edge = halfedges_[index].edge;
        halfedges_[index].twin = h;
        halfedges_.push_back(boundary);
        boundary_outgoing[origin] = h;
        vertices_[origin].halfedge = h;
        ++vertices_[origin].valence;
    }
    for (std::size_t index = interior_count; index < halfedges_.size(); ++index) {
        check_batch(cancellation, index);
        const auto end = halfedges_[halfedges_[index].twin].origin;
        const auto next = boundary_outgoing[end];
        if (next == invalid_mesh_index) {
            throw std::invalid_argument("Boundary does not form a closed loop");
        }
        halfedges_[index].next = next;
        halfedges_[next].previous = static_cast<MeshIndex>(index);
    }

    // 每个顶点的出边必须构成一个旋转环。这也能发现两个闭合壳体仅共享一个位置的情况。
    std::size_t traversed = 0;
    for (std::size_t vertex = 0; vertex < vertices_.size(); ++vertex) {
        check_batch(cancellation, vertex);
        const auto& v = vertices_[vertex];
        if (v.halfedge == invalid_mesh_index) {
            ++statistics_.isolated_vertices;
            continue;
        }
        ++statistics_.referenced_vertices;
        auto current = v.halfedge;
        MeshIndex count = 0;
        do {
            check_batch(cancellation, traversed++);
            ++count;
            current = halfedges_[halfedges_[current].twin].next;
        } while (current != v.halfedge && count < v.valence);
        if (current != v.halfedge || count != v.valence) {
            throw std::invalid_argument("Vertex " + std::to_string(vertex) +
                                        " has disconnected incident fans");
        }
    }

    // 边界与面连通分量各遍历一次；孤立位置不计入表面的 Euler 数。
    std::vector<bool> boundary_visited(halfedges_.size() - interior_count, false);
    for (std::size_t index = interior_count; index < halfedges_.size(); ++index) {
        check_batch(cancellation, index);
        if (boundary_visited[index - interior_count]) {
            continue;
        }
        ++statistics_.boundary_loops;
        auto current = static_cast<MeshIndex>(index);
        do {
            check_batch(cancellation, traversed++);
            boundary_visited[current - interior_count] = true;
            current = halfedges_[current].next;
        } while (current != index);
    }
    std::vector<bool> visited(faces_.size(), false);
    std::vector<MeshIndex> stack;
    for (std::size_t face = 0; face < faces_.size(); ++face) {
        check_batch(cancellation, face);
        if (visited[face]) {
            continue;
        }
        ++statistics_.face_components;
        visited[face] = true;
        stack.push_back(static_cast<MeshIndex>(face));
        while (!stack.empty()) {
            check_batch(cancellation, traversed++);
            const auto first = faces_[stack.back()].halfedge;
            stack.pop_back();
            for (MeshIndex corner = 0; corner < 3; ++corner) {
                const auto neighbor = halfedges_[halfedges_[first + corner].twin].face;
                if (neighbor != invalid_mesh_index && !visited[neighbor]) {
                    visited[neighbor] = true;
                    stack.push_back(neighbor);
                }
            }
        }
    }
    statistics_.edges = edges_.size();
    statistics_.faces = faces_.size();
    statistics_.boundary_edges = halfedges_.size() - interior_count;
    statistics_.euler_characteristic = static_cast<std::int64_t>(statistics_.referenced_vertices) -
                                       static_cast<std::int64_t>(statistics_.edges) +
                                       static_cast<std::int64_t>(statistics_.faces);
    cancellation.check();
}

std::span<const HalfedgeVertex> HalfedgeMesh::vertices() const noexcept {
    return vertices_;
}
std::span<const Halfedge> HalfedgeMesh::halfedges() const noexcept {
    return halfedges_;
}
std::span<const HalfedgeEdge> HalfedgeMesh::edges() const noexcept {
    return edges_;
}
std::span<const HalfedgeFace> HalfedgeMesh::faces() const noexcept {
    return faces_;
}
const TopologyStatistics& HalfedgeMesh::statistics() const noexcept {
    return statistics_;
}
MeshIndex HalfedgeMesh::destination(MeshIndex halfedge) const noexcept {
    return halfedges_[halfedges_[halfedge].twin].origin;
}
bool HalfedgeMesh::is_boundary(MeshIndex halfedge) const noexcept {
    return halfedges_[halfedge].face == invalid_mesh_index;
}

} // namespace minirender
