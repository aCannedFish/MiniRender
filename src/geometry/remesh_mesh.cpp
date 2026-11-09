#include "remesh_mesh.hpp"
#include <minirender/geometry/normals.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <stdexcept>

namespace minirender::detail {
namespace {

template <class T> void sort(std::vector<T>& values, const CancellationToken& token) {
    std::size_t comparisons = 0;
    std::sort(values.begin(), values.end(), [&](const T& a, const T& b) {
        if (++comparisons % 4096 == 0) {
            token.check();
        }
        return a < b;
    });
    token.check();
}
template <class T> void unique(std::vector<T>& values, const CancellationToken& token) {
    sort(values, token);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool finite(glm::dvec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

glm::vec3 interpolated_normal(glm::vec3 a, glm::vec3 b) {
    const auto sum = glm::dvec3(a) + glm::dvec3(b);
    if (glm::length(sum) <= 1e-12) {
        throw std::invalid_argument("Remeshing cannot interpolate opposite corner normals");
    }
    return glm::vec3(glm::normalize(sum));
}

double uv_area(const MeshTriangle& triangle) {
    const auto a = glm::dvec2(triangle.corners[1].uv) - glm::dvec2(triangle.corners[0].uv);
    const auto b = glm::dvec2(triangle.corners[2].uv) - glm::dvec2(triangle.corners[0].uv);
    return a.x * b.y - a.y * b.x;
}

} // namespace

RemeshEdge remesh_edge(MeshIndex a, MeshIndex b) noexcept {
    return (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
}
std::pair<MeshIndex, MeshIndex> remesh_endpoints(RemeshEdge edge) noexcept {
    return {static_cast<MeshIndex>(edge >> 32U), static_cast<MeshIndex>(edge)};
}

RemeshMesh::RemeshMesh(const MeshData& input, const HalfedgeMesh& topology,
                       const RemeshSettings& settings, CancellationToken cancellation)
    : settings_(settings), token_(cancellation), triangle_count_(input.triangles.size()) {
    vertices_.resize(input.positions.size());
    faces_.resize(input.triangles.size());
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        vertices_[i].position = glm::dvec3(input.positions[i]);
        vertices_[i].boundary = topology.vertices()[i].halfedge != invalid_mesh_index &&
                                topology.is_boundary(topology.vertices()[i].halfedge);
    }
    const auto hs = topology.halfedges();
    std::vector<bool> seen(vertices_.size(), false);
    std::vector<glm::dvec3> first_normal(vertices_.size());
    for (MeshIndex i = 0; i < faces_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        faces_[i] = {input.triangles[i], 0, true};
        for (const auto& corner : input.triangles[i].corners) {
            auto& vertex = vertices_[corner.position];
            vertex.faces.push_back(i);
            const auto n = glm::normalize(glm::dvec3(corner.normal));
            if (seen[corner.position]) {
                const bool uv_jump =
                    glm::length(glm::dvec2(corner.uv) - glm::dvec2(vertex.uv)) > 1e-6;
                const bool normal_seam = glm::dot(n, first_normal[corner.position]) < 1 - 1e-6;
                vertex.normal_seam =
                    vertex.normal_seam || (settings.preserve_hard_normals && normal_seam);
                vertex.attribute_locked = vertex.attribute_locked ||
                                          (settings.preserve_uv_seams && uv_jump) ||
                                          (settings.preserve_hard_normals && normal_seam);
            } else {
                seen[corner.position] = true;
                vertex.uv = corner.uv;
                first_normal[corner.position] = n;
            }
        }
    }
    const double cosine = std::cos(settings.crease_angle_degrees * std::numbers::pi / 180.0);
    for (std::size_t i = 0; i < topology.edges().size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        const auto h = topology.edges()[i].halfedge;
        const auto twin = hs[h].twin;
        const auto a = hs[h].origin;
        const auto b = hs[twin].origin;
        std::uint8_t feature = 0;
        if (topology.is_boundary(twin)) {
            feature = settings.preserve_boundary ? boundary : 0;
        } else {
            const auto& ac = input.triangles[hs[h].face].corners[h % 3];
            const auto& bc = input.triangles[hs[h].face].corners[(h % 3 + 1) % 3];
            const auto& bt = input.triangles[hs[twin].face].corners[twin % 3];
            const auto& at = input.triangles[hs[twin].face].corners[(twin % 3 + 1) % 3];
            if (settings.preserve_uv_seams &&
                (glm::length(glm::dvec2(ac.uv) - glm::dvec2(at.uv)) > 1e-6 ||
                 glm::length(glm::dvec2(bc.uv) - glm::dvec2(bt.uv)) > 1e-6)) {
                feature |= uv;
            }
            if (settings.preserve_hard_normals &&
                (glm::dot(glm::normalize(glm::dvec3(ac.normal)),
                          glm::normalize(glm::dvec3(at.normal))) < 1 - 1e-6 ||
                 glm::dot(glm::normalize(glm::dvec3(bc.normal)),
                          glm::normalize(glm::dvec3(bt.normal))) < 1 - 1e-6)) {
                feature |= normal_jump;
            }
            if (settings.preserve_creases &&
                glm::dot(glm::normalize(normal(input.triangles[hs[h].face])),
                         glm::normalize(normal(input.triangles[hs[twin].face]))) < cosine) {
                feature |= crease;
            }
        }
        if (feature) {
            features_.emplace(remesh_edge(a, b), feature);
            vertices_[a].fixed = vertices_[b].fixed = true;
        }
    }
    for (auto& vertex : vertices_) {
        vertex.fixed = vertex.fixed || vertex.attribute_locked;
    }
    // 在保护边处切分原表面，投影与 UV 传递限制在同一个连通片区内。
    std::vector<MeshIndex> components(input.triangles.size(), invalid_mesh_index);
    for (MeshIndex seed = 0; seed < components.size(); ++seed) {
        if (components[seed] != invalid_mesh_index) {
            continue;
        }
        std::vector<MeshIndex> pending{seed};
        components[seed] = component_count_;
        while (!pending.empty()) {
            token_.check();
            const auto face = pending.back();
            pending.pop_back();
            for (MeshIndex corner = 0; corner < 3; ++corner) {
                const auto h = face * 3 + corner;
                const auto neighbor = hs[hs[h].twin].face;
                if (neighbor != invalid_mesh_index && components[neighbor] == invalid_mesh_index &&
                    !protected_edge(remesh_edge(hs[h].origin, hs[hs[h].twin].origin))) {
                    components[neighbor] = component_count_;
                    pending.push_back(neighbor);
                }
            }
        }
        ++component_count_;
    }
    for (MeshIndex face = 0; face < faces_.size(); ++face) {
        if (face % 1024 == 0) {
            token_.check();
        }
        faces_[face].component = components[face];
        for (const auto& corner : faces_[face].triangle.corners) {
            vertices_[corner.position].component = components[face];
        }
    }
}

std::span<const RemeshVertex> RemeshMesh::vertices() const noexcept {
    return vertices_;
}
std::span<const RemeshFace> RemeshMesh::faces() const noexcept {
    return faces_;
}
std::size_t RemeshMesh::triangle_count() const noexcept {
    return triangle_count_;
}
MeshIndex RemeshMesh::component_count() const noexcept {
    return component_count_;
}

bool RemeshMesh::contains(MeshIndex face, MeshIndex vertex) const {
    return faces_[face].alive &&
           std::any_of(faces_[face].triangle.corners.begin(), faces_[face].triangle.corners.end(),
                       [&](const auto& c) { return c.position == vertex; });
}
std::vector<RemeshEdge> RemeshMesh::edges() const {
    std::vector<RemeshEdge> result;
    result.reserve(triangle_count_ * 3);
    for (std::size_t i = 0; i < faces_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (!faces_[i].alive) {
            continue;
        }
        const auto& c = faces_[i].triangle.corners;
        for (std::size_t j = 0; j < 3; ++j) {
            result.push_back(remesh_edge(c[j].position, c[(j + 1) % 3].position));
        }
    }
    unique(result, token_);
    token_.check();
    return result;
}
std::vector<MeshIndex> RemeshMesh::neighbors(MeshIndex vertex) const {
    std::vector<MeshIndex> result;
    std::size_t visited = 0;
    for (const auto face : vertices_[vertex].faces) {
        if (++visited % 1024 == 0) {
            token_.check();
        }
        for (const auto& corner : faces_[face].triangle.corners) {
            if (corner.position != vertex) {
                result.push_back(corner.position);
            }
        }
    }
    unique(result, token_);
    return result;
}
std::vector<MeshIndex> RemeshMesh::edge_faces(MeshIndex a, MeshIndex b) const {
    std::vector<MeshIndex> result;
    std::size_t visited = 0;
    for (const auto face : vertices_[a].faces) {
        if (++visited % 1024 == 0) {
            token_.check();
        }
        if (contains(face, b)) {
            result.push_back(face);
        }
    }
    return result;
}
bool RemeshMesh::has_edge(MeshIndex a, MeshIndex b) const {
    return vertices_[a].alive && vertices_[b].alive && !edge_faces(a, b).empty();
}
bool RemeshMesh::protected_edge(RemeshEdge edge) const {
    return features_.contains(edge);
}

glm::dvec3 RemeshMesh::normal(const MeshTriangle& triangle) const {
    return normal_with(triangle, invalid_mesh_index, {});
}
glm::dvec3 RemeshMesh::normal_with(const MeshTriangle& triangle, MeshIndex vertex,
                                   glm::dvec3 position) const {
    std::array<glm::dvec3, 3> p;
    for (std::size_t i = 0; i < 3; ++i) {
        p[i] = triangle.corners[i].position == vertex
                   ? position
                   : vertices_[triangle.corners[i].position].position;
    }
    return glm::cross(p[1] - p[0], p[2] - p[0]);
}
glm::dvec3 RemeshMesh::vertex_normal(MeshIndex vertex) const {
    glm::dvec3 sum(0);
    double area = 0;
    std::size_t visited = 0;
    for (const auto face : vertices_[vertex].faces) {
        if (++visited % 1024 == 0) {
            token_.check();
        }
        const auto n = normal(faces_[face].triangle);
        sum += n;
        area += glm::length(n);
    }
    if (area == 0) {
        return {};
    }
    if (glm::length(sum) <= 1e-12 * area) {
        throw std::invalid_argument("Remeshing vertex has no stable normal");
    }
    return glm::normalize(sum);
}
bool RemeshMesh::valid_face(const MeshTriangle& candidate, glm::dvec3 before,
                            MeshIndex moved_vertex, glm::dvec3 position) const {
    const auto n = normal_with(candidate, moved_vertex, position);
    return finite(n) && glm::dot(n, n) > 1e-20 && glm::dot(n, before) > 0;
}
bool RemeshMesh::uv_orientation(const MeshTriangle& before, const MeshTriangle& after) const {
    if (!settings_.preserve_uv_seams) {
        return true;
    }
    const auto a = uv_area(before);
    const auto b = uv_area(after);
    return std::abs(a) <= 1e-16 || a * b > 0;
}

void RemeshMesh::remove_face(MeshIndex face) {
    for (const auto& c : faces_[face].triangle.corners) {
        std::erase(vertices_[c.position].faces, face);
    }
    faces_[face].alive = false;
    --triangle_count_;
}
void RemeshMesh::replace_face(MeshIndex face, const MeshTriangle& triangle) {
    for (const auto& c : faces_[face].triangle.corners) {
        std::erase(vertices_[c.position].faces, face);
    }
    faces_[face].triangle = triangle;
    for (const auto& c : triangle.corners) {
        vertices_[c.position].faces.push_back(face);
    }
}
void RemeshMesh::add_face(const MeshTriangle& triangle, MeshIndex component) {
    if (faces_.size() >= invalid_mesh_index / 6) {
        throw std::length_error("Remeshing exceeds face index capacity");
    }
    const auto index = static_cast<MeshIndex>(faces_.size());
    faces_.push_back({triangle, component, true});
    for (const auto& c : triangle.corners) {
        vertices_[c.position].faces.push_back(index);
    }
    ++triangle_count_;
}
void RemeshMesh::changed(std::vector<MeshIndex>& vertices) {
    unique(vertices, token_);
    for (const auto v : vertices) {
        ++vertices_[v].version;
    }
}

// link 条件检查共同一环，替换后的无序面键补充检查四面体产生的重复面。
bool RemeshMesh::link_condition(MeshIndex a, MeshIndex b, std::vector<MeshIndex>& local) const {
    const auto na = neighbors(a);
    const auto nb = neighbors(b);
    std::vector<MeshIndex> common;
    std::vector<MeshIndex> opposite;
    std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(), std::back_inserter(common));
    local = vertices_[a].faces;
    local.insert(local.end(), vertices_[b].faces.begin(), vertices_[b].faces.end());
    unique(local, token_);
    const auto shared = edge_faces(a, b);
    for (const auto face : shared) {
        for (const auto& c : faces_[face].triangle.corners) {
            if (c.position != a && c.position != b) {
                opposite.push_back(c.position);
            }
        }
    }
    unique(opposite, token_);
    if (shared.empty() || shared.size() > 2 || common != opposite ||
        local.size() <= shared.size() ||
        (shared.size() == 2 && vertices_[a].boundary && vertices_[b].boundary)) {
        return false;
    }
    std::vector<std::array<MeshIndex, 3>> keys;
    for (const auto face : local) {
        if (contains(face, a) && contains(face, b)) {
            continue;
        }
        std::array<MeshIndex, 3> key;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto vertex = faces_[face].triangle.corners[i].position;
            key[i] = vertex == b ? a : vertex;
        }
        std::sort(key.begin(), key.end());
        keys.push_back(key);
    }
    sort(keys, token_);
    return std::adjacent_find(keys.begin(), keys.end()) == keys.end();
}

RemeshEdit RemeshMesh::split(RemeshEdge edge, glm::dvec3 position,
                             std::optional<glm::vec2> uv_value) {
    token_.check();
    const auto [a, b] = remesh_endpoints(edge);
    if (!has_edge(a, b)) {
        return {};
    }
    const auto adjacent = edge_faces(a, b);
    if (triangle_count_ + adjacent.size() > settings_.maximum_triangles) {
        throw std::length_error("Remeshing split exceeds the triangle budget");
    }
    if (vertices_.size() >= invalid_mesh_index) {
        throw std::length_error("Remeshing exceeds vertex index capacity");
    }
    const auto feature = features_.find(edge);
    const std::uint8_t flags = feature == features_.end() ? 0 : feature->second;
    if (flags) {
        position = vertices_[a].position + (vertices_[b].position - vertices_[a].position) / 2.0;
        uv_value.reset();
    }
    position = glm::dvec3(glm::vec3(position));
    if (!finite(position)) {
        return {};
    }
    const auto midpoint = static_cast<MeshIndex>(vertices_.size());
    struct Children {
        MeshIndex face;
        MeshTriangle first;
        MeshTriangle second;
    };
    std::vector<Children> children;
    RemeshEdit edit;
    for (const auto face : adjacent) {
        const auto& parent = faces_[face].triangle;
        std::size_t corner = 0;
        for (; corner < 3; ++corner) {
            const auto u = parent.corners[corner].position;
            const auto v = parent.corners[(corner + 1) % 3].position;
            if ((u == a && v == b) || (u == b && v == a)) {
                break;
            }
        }
        if (corner == 3) {
            throw std::logic_error("Remeshing edge adjacency is inconsistent");
        }
        const auto u = parent.corners[corner];
        const auto v = parent.corners[(corner + 1) % 3];
        const auto w = parent.corners[(corner + 2) % 3];
        const MeshCorner middle{
            midpoint, interpolated_normal(u.normal, v.normal),
            uv_value.value_or(glm::vec2((glm::dvec2(u.uv) + glm::dvec2(v.uv)) / 2.0))};
        const MeshTriangle first{{u, middle, w}};
        const MeshTriangle second{{middle, v, w}};
        if (!valid_face(first, normal(parent), midpoint, position) ||
            !valid_face(second, normal(parent), midpoint, position) ||
            !uv_orientation(parent, first) || !uv_orientation(parent, second)) {
            return {};
        }
        children.push_back({face, first, second});
        for (const auto& c : parent.corners) {
            edit.affected.push_back(c.position);
        }
    }
    RemeshVertex vertex;
    vertex.position = position;
    vertex.uv = children.front().first.corners[1].uv;
    vertex.component = faces_[adjacent.front()].component;
    vertex.boundary = adjacent.size() == 1;
    vertex.fixed = flags != 0;
    vertex.attribute_locked = (flags & (uv | normal_jump)) != 0;
    vertex.normal_seam = (flags & normal_jump) != 0;
    vertices_.push_back(std::move(vertex));
    for (const auto& child : children) {
        const auto component = faces_[child.face].component;
        replace_face(child.face, child.first);
        add_face(child.second, component);
    }
    if (flags) {
        features_.erase(edge);
        features_[remesh_edge(a, midpoint)] = flags;
        features_[remesh_edge(midpoint, b)] = flags;
    }
    edit.affected.push_back(midpoint);
    changed(edit.affected);
    edit.applied = true;
    return edit;
}

RemeshEdit RemeshMesh::collapse(RemeshEdge edge, glm::dvec3 position, double maximum_edge,
                                std::optional<glm::vec2> projected_uv) {
    token_.check();
    auto [a, b] = remesh_endpoints(edge);
    if (!has_edge(a, b) || vertices_[a].attribute_locked || vertices_[b].attribute_locked ||
        (vertices_[a].fixed && vertices_[b].fixed)) {
        return {};
    }
    if (vertices_[b].fixed) {
        std::swap(a, b);
    }
    if (vertices_[a].fixed) {
        position = vertices_[a].position;
        projected_uv.reset();
    }
    position = glm::dvec3(glm::vec3(position));
    if (!finite(position)) {
        return {};
    }
    std::vector<MeshIndex> local;
    if (!link_condition(a, b, local)) {
        return {};
    }
    auto neighbors_a = neighbors(a);
    const auto neighbors_b = neighbors(b);
    neighbors_a.insert(neighbors_a.end(), neighbors_b.begin(), neighbors_b.end());
    unique(neighbors_a, token_);
    for (const auto neighbor : neighbors_a) {
        if (neighbor != a && neighbor != b &&
            glm::length(vertices_[neighbor].position - position) > maximum_edge) {
            return {};
        }
    }
    const auto direction = vertices_[b].position - vertices_[a].position;
    const auto length_squared = glm::dot(direction, direction);
    const auto t =
        length_squared > 0
            ? std::clamp(glm::dot(position - vertices_[a].position, direction) / length_squared,
                         0.0, 1.0)
            : 0;
    const auto uv_value = projected_uv.value_or(
        glm::vec2(glm::dvec2(vertices_[a].uv) +
                  t * (glm::dvec2(vertices_[b].uv) - glm::dvec2(vertices_[a].uv))));
    std::vector<std::pair<MeshIndex, MeshTriangle>> replacements;
    std::vector<MeshIndex> removed;
    for (const auto face : local) {
        if (contains(face, a) && contains(face, b)) {
            removed.push_back(face);
            continue;
        }
        auto candidate = faces_[face].triangle;
        for (auto& c : candidate.corners) {
            if (c.position == a || c.position == b) {
                c.position = a;
                c.uv = uv_value;
            }
        }
        if (!valid_face(candidate, normal(faces_[face].triangle), a, position) ||
            !uv_orientation(faces_[face].triangle, candidate)) {
            return {};
        }
        replacements.emplace_back(face, candidate);
    }
    RemeshEdit edit;
    edit.affected = std::move(neighbors_a);
    edit.affected.push_back(a);
    edit.affected.push_back(b);
    for (const auto face : removed) {
        remove_face(face);
    }
    for (const auto& [face, triangle] : replacements) {
        replace_face(face, triangle);
    }
    vertices_[a].position = position;
    vertices_[a].uv = uv_value;
    vertices_[a].boundary = vertices_[a].boundary || vertices_[b].boundary;
    vertices_[b].alive = false;
    changed(edit.affected);
    edit.applied = true;
    return edit;
}

RemeshEdit RemeshMesh::flip(RemeshEdge edge) {
    token_.check();
    const auto [a, b] = remesh_endpoints(edge);
    if (!has_edge(a, b) || protected_edge(edge)) {
        return {};
    }
    auto adjacent = edge_faces(a, b);
    if (adjacent.size() != 2) {
        return {};
    }
    const auto directed_corner = [&](MeshIndex face, MeshIndex from, MeshIndex to) {
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& corners = faces_[face].triangle.corners;
            if (corners[i].position == from && corners[(i + 1) % 3].position == to) {
                return i;
            }
        }
        return std::size_t{3};
    };
    if (directed_corner(adjacent[0], a, b) == 3) {
        std::swap(adjacent[0], adjacent[1]);
    }
    const auto i = directed_corner(adjacent[0], a, b);
    const auto j = directed_corner(adjacent[1], b, a);
    if (i == 3 || j == 3) {
        throw std::logic_error("Remeshing face orientation is inconsistent");
    }
    const auto old_first = faces_[adjacent[0]].triangle;
    const auto old_second = faces_[adjacent[1]].triangle;
    const auto ac = old_first.corners[i];
    const auto bc = old_first.corners[(i + 1) % 3];
    const auto cc = old_first.corners[(i + 2) % 3];
    const auto dc = old_second.corners[(j + 2) % 3];
    const auto c = cc.position;
    const auto d = dc.position;
    if (c == d || has_edge(c, d)) {
        return {};
    }
    const auto energy = [&](MeshIndex vertex, int delta) {
        const int target = vertices_[vertex].boundary ? 4 : 6;
        const auto deviation = static_cast<std::int64_t>(neighbors(vertex).size()) + delta - target;
        return deviation * deviation;
    };
    const auto before = energy(a, 0) + energy(b, 0) + energy(c, 0) + energy(d, 0);
    const auto after = energy(a, -1) + energy(b, -1) + energy(c, 1) + energy(d, 1);
    if (after >= before) {
        return {};
    }
    const MeshTriangle first{{cc, dc, bc}}, second{{dc, cc, ac}};
    const auto n0 = normal(old_first);
    const auto n1 = normal(old_second);
    if (!valid_face(first, n0, invalid_mesh_index, {}) ||
        !valid_face(first, n1, invalid_mesh_index, {}) ||
        !valid_face(second, n0, invalid_mesh_index, {}) ||
        !valid_face(second, n1, invalid_mesh_index, {}) || !uv_orientation(old_first, first) ||
        !uv_orientation(old_second, second)) {
        return {};
    }
    replace_face(adjacent[0], first);
    replace_face(adjacent[1], second);
    RemeshEdit edit{true, {a, b, c, d}};
    changed(edit.affected);
    return edit;
}

