#include <minirender/geometry/surface.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace minirender {
namespace {

bool finite(glm::dvec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

std::array<glm::dvec3, 3> vertices(const MeshData& mesh, MeshIndex face) {
    const auto& c = mesh.triangles[face].corners;
    return {glm::dvec3(mesh.positions[c[0].position]), glm::dvec3(mesh.positions[c[1].position]),
            glm::dvec3(mesh.positions[c[2].position])};
}

// 先求三角形所在平面上的投影。投影在面内时使用它，否则最近点必在三条线段上。
// 保留重心坐标，后续投影工具可以据此插值面角 UV 与法线。
MeshClosestPoint triangle_closest(glm::dvec3 p, const std::array<glm::dvec3, 3>& v,
                                  MeshIndex face) {
    const auto ab = v[1] - v[0], ac = v[2] - v[0];
    const auto n = glm::cross(ab, ac);
    const auto projected = p - n * (glm::dot(p - v[0], n) / glm::dot(n, n));
    const auto relative = projected - v[0];
    const double b = glm::dot(glm::cross(relative, ac), n) / glm::dot(n, n);
    const double c = glm::dot(glm::cross(ab, relative), n) / glm::dot(n, n);
    MeshClosestPoint result;
    result.triangle = face;
    if (b >= 0 && c >= 0 && b + c <= 1) {
        result.position = projected;
        result.barycentric = {1 - b - c, b, c};
        result.distance_squared = glm::dot(p - projected, p - projected);
        return result;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto j = (i + 1) % 3;
        const auto edge = v[j] - v[i];
        const auto t = std::clamp(glm::dot(p - v[i], edge) / glm::dot(edge, edge), 0.0, 1.0);
        const auto q = v[i] + t * edge;
        const auto distance = glm::dot(p - q, p - q);
        if (distance < result.distance_squared) {
            result.position = q;
            result.barycentric = glm::dvec3(0);
            result.barycentric[static_cast<glm::length_t>(i)] = 1 - t;
            result.barycentric[static_cast<glm::length_t>(j)] = t;
            result.distance_squared = distance;
        }
    }
    return result;
}

double bounds_distance(glm::dvec3 p, glm::dvec3 low, glm::dvec3 high) {
    const auto d = glm::max(glm::max(low - p, p - high), glm::dvec3(0));
    return glm::dot(d, d);
}

std::vector<bool> referenced(const MeshData& mesh, const CancellationToken& token) {
    std::vector<bool> result(mesh.positions.size(), false);
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        if (i % 1024 == 0) {
            token.check();
        }
        for (const auto& corner : mesh.triangles[i].corners) {
            result[corner.position] = true;
        }
    }
    return result;
}

} // namespace

MeshSurface::MeshSurface(std::shared_ptr<const MeshData> mesh, CancellationToken cancellation)
    : mesh_(std::move(mesh)) {
    if (!mesh_ || mesh_->triangles.empty()) {
        throw std::invalid_argument("Surface query requires a nonempty mesh");
    }
    mesh_->validate(cancellation);
    if (mesh_->triangles.size() > invalid_mesh_index / 2) {
        throw std::length_error("Surface query exceeds the node index capacity");
    }
    order_.resize(mesh_->triangles.size());
    std::iota(order_.begin(), order_.end(), MeshIndex{0});
    for (MeshIndex i = 0; i < order_.size(); ++i) {
        if (i % 1024 == 0) {
            cancellation.check();
        }
        const auto v = vertices(*mesh_, i);
        const auto n = glm::cross(v[1] - v[0], v[2] - v[0]);
        if (glm::dot(n, n) <= 1e-20) {
            throw std::invalid_argument("Surface query requires nondegenerate triangles");
        }
    }
    nodes_.reserve(order_.size() / 2 + 1);
    build(0, static_cast<MeshIndex>(order_.size()), cancellation);
    cancellation.check();
}

MeshIndex MeshSurface::build(MeshIndex begin, MeshIndex end, const CancellationToken& token) {
    token.check();
    Node node;
    node.low = glm::dvec3(std::numeric_limits<double>::infinity());
    node.high = -node.low;
    glm::dvec3 center_low = node.low, center_high = node.high;
    for (auto i = begin; i < end; ++i) {
        if (i % 1024 == 0) {
            token.check();
        }
        const auto v = vertices(*mesh_, order_[i]);
        for (auto p : v) {
            node.low = glm::min(node.low, p);
            node.high = glm::max(node.high, p);
        }
        const auto center = (v[0] + v[1] + v[2]) / 3.0;
        center_low = glm::min(center_low, center);
        center_high = glm::max(center_high, center);
    }
    const auto index = static_cast<MeshIndex>(nodes_.size());
    nodes_.push_back(node);
    if (end - begin <= 8) {
        nodes_[index].first = begin;
        nodes_[index].count = end - begin;
        return index;
    }
    const auto extent = center_high - center_low;
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0
                     : extent.y >= extent.z                       ? 1
                                                                  : 2;
    const auto middle = begin + (end - begin) / 2;
    std::size_t comparisons = 0;
    std::nth_element(order_.begin() + begin, order_.begin() + middle, order_.begin() + end,
                     [&](auto a, auto b) {
                         if (++comparisons % 1024 == 0) {
                             token.check();
                         }
                         const auto va = vertices(*mesh_, a), vb = vertices(*mesh_, b);
                         const double ca = va[0][axis] + va[1][axis] + va[2][axis];
                         const double cb = vb[0][axis] + vb[1][axis] + vb[2][axis];
                         return ca == cb ? a < b : ca < cb;
                     });
    const auto left = build(begin, middle, token);
    const auto right = build(middle, end, token);
    nodes_[index].first = left;
    nodes_[index].right = right;
    return index;
}

