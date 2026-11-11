#pragma once

#include <glm/vec3.hpp>
#include <minirender/core/cancellation.hpp>

#include <span>
#include <string>

namespace minirender {

// 均匀 RGB 介质。吸收与散射系数单位为 m^-1；g 为 HG 前向散射的平均余弦。
struct HomogeneousMedium {
    glm::dvec3 absorption{0};
    glm::dvec3 scattering{0};
    double anisotropy = 0;
    glm::dvec3 extinction() const noexcept;
    void validate() const;
};

// 世界空间轴对齐盒，边界只有介质语义。enabled 独立于表面物体的显示状态。
// 启用区域的内部互不重叠；共用边界允许相接。
struct MediumRegion {
    std::string id;
    std::string name;
    glm::dvec3 minimum{-1};
    glm::dvec3 maximum{1};
    HomogeneousMedium medium;
    bool enabled = true;
    void validate() const;
};

inline constexpr std::size_t maximum_medium_regions = 32;
void validate_medium_regions(std::span<const MediumRegion> regions,
                             CancellationToken cancellation = {});

} // namespace minirender