bool RemeshMesh::valid_positions(std::span<const glm::dvec3> positions,
                                 std::span<const glm::vec2> uv_values) const {
    if (positions.size() != vertices_.size() || uv_values.size() != vertices_.size()) {
        throw std::logic_error("Remeshing position count changed");
    }
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (!finite(positions[i]) || !std::isfinite(uv_values[i].x) ||
            !std::isfinite(uv_values[i].y) ||
            (vertices_[i].fixed &&
             (positions[i] != vertices_[i].position || uv_values[i] != vertices_[i].uv))) {
            return false;
        }
    }
    for (std::size_t i = 0; i < faces_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (!faces_[i].alive) {
            continue;
        }
        const auto& c = faces_[i].triangle.corners;
        const auto area = glm::cross(positions[c[1].position] - positions[c[0].position],
                                     positions[c[2].position] - positions[c[0].position]);
        if (!finite(area) || glm::dot(area, area) <= 1e-20 ||
            glm::dot(area, normal(faces_[i].triangle)) <= 0) {
            return false;
        }
        auto candidate = faces_[i].triangle;
        for (auto& corner : candidate.corners) {
            const auto& vertex = vertices_[corner.position];
            if (!vertex.attribute_locked && (positions[corner.position] != vertex.position ||
                                             uv_values[corner.position] != vertex.uv)) {
                corner.uv = uv_values[corner.position];
            }
        }
        if (!uv_orientation(faces_[i].triangle, candidate)) {
            return false;
        }
    }
    return true;
}
std::size_t RemeshMesh::relocate(std::span<const glm::dvec3> positions,
                                 std::span<const glm::vec2> uv_values) {
    std::size_t moved = 0;
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (vertices_[i].alive &&
            (vertices_[i].position != positions[i] || vertices_[i].uv != uv_values[i])) {
            vertices_[i].position = positions[i];
            if (!vertices_[i].attribute_locked) {
                vertices_[i].uv = uv_values[i];
                for (const auto face : vertices_[i].faces) {
                    for (auto& corner : faces_[face].triangle.corners) {
                        if (corner.position == i) {
                            corner.uv = uv_values[i];
                        }
                    }
                }
            }
            ++vertices_[i].version;
            ++moved;
        }
    }
    return moved;
}

