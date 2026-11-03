#include <minirender/render/bvh.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace minirender {
namespace {

constexpr std::uint32_t bin_count = 12;
constexpr std::size_t maximum_build_depth = 48;

bool finite(glm::vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// 桶索引基于重心范围，不用三角形包围盒中心替代几何重心。
std::uint32_t bin_index(float centroid, float low, float high) {
    const double coordinate =
        (static_cast<double>(centroid) - low) / (static_cast<double>(high) - low);
    return std::min(static_cast<std::uint32_t>(coordinate * bin_count), bin_count - 1);
}

} // namespace

SceneBvh::SceneBvh(std::shared_ptr<const SceneSnapshot> scene) : scene_(std::move(scene)) {
    if (!scene_) {
        throw std::invalid_argument("BVH requires a scene snapshot");
    }
    constexpr auto index_limit = std::numeric_limits<std::uint32_t>::max();
    if (scene_->instances.size() > index_limit) {
        throw std::length_error("Scene instance count exceeds BVH index range");
    }
    std::size_t total = 0;
    for (const auto& instance : scene_->instances) {
        if (instance.mesh->triangles.size() > index_limit / 2 - total) {
            throw std::length_error("Scene triangle count exceeds BVH index range");
        }
        total += instance.mesh->triangles.size();
    }
    primitives_.reserve(total);
    mirrored_.reserve(scene_->instances.size());
    std::vector<BuildPrimitive> build_primitives;
    build_primitives.reserve(total);
    for (std::size_t instance_index = 0; instance_index < scene_->instances.size();
         ++instance_index) {
        const auto& instance = scene_->instances[instance_index];
        mirrored_.push_back(glm::determinant(glm::dmat3(instance.object_to_world)) < 0);
        for (std::size_t index = 0; index < instance.mesh->triangles.size(); ++index) {
            const auto& triangle = instance.mesh->triangles[index];
            Primitive primitive{};
            BuildPrimitive build_primitive{};
            primitive.instance = static_cast<std::uint32_t>(instance_index);
            primitive.triangle = static_cast<std::uint32_t>(index);
            glm::dvec3 center(0);
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto position = instance.mesh->positions[triangle.corners[corner].position];
                const auto world = glm::vec3(instance.object_to_world * glm::vec4(position, 1));
                if (!finite(world)) {
                    throw std::invalid_argument("Transformed triangle position is not finite");
                }
                primitive.vertices[corner] = world;
                build_primitive.bounds.expand(world);
                center += glm::dvec3(world) / 3.0;
            }
            const auto edge1 =
                glm::dvec3(primitive.vertices[1]) - glm::dvec3(primitive.vertices[0]);
            const auto edge2 =
                glm::dvec3(primitive.vertices[2]) - glm::dvec3(primitive.vertices[0]);
            const auto normal = glm::cross(edge1, edge2);
            if (glm::dot(normal, normal) == 0) {
                throw std::invalid_argument("Transform collapses a triangle at float precision");
            }
            build_primitive.centroid = glm::vec3(center);
            primitives_.push_back(primitive);
            build_primitives.push_back(build_primitive);
        }
    }
    statistics_.triangles = primitives_.size();
    order_.resize(primitives_.size());
    std::iota(order_.begin(), order_.end(), 0U);
    if (!primitives_.empty()) {
        nodes_.reserve(primitives_.size() * 2 - 1);
        build(0, static_cast<std::uint32_t>(primitives_.size()), 0, build_primitives);
    }
    statistics_.nodes = nodes_.size();
}

