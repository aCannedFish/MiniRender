#pragma once

#include <minirender/geometry/point_cloud.hpp>

#include <limits>
#include <span>

namespace minirender::detail {

inline constexpr std::uint32_t no_point = std::numeric_limits<std::uint32_t>::max();
struct PointNeighbor {
    std::uint32_t index;
    double squared_distance;
};

// 固定点集的平衡 k-d 树。叶子持有位置索引，距离与边界使用 double。
// 查询按距离和原索引排序，树结构及查询结果不依赖哈希顺序。
class PointIndex final {
  public:
    explicit PointIndex(std::span<const glm::vec3> positions, CancellationToken cancellation = {});
    std::vector<PointNeighbor>
    nearest(glm::dvec3 point, std::size_t count, std::uint32_t exclude = no_point,
            double maximum_distance = std::numeric_limits<double>::infinity()) const;
    std::vector<PointNeighbor> radius(glm::dvec3 point, double distance,
                                      std::size_t maximum_results) const;

  private:
    struct Node {
        glm::dvec3 low;
        glm::dvec3 high;
        std::size_t begin;
        std::size_t end;
        std::uint32_t left = no_point;
        std::uint32_t right = no_point;
    };
    std::uint32_t build(std::size_t begin, std::size_t end);
    static double distance_to(const Node& node, glm::dvec3 point);
    std::span<const glm::vec3> positions_;
    std::vector<std::uint32_t> order_;
    std::vector<Node> nodes_;
    CancellationToken cancellation_;
};

} // namespace minirender::detail