// 重编号按旧数组顺序进行，哈希表只重建映射，不决定输出次序。
void RemeshMesh::compact() {
    token_.check();
    std::vector<MeshIndex> mapping(vertices_.size(), invalid_mesh_index);
    std::vector<RemeshVertex> vertices;
    vertices.reserve(vertices_.size());
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (vertices_[i].alive) {
            mapping[i] = static_cast<MeshIndex>(vertices.size());
            auto vertex = vertices_[i];
            vertex.faces.clear();
            vertex.version = 0;
            vertices.push_back(std::move(vertex));
        }
    }
    std::vector<RemeshFace> faces;
    faces.reserve(triangle_count_);
    for (std::size_t i = 0; i < faces_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (!faces_[i].alive) {
            continue;
        }
        auto face = faces_[i];
        for (auto& c : face.triangle.corners) {
            c.position = mapping[c.position];
            vertices[c.position].faces.push_back(static_cast<MeshIndex>(faces.size()));
        }
        faces.push_back(face);
    }
    std::unordered_map<RemeshEdge, std::uint8_t> features;
    features.reserve(features_.size());
    for (const auto& [edge, flags] : features_) {
        const auto [a, b] = remesh_endpoints(edge);
        if (mapping[a] == invalid_mesh_index || mapping[b] == invalid_mesh_index) {
            throw std::logic_error("Remeshing removed a protected edge endpoint");
        }
        features.emplace(remesh_edge(mapping[a], mapping[b]), flags);
    }
    vertices_ = std::move(vertices);
    faces_ = std::move(faces);
    features_ = std::move(features);
}

