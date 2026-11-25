// 均匀 RGB 介质及射线在有界区域内的吸收、散射处理。

#pragma once

#include <minirender/core/sampling.hpp>
#include <minirender/render/intersection.hpp>
#include <minirender/scene/medium.hpp>

#include <array>

namespace minirender {

// wo/wi 均为离开散射点的单位方向。正 g 的峰值位于 wi=-wo，密度为 sr^-1。
double henyey_greenstein(glm::dvec3 wo, glm::dvec3 wi, double g);

// 一次介质相函数采样的方向与立体角密度。
struct PhaseSample {
    glm::dvec3 direction{0};
    double     probability = 0;
};

// 精确 HG 逆 CDF，使用消除 1/g 消去误差的形式；g=0 直接连续退化为均匀球面。
PhaseSample sample_henyey_greenstein(glm::dvec3 wo, double g, glm::dvec2 u);

// Beer–Lambert 的逐通道透射率。distance 为非负米数，允许正无穷。
glm::dvec3 medium_transmittance(const HomogeneousMedium& medium, double distance);

// 自由程采样结果，weight 包含散射或逃逸事件的透射率与概率修正。
struct MediumDistanceSample {
    bool       scattered = false;
    double     distance  = 0;
    glm::dvec3 weight{1};
};

// 均匀选 RGB 通道后抽指数自由程。事件权重为 Tr*sigma_s/mean(Tr*sigma_t)，
// 逃逸权重为 Tr/mean(Tr)。channel_u、distance_u 位于 [0,1)。
MediumDistanceSample sample_medium_distance(const HomogeneousMedium& medium, double length,
                                            double channel_u, double distance_u);

// 射线与一个有界介质相交的参数区间，region 指向该介质。
struct MediumSegment {
    std::size_t region = 0;
    RayInterval interval;
};

// 固定预算避免逐射线分配堆内存；只有正长度的区段进入列表，按进入距离排序。
struct MediumSegments {
    std::array<MediumSegment, maximum_medium_regions> values{};
    std::size_t                                       count = 0;
};

// 保存体散射点、到射线起点的距离、相函数参数与介质索引。
struct MediumInteraction {
    glm::dvec3  position{0};
    double      distance   = 0;
    double      anisotropy = 0;
    std::size_t region     = 0;
};

// 组合入射段的传输权重与可选散射点，空交互表示光线穿过该段。
struct MediumSample {
    glm::dvec3                       weight{1};
    std::optional<MediumInteraction> interaction;
};

// 构造时复制并验证区域。渲染查询只读，区段裁剪和权重组合可由多线程共享。
// 纯吸收区段使用解析 Tr，零密度不消耗随机数，保留无介质渲染的原随机序列。
class MediumField final {
  public:
    explicit MediumField(std::span<const MediumRegion> regions      = {},
                         CancellationToken             cancellation = {});
    MediumSegments segments(const Ray& ray, RayInterval interval,
                            CancellationToken cancellation = {}) const;
    glm::dvec3 transmittance(const Ray& ray, RayInterval interval,
                             CancellationToken cancellation = {}) const;
    MediumSample sample(const Ray& ray, RayInterval interval, Sampler& sampler,
                        CancellationToken cancellation = {}) const;

  private:
    std::vector<MediumRegion> regions_;
};

} // namespace minirender
