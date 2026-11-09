#include "point_cloud_utils.hpp"
#include "point_index.hpp"
#include "symmetric_eigen.hpp"
#include <minirender/geometry/point_normals.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace minirender {
namespace {

// Kruskal 只保留森林边，后续方向传播沿该森林访问每个点一次。
struct NormalEdge {
    std::uint32_t a;
    std::uint32_t b;
    double cost;
};
// 按秩合并与路径压缩维护森林分量，Kruskal 据此排除形成环的边。
class DisjointSets final {
  public:
    explicit DisjointSets(std::size_t size) : parents_(size), ranks_(size, 0) {
        std::iota(parents_.begin(), parents_.end(), std::uint32_t{0});
    }
    std::uint32_t root(std::uint32_t index) {
        while (parents_[index] != index) {
            parents_[index] = parents_[parents_[index]];
            index = parents_[index];
        }
        return index;
    }
    bool join(std::uint32_t a, std::uint32_t b) {
        a = root(a);
        b = root(b);
        if (a == b) {
            return false;
        }
        if (ranks_[a] < ranks_[b]) {
            std::swap(a, b);
        }
        parents_[b] = a;
        if (ranks_[a] == ranks_[b]) {
            ++ranks_[a];
        }
        return true;
    }

  private:
    std::vector<std::uint32_t> parents_;
    std::vector<std::uint8_t> ranks_;
};

// 坐标先相对中心点平移，再计算均值与协方差，避免大平移参与相减。
glm::vec3 fit_normal(const PointCloud& cloud, std::uint32_t point,
                     const std::vector<detail::PointNeighbor>& neighbors) {
    if (neighbors.size() < 2) {
        throw std::invalid_argument("Point PCA has fewer than three neighborhood samples at " +
                                    std::to_string(point));
    }
    const auto anchor = glm::dvec3(cloud.positions[point]);
    glm::dvec3 mean(0);
    for (const auto& neighbor : neighbors) {
        mean += glm::dvec3(cloud.positions[neighbor.index]) - anchor;
    }
    mean /= static_cast<double>(neighbors.size() + 1);
    glm::dmat3 covariance = glm::outerProduct(-mean, -mean);
    for (const auto& neighbor : neighbors) {
        const auto delta = glm::dvec3(cloud.positions[neighbor.index]) - anchor - mean;
        covariance += glm::outerProduct(delta, delta);
    }
    const auto eigen = detail::symmetric_eigen(covariance);
    if (eigen.values.z <= 0 || eigen.values.y <= eigen.values.z * 1e-10 ||
        eigen.values.y - eigen.values.x <= eigen.values.z * 1e-10) {
        throw std::invalid_argument("Point PCA has no stable surface normal at " +
                                    std::to_string(point));
    }
    auto normal = eigen.vectors[0];
    int axis = 0;
    for (int i = 1; i < 3; ++i) {
        if (std::abs(normal[i]) > std::abs(normal[axis])) {
            axis = i;
        }
    }
    if (normal[axis] < 0) {
        normal = -normal;
    }
    return glm::vec3(normal);
}

} // namespace

void PointNormalSettings::validate() const {
    if (neighbors < 3 || neighbors > 128 || std::isnan(maximum_distance) || maximum_distance <= 0 ||
        maximum_neighbor_links == 0) {
        throw std::invalid_argument(
            "Point normals require 3..128 neighbors and positive distance/work budgets");
    }
}

