#pragma once

#include <minirender/render/path_tracer.hpp>

namespace minirender::detail {

// SPPM 两种传输方向共用的表面状态。eval 保持现有 PT 的着色法线测度，
// gather 和 importance 通过几何余弦换算为光子携带的面积功率。
class SppmSurface final {
  public:
    SppmSurface(const SceneSnapshot& scene, const SurfaceHit& hit, glm::dvec3 wo_world);
    glm::dvec3 response(glm::dvec3 wi_world) const;
    glm::dvec3 gather(glm::dvec3 wi_world) const;
    double pdf(glm::dvec3 wi_world) const;
    std::optional<BsdfSample> sample(double component, glm::dvec2 u, TransportMode mode) const;
    bool delta() const noexcept;
    bool glossy() const noexcept;
    SurfaceHit hit;
    glm::dvec3 wo_world;

  private:
    static Bsdf make_bsdf(const SceneSnapshot& scene, const SurfaceHit& hit,
                          const ShadingFrame& frame);
    ShadingFrame frame_;
    glm::dvec3 wo_;
    Bsdf bsdf_;
    MaterialKind kind_;
};

} // namespace minirender::detail
