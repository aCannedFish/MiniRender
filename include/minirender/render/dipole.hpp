// 均匀半无限介质的 dipole 扩散模型及径向分布。

#pragma once

#include <minirender/scene/subsurface.hpp>

namespace minirender {

// Jensen dipole 的径向扩散核。所有距离为世界空间米，profile 的单位为 m^-2。
// 解析面积累计用于归一和反演，避免表格分辨率或人为尾部截断改变核的能量。
class DipoleProfile final {
  public:
    // 根据光学系数与边界折射率预计算每个颜色通道的扩散源深度。
    DipoleProfile(SubsurfaceParameters parameters, double ior);
    // 返回距入射点 radius 米处的 RGB 扩散剖面。
    glm::dvec3 evaluate(double radius) const;
    // 在半径 radius 的圆盘上对剖面积分，给出三个通道的累计反射量。
    glm::dvec3 cumulative(double radius) const;
    // 返回整个切平面上积分所得的 RGB 反射率。
    glm::dvec3 reflectance() const noexcept;
    // 在 [0,maximum_radius] 按 2*pi*r*profile[channel] 归一采样。
    double sample_radius(int channel, double u, double maximum_radius) const;
    // 外侧方向核 Ft(cos)/pi；其余弦半球积分为 external_transmission()。
    double directional(double cosine) const;
    // 读取外侧入射方向的平均透射因子。
    double external_transmission() const noexcept;
    // 读取内部方向半球平均的边界反射率。
    double internal_reflectance() const noexcept;
    // 返回计算边界 Fresnel 项时使用的折射率。
    double ior() const noexcept;

  private:
    // 每通道保存约化反照率、真实源与虚拟源深度及衰减系数。
    struct Channel {
        double albedo;
        double real_depth;
        double virtual_depth;
        double extinction;
    };

    // 分别积分真实源与虚拟源的扩散贡献，供累计量计算和半径反演使用。
    double integral(const Channel& channel, double radius) const;
    Channel    channels_[3];
    glm::dvec3 reflectance_;
    double     ior_;
    double     external_transmission_;
    double     internal_reflectance_;
};

} // namespace minirender
