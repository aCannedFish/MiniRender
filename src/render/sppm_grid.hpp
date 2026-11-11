#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <minirender/core/cancellation.hpp>
#include <optional>
#include <span>
#include <vector>

namespace minirender::detail {
struct SppmGridPoint {
    glm::dvec3 position{0};
    double radius = 0;
    std::size_t index = 0;
};

// 可见点的半径盒最多登记到八个格子。哈希碰撞保留完整格坐标，查询再查球距离，
// 因而一个光子对同一可见点只累加一次。构造后只读，无逐光子堆分配。
class SppmGrid final {
  public:
    explicit SppmGrid(std::span<const SppmGridPoint> points, CancellationToken cancellation = {});
    static std::uint64_t estimated_bytes(std::size_t points);
    bool empty() const noexcept {
        return points_.empty();
    }
    template <class Function> void query(glm::dvec3 position, Function function) const {
        const auto coordinate = cell(position);
        if (!coordinate) {
            return;
        }
        for (auto i = heads_[hash(*coordinate)]; i != none; i = entries_[i].next) {
            const auto& entry = entries_[i];
            if (entry.cell != *coordinate) {
                continue;
            }
            const auto& point = points_[entry.point];
            const auto delta = position - point.position;
            if (glm::dot(delta, delta) <= point.radius * point.radius) {
                function(point.index);
            }
        }
    }

  private:
    using Cell = std::array<std::int64_t, 3>;
    struct Entry {
        Cell cell;
        std::size_t point, next;
    };
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    std::optional<Cell> cell(glm::dvec3 point) const;
    std::size_t hash(const Cell& cell) const;
    glm::dvec3 minimum_{0}, maximum_{0};
    double cell_size_ = 1;
    std::vector<SppmGridPoint> points_;
    std::vector<std::size_t> heads_;
    std::vector<Entry> entries_;
};
} // namespace minirender::detail
