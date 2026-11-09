#include "point_cloud_utils.hpp"
#include <minirender/core/sampling.hpp>
#include <minirender/geometry/point_cloud.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace minirender {
namespace {
bool finite(glm::vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

void PointCloud::validate(CancellationToken cancellation) const {
    cancellation.check();
    if (positions.empty() || positions.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Point cloud requires a nonempty 32-bit position array");
    }
    if (!normals.empty() && normals.size() != positions.size()) {
        throw std::invalid_argument("Point cloud normals must match its positions");
    }
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (i % 1024 == 0) {
            cancellation.check();
        }
        if (!finite(positions[i])) {
            throw std::invalid_argument("Point position is not finite at index " +
                                        std::to_string(i));
        }
        if (!normals.empty() && (!finite(normals[i]) ||
                                 glm::dot(glm::dvec3(normals[i]), glm::dvec3(normals[i])) == 0)) {
            throw std::invalid_argument("Point normal is invalid at index " + std::to_string(i));
        }
    }
    cancellation.check();
}

PointCloud sample_mesh_surface(const MeshData& mesh, std::size_t count, std::uint64_t seed,
                               CancellationToken cancellation, PointProgress progress) {
    mesh.validate(cancellation);
    if (count == 0 || count > 2000000) {
        throw std::invalid_argument("Surface sample count must be in 1..2000000");
    }
    const auto report = [&](std::size_t done) {
        cancellation.check();
        if (progress) {
            progress(done, mesh.triangles.size() + count);
        }
        cancellation.check();
    };
    std::vector<double> cumulative;
    cumulative.reserve(mesh.triangles.size());
    double area = 0;
    report(0);
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        const auto& c = mesh.triangles[i].corners;
        const auto a = glm::dvec3(mesh.positions[c[0].position]);
        const auto b = glm::dvec3(mesh.positions[c[1].position]);
        const auto d = glm::dvec3(mesh.positions[c[2].position]);
        area += glm::length(glm::cross(b - a, d - a)) * .5;
        cumulative.push_back(area);
        if (i % 1024 == 0) {
            report(i + 1);
        }
    }
    if (!std::isfinite(area) || area <= 0) {
        throw std::invalid_argument("Surface sampling requires positive finite total area");
    }
    PointCloud result;
    result.positions.reserve(count);
    result.normals.reserve(count);
    Sampler sampler(seed);
    for (std::size_t i = 0; i < count; ++i) {
        const auto target = sampler.next_1d() * area;
        const auto face = static_cast<std::size_t>(
            std::upper_bound(cumulative.begin(), cumulative.end(), target) - cumulative.begin());
        const auto& c = mesh.triangles[std::min(face, mesh.triangles.size() - 1)].corners;
        const double root = std::sqrt(sampler.next_1d());
        const double fraction = sampler.next_1d();
        const glm::dvec3 weights{1 - root, root * (1 - fraction), root * fraction};
        glm::dvec3 position(0);
        glm::dvec3 normal(0);
        for (std::size_t corner = 0; corner < 3; ++corner) {
            position +=
                weights[static_cast<int>(corner)] * glm::dvec3(mesh.positions[c[corner].position]);
            normal += weights[static_cast<int>(corner)] * glm::dvec3(c[corner].normal);
        }
        const double length = glm::length(normal);
        if (!std::isfinite(length) || length <= 1e-12) {
            throw std::invalid_argument("Surface sample has no stable interpolated normal");
        }
        result.positions.emplace_back(position);
        result.normals.emplace_back(normal / length);
        if (i % 1024 == 0) {
            report(mesh.triangles.size() + i + 1);
        }
    }
    result.validate(cancellation);
    report(mesh.triangles.size() + count);
    return result;
}

namespace detail {
namespace {
struct PositionKey {
    std::array<std::uint32_t, 3> components;
    bool operator==(const PositionKey&) const = default;
};
struct PositionHash {
    std::size_t operator()(const PositionKey& key) const noexcept {
        std::size_t hash = 0;
        for (const auto value : key.components) {
            hash ^= std::hash<std::uint32_t>{}(value) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        }
        return hash;
    }
};
} // namespace

UniquePointCloud unique_points(const PointCloud& input, CancellationToken cancellation,
                               PointProgress progress) {
    input.validate(cancellation);
    UniquePointCloud result;
    std::unordered_set<PositionKey, PositionHash> seen;
    const auto report = [&](std::size_t done) {
        cancellation.check();
        if (progress) {
            progress(done, input.positions.size());
        }
        cancellation.check();
    };
    report(0);
    for (std::size_t i = 0; i < input.positions.size(); ++i) {
        const auto p = input.positions[i];
        const PositionKey key{{p.x == 0 ? 0 : std::bit_cast<std::uint32_t>(p.x),
                               p.y == 0 ? 0 : std::bit_cast<std::uint32_t>(p.y),
                               p.z == 0 ? 0 : std::bit_cast<std::uint32_t>(p.z)}};
        if (seen.insert(key).second) {
            result.cloud.positions.push_back(p);
            result.source_indices.push_back(static_cast<std::uint32_t>(i));
            if (!input.normals.empty()) {
                result.cloud.normals.push_back(input.normals[i]);
            }
        }
        if ((i + 1) % 1024 == 0) {
            report(i + 1);
        }
    }
    report(input.positions.size());
    return result;
}
} // namespace detail

} // namespace minirender
