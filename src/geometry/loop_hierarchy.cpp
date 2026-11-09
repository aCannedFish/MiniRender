#include "loop_hierarchy.hpp"
#include "loop_rules.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender::detail {
namespace {

std::uint64_t ring_key(MeshIndex vertex, std::uint32_t level) {
    return (static_cast<std::uint64_t>(level) << 32U) | vertex;
}

} // namespace

std::size_t LoopHierarchy::EdgeHash::operator()(const EdgeKey& key) const noexcept {
    const auto edge = (static_cast<std::uint64_t>(key.a) << 32U) | key.b;
    return std::hash<std::uint64_t>{}(edge) ^ (std::hash<std::uint32_t>{}(key.level) << 1U);
}

LoopHierarchy::LoopHierarchy(std::shared_ptr<const MeshData> input, const HalfedgeMesh& topology,
                             std::size_t maximum_points, CancellationToken cancellation)
    : input_(std::move(input)), maximum_points_(maximum_points),
      cancellation_(std::move(cancellation)) {
    cancellation_.check();
    if (!input_ || input_->positions.size() > maximum_points_ ||
        maximum_points_ >= invalid_mesh_index) {
        throw std::invalid_argument("Loop hierarchy requires an input within the point budget");
    }
    if (topology.vertices().size() != input_->positions.size()) {
        throw std::logic_error("Loop hierarchy topology does not match its input");
    }
    const auto hs = topology.halfedges();
    for (MeshIndex vertex = 0; vertex < input_->positions.size(); ++vertex) {
        if (vertex % 1024 == 0) {
            cancellation_.check();
        }
        const auto& source = topology.vertices()[vertex];
        Point point;
        point.position = glm::dvec3(input_->positions[vertex]);
        point.ready = true;
        LoopRing ring;
        if (source.halfedge != invalid_mesh_index) {
            ring.boundary = topology.is_boundary(source.halfedge);
            auto current = source.halfedge;
            if (ring.boundary) {
                current = hs[hs[current].previous].twin;
            }
            const auto start = current;
            do {
                if (ring.neighbors.size() > source.valence) {
                    throw std::logic_error("Loop hierarchy found an inconsistent vertex ring");
                }
                if (ring.neighbors.size() % 1024 == 0) {
                    cancellation_.check();
                }
                ring.neighbors.push_back(topology.destination(current));
                if (topology.is_boundary(current)) {
                    break;
                }
                current = hs[hs[current].previous].twin;
            } while (current != start);
            if (ring.neighbors.size() != source.valence) {
                throw std::logic_error("Loop hierarchy ring has the wrong valence");
            }
        }
        point.boundary = ring.boundary;
        points_.push_back(point);
        rings_.emplace(ring_key(vertex, 0), std::move(ring));
    }
}

void LoopHierarchy::validate_query(MeshIndex vertex, std::uint32_t level) const {
    cancellation_.check();
    if (level > 8 || vertex >= points_.size() || points_[vertex].birth > level) {
        throw std::invalid_argument("Loop hierarchy query has an invalid vertex or level");
    }
}

MeshIndex LoopHierarchy::midpoint(MeshIndex a, MeshIndex b, std::uint32_t parent_level) {
    validate_query(a, parent_level);
    validate_query(b, parent_level);
    if (a == b || parent_level == 8) {
        throw std::invalid_argument("Loop hierarchy midpoint requires an edge below level 8");
    }
    if (a > b) {
        std::swap(a, b);
    }
    const EdgeKey key{a, b, parent_level};
    const auto found = edges_.find(key);
    if (found != edges_.end()) {
        return found->second;
    }
    const auto adjacent = opposites(a, b, parent_level);
    if (points_.size() >= maximum_points_) {
        throw std::length_error("Adaptive evaluation exceeds the point budget");
    }
    Point point;
    point.birth = parent_level + 1;
    point.a = a;
    point.b = b;
    point.left = adjacent[0];
    point.right = adjacent[1];
    point.boundary = point.left == invalid_mesh_index || point.right == invalid_mesh_index;
    const auto index = static_cast<MeshIndex>(points_.size());
    points_.push_back(point);
    edges_.emplace(key, index);
    return index;
}

const LoopRing& LoopHierarchy::ring(MeshIndex vertex, std::uint32_t level) {
    validate_query(vertex, level);
    const auto key = ring_key(vertex, level);
    const auto existing = rings_.find(key);
    if (existing != rings_.end()) {
        return existing->second;
    }
    const auto point = points_[vertex];
    LoopRing result;
    result.boundary = point.boundary;
    if (point.birth < level) {
        // even 点的一环由父层每条出边的 odd 点组成，顺序保持不变。
        const auto& parent = ring(vertex, level - 1);
        for (const auto neighbor : parent.neighbors) {
            result.neighbors.push_back(midpoint(vertex, neighbor, level - 1));
        }
    } else {
        // 两个父面分别提供三个相邻点；边界保留一个面的开放扇。
        const auto a = point.a;
        const auto b = point.b;
        if (point.left != invalid_mesh_index) {
            result.neighbors = {b, midpoint(b, point.left, level - 1),
                                midpoint(a, point.left, level - 1), a};
            if (point.right != invalid_mesh_index) {
                result.neighbors.push_back(midpoint(a, point.right, level - 1));
                result.neighbors.push_back(midpoint(b, point.right, level - 1));
            }
        } else {
            result.neighbors = {a, midpoint(a, point.right, level - 1),
                                midpoint(b, point.right, level - 1), b};
        }
    }
    if (rings_.size() >= maximum_points_ * 2) {
        throw std::length_error("Adaptive neighborhood cache exceeds its work budget");
    }
    return rings_.emplace(key, std::move(result)).first->second;
}

