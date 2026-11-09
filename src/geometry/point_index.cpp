#include "point_index.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>

namespace minirender::detail {
namespace {
void validate_query(glm::dvec3 point, double distance) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        std::isnan(distance) || distance < 0) {
        throw std::invalid_argument(
            "Point query requires a finite position and nonnegative radius");
    }
}
bool ordered(const PointNeighbor& a, const PointNeighbor& b) {
    return a.squared_distance == b.squared_distance ? a.index < b.index
                                                    : a.squared_distance < b.squared_distance;
}
} // namespace

PointIndex::PointIndex(std::span<const glm::vec3> positions, CancellationToken cancellation)
    : positions_(positions), cancellation_(cancellation) {
    cancellation_.check();
    if (positions_.empty() || positions_.size() >= no_point) {
        throw std::invalid_argument("Point index requires a nonempty 32-bit point array");
    }
    order_.resize(positions_.size());
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        if (i % 1024 == 0) {
            cancellation_.check();
        }
        const auto p = positions_[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            throw std::invalid_argument("Point index position is not finite");
        }
        order_[i] = static_cast<std::uint32_t>(i);
    }
    build(0, order_.size());
}

std::uint32_t PointIndex::build(std::size_t begin, std::size_t end) {
    cancellation_.check();
    Node node{glm::dvec3(std::numeric_limits<double>::infinity()),
              glm::dvec3(-std::numeric_limits<double>::infinity()), begin, end};
    for (std::size_t i = begin; i < end; ++i) {
        if (i % 1024 == 0) {
            cancellation_.check();
        }
        const auto p = glm::dvec3(positions_[order_[i]]);
        node.low = glm::min(node.low, p);
        node.high = glm::max(node.high, p);
    }
    const auto index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(node);
    if (end - begin > 16) {
        const auto size = node.high - node.low;
        const int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
        const auto middle = begin + (end - begin) / 2;
        std::size_t comparisons = 0;
        std::nth_element(order_.begin() + static_cast<std::ptrdiff_t>(begin),
                         order_.begin() + static_cast<std::ptrdiff_t>(middle),
                         order_.begin() + static_cast<std::ptrdiff_t>(end), [&](auto a, auto b) {
                             if (++comparisons % 4096 == 0) {
                                 cancellation_.check();
                             }
                             return positions_[a][axis] == positions_[b][axis]
                                        ? a < b
                                        : positions_[a][axis] < positions_[b][axis];
                         });
        const auto left = build(begin, middle);
        const auto right = build(middle, end);
        nodes_[index].left = left;
        nodes_[index].right = right;
    }
    return index;
}

double PointIndex::distance_to(const Node& node, glm::dvec3 point) {
    const auto delta = glm::max(glm::max(node.low - point, point - node.high), glm::dvec3(0));
    return glm::dot(delta, delta);
}

std::vector<PointNeighbor> PointIndex::nearest(glm::dvec3 point, std::size_t count,
                                               std::uint32_t exclude,
                                               double maximum_distance) const {
    cancellation_.check();
    validate_query(point, maximum_distance);
    if (count == 0) {
        return {};
    }
    using Candidate = std::pair<double, std::uint32_t>;
    std::priority_queue<Candidate> heap;
    const double bound = maximum_distance * maximum_distance;
    std::vector<std::uint32_t> pending{0};
    std::size_t visits = 0;
    while (!pending.empty()) {
        if (++visits % 128 == 0) {
            cancellation_.check();
        }
        const auto index = pending.back();
        pending.pop_back();
        const auto& node = nodes_[index];
        const double farthest = heap.size() < count ? bound : std::min(bound, heap.top().first);
        if (distance_to(node, point) > farthest) {
            continue;
        }
        if (node.left == no_point) {
            for (std::size_t i = node.begin; i < node.end; ++i) {
                const auto id = order_[i];
                if (id == exclude) {
                    continue;
                }
                const auto delta = point - glm::dvec3(positions_[id]);
                const Candidate candidate{glm::dot(delta, delta), id};
                if (candidate.first <= bound && (heap.size() < count || candidate < heap.top())) {
                    heap.push(candidate);
                    if (heap.size() > count) {
                        heap.pop();
                    }
                }
            }
        } else {
            const bool left_first =
                distance_to(nodes_[node.left], point) <= distance_to(nodes_[node.right], point);
            pending.push_back(left_first ? node.right : node.left);
            pending.push_back(left_first ? node.left : node.right);
        }
    }
    std::vector<PointNeighbor> result;
    result.reserve(heap.size());
    while (!heap.empty()) {
        const auto [distance, id] = heap.top();
        result.push_back({id, distance});
        heap.pop();
    }
    std::reverse(result.begin(), result.end());
    cancellation_.check();
    return result;
}

std::vector<PointNeighbor> PointIndex::radius(glm::dvec3 point, double distance,
                                              std::size_t maximum_results) const {
    cancellation_.check();
    validate_query(point, distance);
    const double bound = distance * distance;
    std::vector<PointNeighbor> result;
    std::vector<std::uint32_t> pending{0};
    std::size_t visits = 0;
    while (!pending.empty()) {
        if (++visits % 128 == 0) {
            cancellation_.check();
        }
        const auto& node = nodes_[pending.back()];
        pending.pop_back();
        if (distance_to(node, point) > bound) {
            continue;
        }
        if (node.left == no_point) {
            for (std::size_t i = node.begin; i < node.end; ++i) {
                const auto delta = point - glm::dvec3(positions_[order_[i]]);
                const double squared = glm::dot(delta, delta);
                if (squared <= bound) {
                    if (result.size() == maximum_results) {
                        throw std::length_error("Point radius query exceeds its neighbor budget");
                    }
                    result.push_back({order_[i], squared});
                }
            }
        } else {
            pending.push_back(node.right);
            pending.push_back(node.left);
        }
    }
    std::size_t comparisons = 0;
    std::sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
        if (++comparisons % 4096 == 0) {
            cancellation_.check();
        }
        return ordered(a, b);
    });
    cancellation_.check();
    return result;
}

} // namespace minirender::detail
