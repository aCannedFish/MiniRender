// 各向同性重网格的分裂、折叠、翻边、平滑和表面投影。

#pragma once

#include <minirender/geometry/surface.hpp>

namespace minirender {

// 各向同性重网格的目标为局部米制边长。每轮包含分裂、折叠、翻边、切向平滑和表面投影。
// 边界/特征位置固定，长特征边可细分；UV 与硬法线接缝按面角保留。
struct RemeshSettings {
    double target_edge_length = 0.1;
    // 重网格分裂、折叠、翻边与平滑的迭代预算。
    std::uint32_t iterations = 5;
    // 输出网格的三角形数量上限，分配前检查预算。
    std::size_t maximum_triangles     = 2000000;
    double      smoothing_step        = 0.5;
    bool        project_to_surface    = true;
    bool        preserve_boundary     = true;
    bool        preserve_uv_seams     = true;
    bool        preserve_hard_normals = true;
    bool        preserve_creases      = true;
    double      crease_angle_degrees  = 60;
    // 检查数值范围和预算组合，非法设置在修改网格前抛出异常。
    void validate() const;
};

// 对唯一几何边统计长度；minimum_angle 是全部三角形中的最小内角，单位度。
// in_target_range 计入 [4L/5, 4L/3] 区间，包含保护边，便于观察约束后的实际结果。
struct MeshQuality {
    // 用于边长质量统计的边数。
    std::size_t edges = 0;
    // 边长落入目标区间的边数。
    std::size_t in_target_range = 0;
    // 最短边长，单位为局部米。
    double minimum_edge = 0;
    // 平均边长，单位为局部米。
    double mean_edge = 0;
    // 边长中位数，单位为局部米。
    double median_edge = 0;
    // 最长边长，单位为局部米。
    double maximum_edge = 0;
    // 边长的第 10 百分位数。
    double edge_p10 = 0;
    // 边长的第 90 百分位数。
    double edge_p90 = 0;
    // 全部三角形的最小内角，单位为度。
    double minimum_angle        = 0;
    double mean_angle_deviation = 0; // 全部内角相对 60° 的平均绝对偏差。
};

enum class RemeshStage {
    input_topology,
    reference_surface,
    edge_queue,
    split,
    collapse,
    flip,
    smooth,
    projection,
    output,
    quality,
    error,
    complete
};

// 数量在每个阶段内计数；iteration 的预处理阶段为 0，迭代从 1 开始。
struct RemeshProgress {
    // 当前执行阶段，completed 和 total 在阶段内解释。
    RemeshStage stage = RemeshStage::input_topology;
    // 当前迭代编号，预处理阶段为 0。
    std::uint32_t iteration = 0;
    // 当前阶段已完成的工作项数量。
    std::size_t completed = 0;
    // 当前阶段工作项总数。
    std::size_t total = 0;
    // 当前工作网格的面数。
    std::size_t current_triangles = 0;
};

using RemeshProgressCallback = std::function<void(const RemeshProgress&)>;

// 操作次数跨轮累加；质量描述边长与角度分布，error 测量相对输入的表面距离。
struct RemeshResult {
    // 成功结果的共享只读网格，调用方可以直接保存或交给渲染器。
    std::shared_ptr<const MeshData> mesh;
    double                          target_edge_length = 0;
    // 算法开始时输入网格的拓扑统计。
    TopologyStatistics input_topology;
    // 输出网格通过拓扑检查后的统计。
    TopologyStatistics topology;
    // 输入网格的边长和角度质量统计。
    MeshQuality before;
    // 输出网格的边长和角度质量统计。
    MeshQuality after;
    // 相对输入参考曲面的双向采样距离统计。
    SurfaceComparison error;
    // 成功分裂长边的累计次数。
    std::size_t splits = 0;
    // 成功执行的边折叠次数。
    std::size_t collapses = 0;
    // 成功执行价数改善翻边的累计次数。
    std::size_t flips              = 0;
    std::size_t vertex_updates     = 0; // 累计位置或 UV 更新次数。
    std::size_t rejected_moves     = 0;
    std::size_t rejected_splits    = 0;
    std::size_t rejected_collapses = 0;
    // 因几何或拓扑条件不满足而放弃的翻边次数。
    std::size_t rejected_flips = 0;
    // 因边界、接缝或其他约束保留的顶点数。
    std::size_t protected_vertices = 0;
    // 实际完成的完整迭代轮数。
    std::uint32_t iterations_completed = 0;
};

// 保持有向流形连接，拒绝退化、局部翻转、重复面和边界夹断。
// 表面投影按保护边切分的输入片区查询，并传递 UV；保留连通分量及孤立位置。
// 输入及已发布结果始终不可变。
// 同步计算，回调在调用线程执行；Qt 调用方在 worker 运行。取消不发布部分网格。
RemeshResult remesh_isotropic(std::shared_ptr<const MeshData> input, RemeshSettings settings = {},
                              CancellationToken      cancellation = {},
                              RemeshProgressCallback progress     = {});

} // namespace minirender
