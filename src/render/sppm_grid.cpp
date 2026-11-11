#include "sppm_grid.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender::detail {
namespace {
bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace
std::uint64_t SppmGrid::estimated_bytes(std::size_t points) {
    return std::uint64_t(points) *
               (sizeof(SppmGridPoint) + 8 * sizeof(Entry) + 2 * sizeof(std::size_t)) +
           sizeof(std::size_t);
}
SppmGrid::SppmGrid(std::span<const SppmGridPoint> points, CancellationToken cancellation) {
    cancellation.check();
    if (points.empty()) {
        return;
    }
    minimum_ = glm::dvec3(std::numeric_limits<double>::infinity());
    maximum_ = -minimum_;
    double largest = 0;
    for (const auto& point : points) {
        cancellation.check();
        if (!finite(point.position) || !std::isfinite(point.radius) || point.radius <= 0) {
            throw std::invalid_argument("Invalid SPPM grid point or radius");
        }
        minimum_ = glm::min(minimum_, point.position - glm::dvec3(point.radius));
        maximum_ = glm::max(maximum_, point.position + glm::dvec3(point.radius));
        largest = std::max(largest, point.radius);
    }
    cell_size_ = 4 * largest;
    points_.assign(points.begin(), points.end());
    heads_.assign(2 * points.size() + 1, none);
    entries_.reserve(8 * points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        cancellation.check();
        const auto low = cell(points[i].position - glm::dvec3(points[i].radius));
        const auto high = cell(points[i].position + glm::dvec3(points[i].radius));
        if (!low || !high) {
            throw std::runtime_error("SPPM grid bounds are not representable");
        }
        for (int axis = 0; axis < 3; ++axis) {
            if ((*high)[std::size_t(axis)] - (*low)[std::size_t(axis)] > 1) {
                throw std::runtime_error("SPPM cell span exceeds its bounded allocation");
            }
        }
        for (auto z = (*low)[2]; z <= (*high)[2]; ++z) {
            for (auto y = (*low)[1]; y <= (*high)[1]; ++y) {
                for (auto x = (*low)[0]; x <= (*high)[0]; ++x) {
                    const Cell key{x, y, z};
                    const auto bucket = hash(key);
                    entries_.push_back({key, i, heads_[bucket]});
                    heads_[bucket] = entries_.size() - 1;
                }
            }
        }
    }
}
std::optional<SppmGrid::Cell> SppmGrid::cell(glm::dvec3 point) const {
    if (points_.empty() || !finite(point) || glm::any(glm::lessThan(point, minimum_)) ||
        glm::any(glm::greaterThan(point, maximum_))) {
        return {};
    }
    Cell result;
    const auto relative = (point - minimum_) / cell_size_;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double c = std::floor(relative[int(axis)]);
        if (!std::isfinite(c) || c < 0 ||
            c >= double(std::numeric_limits<std::int64_t>::max()) - 1024) {
            throw std::invalid_argument("SPPM grid coordinate exceeds int64; increase the radius");
        }
        result[axis] = std::int64_t(c);
    }
    return result;
}
std::size_t SppmGrid::hash(const Cell& cell) const {
    const auto x = std::uint64_t(cell[0]), y = std::uint64_t(cell[1]), z = std::uint64_t(cell[2]);
    return std::size_t((x * 73856093ULL) ^ (y * 19349663ULL) ^ (z * 83492791ULL)) % heads_.size();
}
} // namespace minirender::detail
