#pragma once

#include <minirender/geometry/halfedge.hpp>

#include <deque>
#include <unordered_map>

namespace minirender::detail {

// 开放一环的首尾为两个边界邻点；环中连续邻点与中心组成正向三角形。
struct LoopRing {
    std::vector<MeshIndex> neighbors;
    bool boundary = false;
};

// 按需生成虚拟均匀 Loop 拓扑。旧点在各层沿用几何 ID，新点由父边和层级唯一确定。
// 控制位置通过出生点和一环的有限次 even 递推计算，缓存只由所属几何 worker 使用。
// 该类不生成最终自适应三角形；终端过渡使用同一组虚拟点，避免修改 Loop 的一环规则。
class LoopHierarchy final {
  public:
    LoopHierarchy(std::shared_ptr<const MeshData> input, const HalfedgeMesh& topology,
                  std::size_t maximum_points, CancellationToken cancellation = {});
    MeshIndex midpoint(MeshIndex a, MeshIndex b, std::uint32_t parent_level);
    const LoopRing& ring(MeshIndex vertex, std::uint32_t level);
    // 分别为有向边 (a,b) 左右的对顶点；边界缺失的一侧为 invalid_mesh_index。
    std::array<MeshIndex, 2> opposites(MeshIndex a, MeshIndex b, std::uint32_t level);
    glm::dvec3 control_position(MeshIndex vertex, std::uint32_t level);
    glm::dvec3 smooth_normal(MeshIndex vertex, std::uint32_t level);
    std::uint32_t birth_level(MeshIndex vertex) const;
    std::array<MeshIndex, 2> parent_edge(MeshIndex vertex) const;
    std::size_t point_count() const noexcept;

  private:
    // 出生点的父层四点决定 odd 位置，delta/rho 决定之后任意有限层的 even 位置。
    struct Point {
        std::uint32_t birth = 0;
        MeshIndex a = invalid_mesh_index;
        MeshIndex b = invalid_mesh_index;
        MeshIndex left = invalid_mesh_index;
        MeshIndex right = invalid_mesh_index;
        bool boundary = false;
        bool ready = false;
        bool coefficient_ready = false;
        glm::dvec3 position{0};
        glm::dvec3 delta{0};
        double rho = 0;
    };
    struct EdgeKey {
        MeshIndex a;
        MeshIndex b;
        std::uint32_t level;
        bool operator==(const EdgeKey&) const = default;
    };
    struct EdgeHash {
        std::size_t operator()(const EdgeKey& key) const noexcept;
    };
    void validate_query(MeshIndex vertex, std::uint32_t level) const;
    glm::dvec3 birth_position(MeshIndex vertex);
    void prepare_coefficient(MeshIndex vertex);
    std::shared_ptr<const MeshData> input_;
    std::deque<Point> points_;
    std::unordered_map<EdgeKey, MeshIndex, EdgeHash> edges_;
    std::unordered_map<std::uint64_t, LoopRing> rings_;
    std::size_t maximum_points_;
    CancellationToken cancellation_;
};

} // namespace minirender::detail