std::uint32_t SceneBvh::build(std::uint32_t begin, std::uint32_t end, std::size_t depth,
                              std::span<const BuildPrimitive> primitives) {
    const auto node_index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.emplace_back();
    Bounds3 bounds;
    Bounds3 centroids;
    for (auto index = begin; index < end; ++index) {
        bounds.expand(primitives[order_[index]].bounds);
        centroids.expand(primitives[order_[index]].centroid);
    }
    const auto count = end - begin;
    statistics_.maximum_depth = std::max(statistics_.maximum_depth, depth);
    const auto leaf = [&] {
        nodes_[node_index] = {bounds, begin, 0, count};
        ++statistics_.leaves;
        return node_index;
    };
    if (count <= 4 || depth >= maximum_build_depth) {
        return leaf();
    }
    struct Bin {
        Bounds3 bounds;
        std::uint32_t count = 0;
    };
    double best_cost = static_cast<double>(count);
    int best_axis = -1;
    std::uint32_t best_split = 0;
    // C = C_traversal + (A_left*N_left + A_right*N_right) / A_parent。
    // 这里令一次节点遍历和一次三角形测试的相对代价均为 1，比较是否优于直接建叶子。
    const double parent_area = bounds.surface_area();
    for (int axis = 0; axis < 3; ++axis) {
        if (centroids.maximum[axis] == centroids.minimum[axis]) {
            continue;
        }
        std::array<Bin, bin_count> bins{};
        for (auto index = begin; index < end; ++index) {
            const auto& primitive = primitives[order_[index]];
            auto& bin = bins[bin_index(primitive.centroid[axis], centroids.minimum[axis],
                                       centroids.maximum[axis])];
            ++bin.count;
            bin.bounds.expand(primitive.bounds);
        }
        std::array<Bounds3, bin_count> right_bounds;
        std::array<std::uint32_t, bin_count> right_counts{};
        Bounds3 right;
        std::uint32_t right_count = 0;
        for (std::uint32_t index = bin_count; index-- > 0;) {
            right.expand(bins[index].bounds);
            right_count += bins[index].count;
            right_bounds[index] = right;
            right_counts[index] = right_count;
        }
        Bounds3 left;
        std::uint32_t left_count = 0;
        for (std::uint32_t split = 0; split + 1 < bin_count; ++split) {
            left.expand(bins[split].bounds);
            left_count += bins[split].count;
            if (left_count == 0 || right_counts[split + 1] == 0) {
                continue;
            }
            const double cost =
                1 + (left.surface_area() * left_count +
                     right_bounds[split + 1].surface_area() * right_counts[split + 1]) /
                        parent_area;
            if (cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
                best_split = split;
            }
        }
    }
    if (best_axis < 0) {
        return leaf();
    }
    const auto first = order_.begin() + begin;
    const auto last = order_.begin() + end;
    const auto middle = std::partition(first, last, [&](std::uint32_t index) {
        return bin_index(primitives[index].centroid[best_axis], centroids.minimum[best_axis],
                         centroids.maximum[best_axis]) <= best_split;
    });
    const auto split = static_cast<std::uint32_t>(middle - order_.begin());
    const auto left = build(begin, split, depth + 1, primitives);
    const auto right = build(split, end, depth + 1, primitives);
    // 递归可能使 vector 重新分配，不能跨递归保存 Node&。
    nodes_[node_index] = {bounds, left, right, 0};
    return node_index;
}

const SceneSnapshot& SceneBvh::scene() const noexcept {
    return *scene_;
}
const BvhBuildStatistics& SceneBvh::statistics() const noexcept {
    return statistics_;
}

