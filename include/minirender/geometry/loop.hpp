#pragma once

#include <minirender/geometry/halfedge.hpp>

#include <functional>

namespace minirender {

// 均匀细分每层使面数变为四倍。先检查全部层的预算，再分配输出。
// maximum_triangles 限制最终面数，取值 1..16000000；levels 取值 0..8。
struct LoopSettings {
    std::uint32_t levels = 1;
    std::size_t maximum_triangles = 2000000;
    void validate() const;
};

enum class LoopStage { input_topology, vertices, edges, faces, normals, output_topology, complete };

// level 为当前处理层，输入层为 0。completed/total 属于当前阶段，不跨阶段累加。
// topology 阶段以一次完整校验为单位；其余阶段以实际处理的位置、边、面或面角计数。
struct LoopProgress {
    LoopStage stage = LoopStage::input_topology;
    std::uint32_t level = 0;
    std::size_t completed = 0;
    std::size_t total = 0;
};
using LoopProgressCallback = std::function<void(const LoopProgress&)>;

struct LoopResult {
    std::shared_ptr<const MeshData> mesh;
    std::vector<TopologyStatistics> levels; // 输入及每层结果，故长度为 settings.levels + 1。
};

// 在物体局部坐标中执行有限层 Loop 细分，使用原始余弦权重及三次 B-spline 边界规则。
// 每条共享边只新增一个位置。UV 按各父面线性插值，保留接缝；法线按面积重新平滑。
// levels=0 校验后返回原 shared_ptr，保留原属性。输入与已发布结果均不修改。
// 同步执行，回调在调用线程触发；Qt 接入应放在 worker。取消/异常不发布部分结果。
// 要求子面法线在父面法线上的投影为正，非法拓扑、退化结果和无法定义的法线报错。
// 不进行极限面投影、折痕保护或全局自交检测。
LoopResult subdivide_loop(std::shared_ptr<const MeshData> input, LoopSettings settings = {},
                          CancellationToken cancellation = {}, LoopProgressCallback progress = {});

} // namespace minirender
