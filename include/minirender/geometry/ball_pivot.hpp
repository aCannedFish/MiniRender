#pragma once

#include <minirender/geometry/halfedge.hpp>
#include <minirender/geometry/point_cloud.hpp>

namespace minirender {

// 半径递增，单位为局部米。候选预算覆盖种子组合、接触球及空球检查。
struct BallPivotSettings {
    std::vector<double> radii{.05, .1, .2};
    std::size_t maximum_triangles = 2000000;
    std::size_t maximum_neighbors = 4096;
    std::size_t maximum_candidate_tests = 20000000;
    void validate() const;
};

enum class BallPivotStage {
    validation,
    duplicates,
    index,
    reactivation,
    seeds,
    pivoting,
    output,
    complete
};
struct BallPivotProgress {
    BallPivotStage stage;
    std::size_t completed;
    std::size_t total;
    std::size_t radius_index;
    std::size_t radius_count;
    std::size_t triangles;
    std::size_t candidate_tests;
};
using BallPivotCallback = std::function<void(const BallPivotProgress&)>;

struct BallPivotPass {
    double radius = 0;
    std::size_t new_triangles = 0;
    std::size_t total_triangles = 0;
    std::size_t boundary_edges = 0;
    std::size_t restarted_edges = 0;
};

// 输出只包含被重建三角形引用的点，source_indices 将位置映射回首条原始输入记录。
// 未闭合的多个面扇使用独立输出顶点，split_vertices 记录额外副本。
// 无合法种子时 mesh 为空；其余结果允许保留孔洞，由 topology 与 unused_points 报告。
struct BallPivotResult {
    std::shared_ptr<const MeshData> mesh;
    TopologyStatistics topology;
    std::vector<std::uint32_t> source_indices;
    std::vector<BallPivotPass> passes;
    std::size_t input_points = 0;
    std::size_t unique_points = 0;
    std::size_t duplicate_points = 0;
    std::size_t unused_points = 0;
    std::size_t used_points = 0;
    std::size_t split_vertices = 0;
    std::size_t seed_triangles = 0;
    std::size_t pivot_triangles = 0;
    std::size_t rejected_normals = 0;
    std::size_t rejected_topology = 0;
    std::size_t candidate_tests = 0;
};

// 输入须包含表面法线，可先调用 prepare_point_normals。各轮复用已有网格与前沿。
// 增量检查边方向与面扇，输出按独立面扇整理后执行完整半边验证。
// 失败或取消不发布部分网格。
BallPivotResult reconstruct_ball_pivot(std::shared_ptr<const PointCloud> input,
                                       BallPivotSettings settings = {},
                                       CancellationToken cancellation = {},
                                       BallPivotCallback progress = {});

} // namespace minirender