std::optional<SceneBvh::PrimitiveHit> SceneBvh::trace(const RayQuery& query, RayInterval interval,
                                                      bool any_hit,
                                                      TraceStatistics* statistics) const {
    interval.validate();
    if (nodes_.empty()) {
        return std::nullopt;
    }
    const auto box_hit = [&](const Bounds3& bounds) {
        if (statistics) {
            ++statistics->bounds_tests;
        }
        return query.intersect(bounds, interval);
    };
    const auto root = box_hit(nodes_[0].bounds);
    if (!root) {
        return std::nullopt;
    }
    struct Entry {
        std::uint32_t node;
        double enter;
    };
    // 最大构建深度为 48，深度优先时最多保留每层一个兄弟节点及当前节点。
    std::array<Entry, maximum_build_depth + 2> stack{};
    std::size_t pending = 0;
    stack[pending++] = {0, *root};
    std::optional<PrimitiveHit> result;
    while (pending) {
        const auto entry = stack[--pending];
        if (entry.enter > interval.maximum) {
            continue;
        }
        const auto& node = nodes_[entry.node];
        if (node.count) {
            for (std::uint32_t offset = 0; offset < node.count; ++offset) {
                const auto primitive = order_[node.first + offset];
                if (statistics) {
                    ++statistics->triangle_tests;
                }
                const auto hit = query.intersect(primitives_[primitive].vertices, interval);
                if (hit &&
                    (!result || hit->distance < result->hit.distance ||
                     (hit->distance == result->hit.distance && primitive < result->primitive))) {
                    result = PrimitiveHit{primitive, *hit};
                    if (any_hit) {
                        return result;
                    }
                    interval.maximum = hit->distance;
                }
            }
        } else {
            const auto left = box_hit(nodes_[node.first].bounds);
            const auto right = box_hit(nodes_[node.right].bounds);
            if (left && right) {
                const bool left_nearer = *left <= *right;
                stack[pending++] =
                    left_nearer ? Entry{node.right, *right} : Entry{node.first, *left};
                stack[pending++] =
                    left_nearer ? Entry{node.first, *left} : Entry{node.right, *right};
            } else if (left) {
                stack[pending++] = {node.first, *left};
            } else if (right) {
                stack[pending++] = {node.right, *right};
            }
        }
    }
    return result;
}

SurfaceHit SceneBvh::surface_hit(const Ray& ray, const PrimitiveHit& hit) const {
    const auto& primitive = primitives_[hit.primitive];
    const auto& instance = scene_->instances[primitive.instance];
    const auto& triangle = instance.mesh->triangles[primitive.triangle];
    const auto edge1 = glm::dvec3(primitive.vertices[1]) - glm::dvec3(primitive.vertices[0]);
    const auto edge2 = glm::dvec3(primitive.vertices[2]) - glm::dvec3(primitive.vertices[0]);
    auto geometric = glm::normalize(glm::cross(edge1, edge2));
    if (mirrored_[primitive.instance]) {
        geometric = -geometric;
    }
    glm::dvec3 shading(0);
    glm::vec2 uv(0);
    for (std::size_t corner = 0; corner < 3; ++corner) {
        const auto& data = triangle.corners[corner];
        const double weight = hit.hit.barycentric[static_cast<int>(corner)];
        shading +=
            weight * glm::normalize(glm::dmat3(instance.normal_to_world) * glm::dvec3(data.normal));
        uv += static_cast<float>(weight) * data.uv;
    }
    if (glm::dot(shading, shading) < 1e-24) {
        shading = geometric;
    } else {
        shading = glm::normalize(shading);
        if (glm::dot(shading, geometric) < 0) {
            shading = -shading;
        }
    }
    return {hit.hit.distance,
            glm::vec3(glm::dvec3(ray.origin) + hit.hit.distance * glm::dvec3(ray.direction)),
            glm::vec3(geometric),
            glm::vec3(shading),
            uv,
            hit.hit.barycentric,
            primitive.instance,
            primitive.triangle,
            glm::dot(glm::dvec3(ray.direction), geometric) < 0};
}

std::optional<SurfaceHit> SceneBvh::intersect(const Ray& ray, RayInterval interval,
                                              TraceStatistics* statistics) const {
    const auto hit = trace(RayQuery(ray), interval, false, statistics);
    return hit ? std::optional(surface_hit(ray, *hit)) : std::nullopt;
}
bool SceneBvh::occluded(const Ray& ray, RayInterval interval, TraceStatistics* statistics) const {
    return trace(RayQuery(ray), interval, true, statistics).has_value();
}

} // namespace minirender