PointNormalResult prepare_point_normals(std::shared_ptr<const PointCloud> input,
                                        PointNormalSettings settings,
                                        CancellationToken cancellation,
                                        PointNormalCallback progress) {
    cancellation.check();
    settings.validate();
    if (!input || input->positions.size() > 2000000) {
        throw std::invalid_argument("Point normal input is missing or exceeds two million points");
    }
    const auto report = [&](PointNormalStage stage, std::size_t done, std::size_t total) {
        cancellation.check();
        if (progress) {
            progress({stage, done, total});
        }
        cancellation.check();
    };
    report(PointNormalStage::validation, 0, 1);
    input->validate(cancellation);
    report(PointNormalStage::validation, 1, 1);
    auto unique = detail::unique_points(*input, cancellation, [&](auto done, auto total) {
        report(PointNormalStage::duplicates, done, total);
    });
    auto cloud = std::make_shared<PointCloud>(std::move(unique.cloud));
    const auto count = cloud->positions.size();
    if (count < 3 || count > settings.maximum_neighbor_links / (settings.neighbors - 1)) {
        throw std::length_error(
            "Point normal input has too few unique points or exceeds its neighbor budget");
    }
    if (!settings.estimate && cloud->normals.empty()) {
        throw std::invalid_argument("Preserving point normals requires normals in the input");
    }
    report(PointNormalStage::index, 0, 1);
    const detail::PointIndex index(cloud->positions, cancellation);
    report(PointNormalStage::index, 1, 1);
    cloud->normals.resize(count);
    std::vector<NormalEdge> edges;
    std::vector<double> spacing;
    report(PointNormalStage::estimation, 0, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto neighbors = index.nearest(glm::dvec3(cloud->positions[i]),
                                             settings.neighbors - 1, i, settings.maximum_distance);
        if (!neighbors.empty()) {
            spacing.push_back(std::sqrt(neighbors.front().squared_distance));
        }
        cloud->normals[i] = settings.estimate
                                ? fit_normal(*cloud, i, neighbors)
                                : glm::vec3(glm::normalize(glm::dvec3(cloud->normals[i])));
        // 邻接暂存端点，待全部法线完成后再计算权重。
        if (settings.orient) {
            for (const auto& neighbor : neighbors) {
                edges.push_back({std::min(i, neighbor.index), std::max(i, neighbor.index), 0});
            }
        }
        if (i % 256 == 0) {
            report(PointNormalStage::estimation, i + 1, count);
        }
    }
    report(PointNormalStage::estimation, count, count);
    PointNormalResult result;
    std::vector<bool> reversed(count, false);
    if (settings.orient) {
        report(PointNormalStage::orientation, 0, count);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            if (i % 1024 == 0) {
                cancellation.check();
            }
            auto& edge = edges[i];
            edge.cost = 1 - std::clamp(std::abs(glm::dot(glm::dvec3(cloud->normals[edge.a]),
                                                         glm::dvec3(cloud->normals[edge.b]))),
                                       0.0, 1.0);
        }
        std::size_t comparisons = 0;
        std::sort(edges.begin(), edges.end(), [&](const auto& a, const auto& b) {
            if (++comparisons % 4096 == 0) {
                cancellation.check();
            }
            return std::tie(a.cost, a.a, a.b) < std::tie(b.cost, b.a, b.b);
        });
        DisjointSets sets(count);
        std::vector<std::array<std::uint32_t, 2>> tree;
        std::vector<std::size_t> offsets(count + 1, 0);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            if (i % 1024 == 0) {
                cancellation.check();
            }
            const auto& edge = edges[i];
            if (sets.join(edge.a, edge.b)) {
                tree.push_back({edge.a, edge.b});
                ++offsets[edge.a + 1];
                ++offsets[edge.b + 1];
            }
        }
        edges.clear();
        edges.shrink_to_fit();
        std::vector<std::uint32_t> roots(count, detail::no_point);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (i % 1024 == 0) {
                cancellation.check();
            }
            auto& root = roots[sets.root(i)];
            if (root == detail::no_point || cloud->positions[i].z > cloud->positions[root].z) {
                root = i;
            }
            offsets[i + 1] += offsets[i];
        }
        std::vector<std::uint32_t> adjacent(offsets.back());
        auto cursors = offsets;
        for (std::size_t i = 0; i < tree.size(); ++i) {
            if (i % 1024 == 0) {
                cancellation.check();
            }
            const auto [a, b] = tree[i];
            adjacent[cursors[a]++] = b;
            adjacent[cursors[b]++] = a;
        }
        std::size_t completed = 0;
        for (const auto root : roots) {
            if (root == detail::no_point) {
                continue;
            }
            ++result.orientation_components;
            const auto n = cloud->normals[root];
            if ((n.z != 0 ? n.z : n.y != 0 ? n.y : n.x) < 0) {
                cloud->normals[root] = -n;
                reversed[root] = true;
            }
            std::vector<std::array<std::uint32_t, 2>> pending{{root, detail::no_point}};
            while (!pending.empty()) {
                const auto [point, parent] = pending.back();
                pending.pop_back();
                for (auto cursor = offsets[point]; cursor < offsets[point + 1]; ++cursor) {
                    const auto next = adjacent[cursor];
                    if (next == parent) {
                        continue;
                    }
                    if (glm::dot(cloud->normals[point], cloud->normals[next]) < 0) {
                        cloud->normals[next] = -cloud->normals[next];
                        reversed[next] = true;
                    }
                    pending.push_back({next, point});
                }
                if (++completed % 256 == 0) {
                    report(PointNormalStage::orientation, completed, count);
                }
            }
        }
        report(PointNormalStage::orientation, count, count);
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 1024 == 0) {
            cancellation.check();
        }
        if (settings.flip) {
            cloud->normals[i] = -cloud->normals[i];
        }
        result.reversed_normals += reversed[i] != settings.flip ? 1 : 0;
    }
    std::size_t comparisons = 0;
    std::sort(spacing.begin(), spacing.end(), [&](double a, double b) {
        if (++comparisons % 4096 == 0) {
            cancellation.check();
        }
        return a < b;
    });
    if (!spacing.empty()) {
        result.spacing_p10 = spacing[(spacing.size() - 1) / 10];
        result.spacing_median = spacing[(spacing.size() - 1) / 2];
        result.spacing_p90 = spacing[(spacing.size() - 1) * 9 / 10];
    }
    cloud->validate(cancellation);
    result.cloud = std::move(cloud);
    result.source_indices = std::move(unique.source_indices);
    result.duplicate_points = input->positions.size() - count;
    report(PointNormalStage::complete, 1, 1);
    return result;
}

} // namespace minirender
