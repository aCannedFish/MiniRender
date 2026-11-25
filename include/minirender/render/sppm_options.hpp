// SPPM 迭代预算、初始半径和渐进更新参数。

#pragma once

#include <cstdint>

namespace minirender {

// 每个像素每轮重新发射一条相机路径；RenderSettings.spp 对应完整迭代数。
// 半径以世界空间米表示，alpha 控制旧的宽核估计淡出速度。
struct SppmOptions {
    std::uint32_t photons_per_iteration = 100000;
    double        initial_radius        = .1;
    double        alpha                 = .7;
    std::uint64_t memory_budget_bytes   = 512ULL * 1024 * 1024;
    // 检查光子预算、初始合并半径及通量更新参数。
    void validate() const;
};

} // namespace minirender