std::shared_ptr<const MeshData> RemeshMesh::publish(bool update_normals) const {
    token_.check();
    auto result = std::make_shared<MeshData>();
    std::vector<MeshIndex> mapping(vertices_.size(), invalid_mesh_index);
    std::vector<bool> normal_seams;
    result->positions.reserve(vertices_.size());
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (vertices_[i].alive) {
            mapping[i] = static_cast<MeshIndex>(result->positions.size());
            result->positions.emplace_back(vertices_[i].position);
            normal_seams.push_back(vertices_[i].normal_seam);
        }
    }
    result->triangles.reserve(triangle_count_);
    for (std::size_t i = 0; i < faces_.size(); ++i) {
        if (i % 1024 == 0) {
            token_.check();
        }
        if (faces_[i].alive) {
            auto triangle = faces_[i].triangle;
            for (auto& c : triangle.corners) {
                c.position = mapping[c.position];
            }
            result->triangles.push_back(triangle);
        }
    }
    if (update_normals) {
        const auto normals = smooth_vertex_normals(*result, token_);
        for (std::size_t i = 0; i < result->triangles.size(); ++i) {
            if (i % 1024 == 0) {
                token_.check();
            }
            for (auto& c : result->triangles[i].corners) {
                if (!normal_seams[c.position]) {
                    c.normal = normals[c.position];
                }
            }
        }
    }
    result->validate(token_);
    return result;
}

} // namespace minirender::detail
