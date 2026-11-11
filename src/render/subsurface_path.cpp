#include <minirender/render/path_tracer.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender {

PathTracer::SubsurfaceStep PathTracer::scatter_subsurface(const SurfaceHit& hit, glm::dvec3 wo,
                                                          Sampler& sampler,
                                                          PathStatistics* statistics) const {
    if (!hit.front_face) {
        throw std::invalid_argument("Dipole SSS paths must arrive from outside the closed object");
    }
    const auto& profile = subsurface_.profile(hit.instance_index);
    const auto normal = glm::normalize(glm::dvec3(hit.geometric_normal));
    const double cosine = std::clamp(glm::dot(normal, wo), 0.0, 1.0);
    const double reflectance = fresnel_dielectric(cosine, 1, profile.ior());
    if (sampler.next_1d() < reflectance) {
        const auto reflected = glm::normalize(2 * cosine * normal - wo);
        return {{0, 0, 0}, {1, 1, 1}, hit.position, spawn_ray(hit, reflected), reflectance, true};
    }
    // 按 Ft(wo) 选择扩散分支，抵消 BSSRDF 的出射边界因子。两个端点均在外部，
    // 没有实际折射路径段，因而不再乘玻璃 BSDF 的 eta^2 换算。
    const auto axis_u = sampler.next_1d();
    const auto channel_u = sampler.next_1d();
    const auto disk_u = sampler.next_2d();
    const auto intersection_u = sampler.next_1d();
    const auto spatial =
        subsurface_.sample(hit, axis_u, channel_u, disk_u, intersection_u,
                           statistics ? &statistics->geometry : nullptr, cancellation_);
    if (!spatial) {
        return {};
    }
    if (!glm::any(glm::greaterThan(spatial->weight, glm::dvec3(0)))) {
        return {};
    }
    if (statistics) {
        ++statistics->subsurface_events;
        statistics->subsurface_distance += glm::length(spatial->surface.position - hit.position);
    }
    const ShadingFrame frame(glm::dvec3(spatial->surface.geometric_normal));
    const double light_u = sampler.next_1d();
    const auto light_surface_u = sampler.next_2d();
    const auto direction_u = sampler.next_2d();
    const auto light = lights_.sample(spatial->surface.position, light_u, light_surface_u);
    SubsurfaceStep step;
    if (light &&
        (settings_.mode != SamplingMode::bsdf || light->measure == SampleMeasure::discrete)) {
        const auto wi = frame.to_local(light->direction);
        if (wi.z > 0) {
            double weight = 1;
            if (settings_.mode == SamplingMode::mis &&
                light->measure == SampleMeasure::solid_angle) {
                weight = power_heuristic(light->probability, cosine_hemisphere_pdf(wi));
            }
            const auto transmission = shadow_transmittance(
                spawn_ray(spatial->surface, light->direction), *light, statistics);
            step.radiance = spatial->weight * transmission * light->radiance *
                            (profile.directional(std::clamp(wi.z, 0.0, 1.0)) * wi.z * weight /
                             light->probability);
        }
    }
    const auto direction = sample_cosine_hemisphere(direction_u);
    step.probability = cosine_hemisphere_pdf(direction);
    if (step.probability > 0) {
        step.weight = spatial->weight * (profile.directional(std::clamp(direction.z, 0.0, 1.0)) *
                                         direction.z / step.probability);
        step.ray = spawn_ray(spatial->surface, frame.to_world(direction));
        step.reference = spatial->surface.position;
    }
    return step;
}

} // namespace minirender