std::array<MeshIndex, 2> LoopHierarchy::opposites(MeshIndex a, MeshIndex b, std::uint32_t level) {
    const auto& around = ring(a, level);
    const auto found = std::find(around.neighbors.begin(), around.neighbors.end(), b);
    if (found == around.neighbors.end()) {
        throw std::logic_error("Loop hierarchy endpoints do not form an edge");
    }
    const auto index = static_cast<std::size_t>(found - around.neighbors.begin());
    const auto count = around.neighbors.size();
    const auto left = index + 1 < count ? around.neighbors[index + 1]
                      : around.boundary ? invalid_mesh_index
                                        : around.neighbors[0];
    const auto right = index > 0         ? around.neighbors[index - 1]
                       : around.boundary ? invalid_mesh_index
                                         : around.neighbors[count - 1];
    return {left, right};
}

glm::dvec3 LoopHierarchy::birth_position(MeshIndex vertex) {
    const auto point = points_[vertex];
    if (point.ready) {
        return point.position;
    }
    const auto a = control_position(point.a, point.birth - 1);
    const auto b = control_position(point.b, point.birth - 1);
    auto position = a + (b - a) / 2.0;
    if (!point.boundary) {
        const auto left = control_position(point.left, point.birth - 1);
        const auto right = control_position(point.right, point.birth - 1);
        position = a + (b - a) * (3.0 / 8.0) + ((left - a) + (right - a)) / 8.0;
    }
    points_[vertex].position = position;
    points_[vertex].ready = true;
    return position;
}

void LoopHierarchy::prepare_coefficient(MeshIndex vertex) {
    if (points_[vertex].coefficient_ready) {
        return;
    }
    const auto point = points_[vertex];
    const auto position = birth_position(vertex);
    const auto& neighbors = ring(vertex, point.birth);
    glm::dvec3 relative_sum(0);
    double gamma = 0;
    double rho = 0;
    if (neighbors.boundary) {
        relative_sum = control_position(neighbors.neighbors.front(), point.birth) - position +
                       control_position(neighbors.neighbors.back(), point.birth) - position;
        gamma = 1.0 / 6;
        rho = 0.25;
    } else if (!neighbors.neighbors.empty()) {
        for (const auto neighbor : neighbors.neighbors) {
            relative_sum += control_position(neighbor, point.birth) - position;
        }
        const auto n = static_cast<std::uint32_t>(neighbors.neighbors.size());
        const double beta = loop_beta(n);
        gamma = 1.0 / (n + 3.0 / (8 * beta));
        rho = 5.0 / 8.0 - n * beta;
    }
    // 由 [p,S] 的二维线性递推得到有限层系数，省去逐层展开全部邻域。
    points_[vertex].delta = gamma * relative_sum;
    points_[vertex].rho = rho;
    points_[vertex].coefficient_ready = true;
}

glm::dvec3 LoopHierarchy::control_position(MeshIndex vertex, std::uint32_t level) {
    validate_query(vertex, level);
    const auto position = birth_position(vertex);
    const auto steps = level - points_[vertex].birth;
    if (steps == 0) {
        return position;
    }
    prepare_coefficient(vertex);
    return position +
           (1 - std::pow(points_[vertex].rho, static_cast<double>(steps))) * points_[vertex].delta;
}

glm::dvec3 LoopHierarchy::smooth_normal(MeshIndex vertex, std::uint32_t level) {
    const auto& around = ring(vertex, level);
    if (around.neighbors.empty()) {
        return glm::dvec3(0);
    }
    const auto p = glm::dvec3(glm::vec3(control_position(vertex, level)));
    glm::dvec3 sum(0);
    double area_sum = 0;
    const auto count = around.neighbors.size() - (around.boundary ? 1 : 0);
    for (std::size_t i = 0; i < count; ++i) {
        const auto a = glm::dvec3(glm::vec3(control_position(around.neighbors[i], level)));
        const auto b = glm::dvec3(glm::vec3(
            control_position(around.neighbors[(i + 1) % around.neighbors.size()], level)));
        const auto area = glm::cross(a - p, b - p);
        const auto length = glm::length(area);
        if (!std::isfinite(length) || length <= 1e-10) {
            throw std::invalid_argument("Adaptive reference contains a degenerate face");
        }
        sum += area;
        area_sum += length;
    }
    const auto length = glm::length(sum);
    if (length <= 1e-12 * area_sum) {
        throw std::invalid_argument("Adaptive reference has no stable smooth normal");
    }
    return sum / length;
}

std::uint32_t LoopHierarchy::birth_level(MeshIndex vertex) const {
    cancellation_.check();
    return points_.at(vertex).birth;
}
std::array<MeshIndex, 2> LoopHierarchy::parent_edge(MeshIndex vertex) const {
    cancellation_.check();
    const auto& point = points_.at(vertex);
    if (point.birth == 0) {
        throw std::invalid_argument("A base Loop vertex has no parent edge");
    }
    return {point.a, point.b};
}
std::size_t LoopHierarchy::point_count() const noexcept {
    return points_.size();
}

} // namespace minirender::detail
