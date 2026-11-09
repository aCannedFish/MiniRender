#pragma once

#include <minirender/geometry/halfedge.hpp>

#include <functional>

namespace minirender {

// 最近点在网格局部坐标中。barycentric 按原三角形面角顺序，distance_squared 单位 m²。
struct MeshClosestPoint {
    glm::dvec3 position{0};
    glm::dvec3 barycentric{0};
    double distance_squared = std::numeric_limits<double>::infinity();
    MeshIndex triangle = invalid_mesh_index;
};

// 只读局部表面查询，拥有不可变网格，使用中位数划分的 AABB 树剪枝。
// 构造时校验网格数据与非退化三角形。查询无共享写入，可供多个 CPU worker 使用。
// 与渲染场景/BVH 独立，用于几何误差评估及重网格后的表面投影。
class MeshSurface {
  public:
    explicit MeshSurface(std::shared_ptr<const MeshData> mesh, CancellationToken cancellation = {});
    MeshClosestPoint closest(glm::dvec3 point, CancellationToken cancellation = {}) const;

  private:
    struct Node {
        glm::dvec3 low{0}, high{0};
        MeshIndex first = 0, count = 0, right = 0;
    };
    MeshIndex build(MeshIndex begin, MeshIndex end, const CancellationToken& cancellation);
    std::shared_ptr<const MeshData> mesh_;
    std::vector<MeshIndex> order_;
    std::vector<Node> nodes_;
};

struct SurfaceDistance {
    std::size_t samples = 0;
    double rms = 0;     // 采样点到另一网格三角形表面的 RMS，单位 m。
    double maximum = 0; // 相同采样集合中的最大距离，单位 m。
};

struct SurfaceComparison {
    SurfaceDistance original_to_result, result_to_original;
    double symmetric_rms = 0;
    double sampled_maximum = 0;
};

using SurfaceProgress = std::function<void(std::size_t completed, std::size_t total)>;

// 两个方向均查询所有被引用顶点和所有三角形重心，按样本数合并 RMS。
// 这是确定性的离散采样误差；maximum 是样本最大值。孤立位置不参与表面误差。
// 包含树构建和逐样本查询的取消检查，回调在调用线程执行。
SurfaceComparison compare_surfaces(std::shared_ptr<const MeshData> original,
                                   std::shared_ptr<const MeshData> result,
                                   CancellationToken cancellation = {},
                                   SurfaceProgress progress = {});

} // namespace minirender
