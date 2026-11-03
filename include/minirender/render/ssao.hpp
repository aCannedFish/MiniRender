#pragma once

#include <array>
#include <cstdint>
#include <glm/vec4.hpp>

namespace minirender {

// SSAO 参数使用相机空间的米。只遮蔽环境漫反射，离线路径追踪不消费这些参数。
struct SsaoSettings {
    bool enabled = false;
    bool filtered = true;
    float radius = 0.5F;
    float bias = 0.02F;
    float strength = 1;
    std::uint32_t samples = 32;
    void validate() const;
};

inline constexpr std::size_t ssao_kernel_size = 64;
// 局部 +Z 余弦半球，长度偏向采样中心。每个前缀均覆盖完整半径，改变样本数不改变半径。
// shader 按像素旋转该固定核；vec4 满足 std140 数组的 16 字节步长。
std::array<glm::vec4, ssao_kernel_size> make_ssao_kernel(std::uint64_t seed = 101);

} // namespace minirender
