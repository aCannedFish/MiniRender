#pragma once

#include <minirender/render/bvh.hpp>
#include <minirender/render/dipole.hpp>

#include <memory>
#include <vector>

namespace minirender {

struct SubsurfaceSample {
    SurfaceHit surface;
    glm::dvec3 weight;  // Rd(|pi-po|) / p_A(pi)，未包含外侧方向核。
    double probability; // 世界表面积密度，单位 m^-2。
};

// 只读 BSSRDF 表面采样器，生命周期短于传入的 SceneBvh，可供 PT worker 并发查询。
// 三轴投影和 RGB 通道混合，逐轴重算多命中数；采样与 pdf 使用相同面积测度。
class SubsurfaceSampler final {
  public:
    static constexpr std::size_t maximum_intersections = 4096;
    explicit SubsurfaceSampler(const SceneBvh& bvh, CancellationToken cancellation = {});
    ~SubsurfaceSampler();
    SubsurfaceSampler(const SubsurfaceSampler&) = delete;
    SubsurfaceSampler& operator=(const SubsurfaceSampler&) = delete;
    std::optional<SubsurfaceSample> sample(const SurfaceHit& from, double axis_u, double channel_u,
                                           glm::dvec2 disk_u, double intersection_u,
                                           TraceStatistics* statistics = nullptr,
                                           const CancellationToken& cancellation = {}) const;
    // from/to 必须是该 BVH 上的真实表面命中；不同实例之间密度为零。
    double pdf(const SurfaceHit& from, const SurfaceHit& to, TraceStatistics* statistics = nullptr,
               const CancellationToken& cancellation = {}) const;
    const DipoleProfile& profile(std::uint32_t instance) const;
    double maximum_radius(std::uint32_t instance) const;

  private:
    struct Instance;
    const Instance& instance(std::uint32_t index) const;
    std::vector<SurfaceHit> intersections(const SurfaceHit& from, glm::dvec3 offset,
                                          glm::dvec3 axis, double radius,
                                          TraceStatistics* statistics,
                                          const CancellationToken& cancellation) const;
    double mixture_pdf(const SurfaceHit& from, const SurfaceHit& to, int sampled_axis,
                       std::size_t sampled_count, TraceStatistics* statistics,
                       const CancellationToken& cancellation) const;
    const SceneBvh& bvh_;
    std::vector<std::unique_ptr<const Instance>> instances_;
};

} // namespace minirender
