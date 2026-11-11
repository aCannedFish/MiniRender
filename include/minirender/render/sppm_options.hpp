#pragma once

#include <cstdint>

namespace minirender {

// 每个像素每轮重新发射一条相机路径；RenderSettings.spp 对应完整迭代数。
// 半径以世界空间米表示，alpha 控制旧的宽核估计淡出速度。
struct SppmOptions {
    std::uint32_t photons_per_iteration = 100000;
    double initial_radius = .1;
    double alpha = .7;
    std::uint64_t memory_budget_bytes = 512ULL * 1024 * 1024;
    void validate() const;
};

} // namespace minirender
