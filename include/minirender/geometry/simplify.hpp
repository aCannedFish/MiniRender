#pragma once

#include <minirender/geometry/surface.hpp>

namespace minirender {

// QEM 使用归一化平面的距离平方和，单位 m²，坐标为物体局部坐标。
// target_triangles 为希望达到的面数上限。闭合网格通常每次减少两面，可能略低于目标。
struct SimplifySettings {
    std::size_t target_triangles = 1000;
    double maximum_quadric_error = std::numeric_limits<double>::infinity();
    bool preserve_boundary = true;
    bool preserve_uv_seams = true;
    bool preserve_hard_normals = true;
    bool preserve_creases = true;
    double crease_angle_degrees = 60;
    void validate() const;
};

enum class SimplifyStop { target_reached, constrained, error_limit };
const char* simplify_stop_name(SimplifyStop stop) noexcept;
enum class SimplifyStage {
    input_topology,
    quadrics,
    candidates,
    collapse,
    output,
    error,
    complete
};
struct SimplifyProgress {
    SimplifyStage stage = SimplifyStage::input_topology;
    std::size_t completed = 0, total = 0;
    std::size_t current_triangles = 0;
};
using SimplifyProgressCallback = std::function<void(const SimplifyProgress&)>;

struct SimplifyResult {
    std::shared_ptr<const MeshData> mesh;
    TopologyStatistics input_topology, topology;
    SurfaceComparison error;
    std::size_t requested_triangles = 0;
    std::size_t collapses = 0;
    std::size_t protected_vertices = 0;
    std::size_t candidate_evaluations = 0;
    double maximum_quadric_error = 0; // 已接受折叠中的最大代价，独立于表面采样误差。
    SimplifyStop stop = SimplifyStop::target_reached;
};

// 流形三角网格上的 QEM 边折叠。维护局部邻接和版本化最小堆，拒绝拓扑改变与面翻转。
// 保护边界/折痕顶点的位置，接缝/硬法线顶点保留各面角属性并拒绝相邻边折叠。
// 连续 UV 按边上的投影参数插值，其余法线按面积重算。输入和已发布资产保持不可变。
// 目标已满足或没有合法折叠时返回原指针。无部分结果发布，取消抛 OperationCancelled。
SimplifyResult simplify_qem(std::shared_ptr<const MeshData> input, SimplifySettings settings = {},
                            CancellationToken cancellation = {},
                            SimplifyProgressCallback progress = {});

} // namespace minirender
