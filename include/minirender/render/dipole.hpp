#pragma once

#include <minirender/scene/subsurface.hpp>

namespace minirender {

// Jensen dipole 的径向扩散核。所有距离为世界空间米，profile 的单位为 m^-2。
// 解析面积累计用于归一和反演，避免表格分辨率或人为尾部截断改变核的能量。
class DipoleProfile final {
  public:
    DipoleProfile(SubsurfaceParameters parameters, double ior);
    glm::dvec3 evaluate(double radius) const;
    glm::dvec3 cumulative(double radius) const;
    glm::dvec3 reflectance() const noexcept;
    // 在 [0,maximum_radius] 按 2*pi*r*profile[channel] 归一采样。
    double sample_radius(int channel, double u, double maximum_radius) const;
    // 外侧方向核 Ft(cos)/pi；其余弦半球积分为 external_transmission()。
    double directional(double cosine) const;
    double external_transmission() const noexcept;
    double internal_reflectance() const noexcept;
    double ior() const noexcept;

  private:
    struct Channel {
        double albedo, real_depth, virtual_depth, extinction;
    };
    double integral(const Channel& channel, double radius) const;
    Channel channels_[3];
    glm::dvec3 reflectance_;
    double ior_, external_transmission_, internal_reflectance_;
};

} // namespace minirender
