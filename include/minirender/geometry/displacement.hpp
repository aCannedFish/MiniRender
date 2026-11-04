#pragma once

#include <minirender/geometry/halfedge.hpp>

#include <functional>

namespace minirender {

// p' = p + normal * scale * (height_R - midlevel)。scale 为局部米制长度，允许负值。
// 高度使用 level 0、双线性、Repeat、OBJ 左下原点 UV，不做 gamma 或 [0,1] 截断。
struct DisplacementSettings {
    double scale = 0.1;
    double midlevel = 0.5;
    double seam_tolerance = 1e-6; // 接缝两侧允许的位移距离差，单位为局部米。
    std::size_t maximum_seam_segments = 1000000;
    void validate() const;
};

enum class DisplacementStage { topology, directions, samples, seams, positions, normals, complete };

// 各阶段独立计数。seams 以边计数，单条边的长扫描也定期发送当前边编号并检查取消。
struct DisplacementProgress {
    DisplacementStage stage = DisplacementStage::topology;
    std::size_t completed = 0;
    std::size_t total = 1;
};
using DisplacementProgressCallback = std::function<void(const DisplacementProgress&)>;

struct DisplacementResult {
    std::shared_ptr<const MeshData> mesh;
    TopologyStatistics topology;
    std::size_t sampled_corners = 0;
    std::size_t seam_edges = 0;
    std::size_t seam_segments = 0;
    double maximum_seam_difference = 0;
    double maximum_displacement = 0;
};

// 沿面积加权平滑法线移动共享位置，忽略输入的硬法线，随后重新计算输出法线。
// 接缝检查包含共享顶点及边内部双线性高度差的分段二次极值。超出检查预算时报错，
// 不跳过接缝或平均不连续高度。每个位置采用首次面角的高度，容差内也不进行平均。
// 输入和旧结果不修改，失败/取消不返回部分结果。拒绝退化及相对原面翻转的输出。
// 不检查全局自交，不更改拓扑；调用前可用 Loop 增加采样密度。
// scale=0 校验设置/拓扑后返回原网格，不扫描高度、不平滑原法线。
DisplacementResult displace_mesh(std::shared_ptr<const MeshData> input,
                                 std::shared_ptr<const Texture> height,
                                 DisplacementSettings settings = {},
                                 CancellationToken cancellation = {},
                                 DisplacementProgressCallback progress = {});

} // namespace minirender