MeshClosestPoint MeshSurface::closest(glm::dvec3 point, CancellationToken cancellation) const {
    if (!finite(point)) {
        throw std::invalid_argument("Surface query point must be finite");
    }
    cancellation.check();
    // 中位数树的最大深度受 32 位面索引约束，64 个槽足以保存待访问的兄弟节点。
    std::array<std::pair<MeshIndex, double>, 64> stack;
    std::size_t pending = 1, visited = 0;
    stack[0] = {0, 0};
    MeshClosestPoint result;
    while (pending) {
        const auto [index, lower_bound] = stack[--pending];
        if (lower_bound > result.distance_squared) {
            continue;
        }
        if (++visited % 1024 == 0) {
            cancellation.check();
        }
        const auto& node = nodes_[index];
        if (node.count) {
            for (auto i = node.first; i < node.first + node.count; ++i) {
                const auto face = order_[i];
                const auto hit = triangle_closest(point, vertices(*mesh_, face), face);
                if (hit.distance_squared < result.distance_squared ||
                    (hit.distance_squared == result.distance_squared && face < result.triangle)) {
                    result = hit;
                }
            }
        } else {
            const auto& left = nodes_[node.first];
            const auto& right = nodes_[node.right];
            std::pair<MeshIndex, double> a{node.first, bounds_distance(point, left.low, left.high)};
            std::pair<MeshIndex, double> b{node.right,
                                           bounds_distance(point, right.low, right.high)};
            if (a.second < b.second) {
                std::swap(a, b);
            }
            if (a.second <= result.distance_squared) {
                stack[pending++] = a;
            }
            if (b.second <= result.distance_squared) {
                stack[pending++] = b;
            }
        }
    }
    cancellation.check();
    return result;
}

SurfaceComparison compare_surfaces(std::shared_ptr<const MeshData> original,
                                   std::shared_ptr<const MeshData> result,
                                   CancellationToken cancellation, SurfaceProgress progress) {
    const MeshSurface original_surface(original, cancellation),
        result_surface(result, cancellation);
    const auto original_used = referenced(*original, cancellation);
    const auto result_used = referenced(*result, cancellation);
    const auto sample_count = [](const MeshData& mesh, const auto& used) {
        return mesh.triangles.size() +
               static_cast<std::size_t>(std::count(used.begin(), used.end(), true));
    };
    const auto total = sample_count(*original, original_used) + sample_count(*result, result_used);
    std::size_t completed = 0;
    const auto report = [&] {
        cancellation.check();
        if (progress) {
            progress(completed, total);
        }
        cancellation.check();
    };
    report();
    const auto measure = [&](const MeshData& mesh, const auto& used, const MeshSurface& surface) {
        SurfaceDistance distance;
        double squared_sum = 0;
        const auto sample = [&](glm::dvec3 p) {
            const auto hit = surface.closest(p, cancellation);
            squared_sum += hit.distance_squared;
            distance.maximum = std::max(distance.maximum, std::sqrt(hit.distance_squared));
            ++distance.samples;
            if (++completed % 1024 == 0) {
                report();
            }
        };
        for (std::size_t i = 0; i < used.size(); ++i) {
            if (i % 1024 == 0) {
                cancellation.check();
            }
            if (used[i]) {
                sample(glm::dvec3(mesh.positions[i]));
            }
        }
        for (MeshIndex i = 0; i < mesh.triangles.size(); ++i) {
            const auto v = vertices(mesh, i);
            sample((v[0] + v[1] + v[2]) / 3.0);
        }
        distance.rms = std::sqrt(squared_sum / static_cast<double>(distance.samples));
        return distance;
    };
    SurfaceComparison comparison;
    comparison.original_to_result = measure(*original, original_used, result_surface);
    comparison.result_to_original = measure(*result, result_used, original_surface);
    const auto& a = comparison.original_to_result;
    const auto& b = comparison.result_to_original;
    comparison.symmetric_rms = std::sqrt((a.rms * a.rms * static_cast<double>(a.samples) +
                                          b.rms * b.rms * static_cast<double>(b.samples)) /
                                         static_cast<double>(total));
    comparison.sampled_maximum = std::max(a.maximum, b.maximum);
    report();
    return comparison;
}

} // namespace minirender
