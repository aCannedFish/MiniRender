#include "sppm_surface.hpp"

#include <minirender/geometry/uv_tangents.hpp>

#include <cmath>
#include <stdexcept>

namespace minirender::detail {
namespace {
glm::dvec3 normal(const SceneSnapshot& scene, const SurfaceHit& hit, glm::dvec3 wo) {
    auto n =
        glm::dvec3(scene.instances[hit.instance_index].material.kind == MaterialKind::dielectric
                       ? hit.geometric_normal
                       : hit.shading_normal);
    return glm::dot(n, wo) < 0 ? -n : n;
}
} // namespace

Bsdf SppmSurface::make_bsdf(const SceneSnapshot& scene, const SurfaceHit& hit,
                            const ShadingFrame& frame) {
    const auto& instance = scene.instances[hit.instance_index];
    const auto color =
        instance.base_color_texture
            ? instance.base_color_texture->sample(hit.uv, 0, instance.material.texture_filter)
            : glm::vec3(1);
    ClothAxes axes;
    if (instance.material.kind == MaterialKind::cloth) {
        const auto& face = instance.mesh->triangles[hit.triangle_index];
        std::array<glm::vec3, 3> positions;
        std::array<glm::vec2, 3> uv;
        for (std::size_t i = 0; i < 3; ++i) {
            positions[i] = instance.mesh->positions[face.corners[i].position];
            uv[i] = face.corners[i].uv;
        }
        const auto tangents = uv_tangents(positions, uv);
        if (!tangents) {
            throw std::logic_error("SPPM cloth surface has invalid UV coordinates");
        }
        const glm::dmat3 transform(instance.object_to_world);
        axes = {frame.to_local(transform * tangents->u), frame.to_local(transform * tangents->v)};
    }
    return Bsdf(instance.material, hit.front_face, color, axes);
}
SppmSurface::SppmSurface(const SceneSnapshot& scene, const SurfaceHit& value, glm::dvec3 outgoing)
    : hit(value), wo_world(outgoing), frame_(normal(scene, value, outgoing)),
      wo_(frame_.to_local(outgoing)), bsdf_(make_bsdf(scene, value, frame_)),
      kind_(scene.instances[value.instance_index].material.kind) {}
glm::dvec3 SppmSurface::response(glm::dvec3 wi_world) const {
    if (glm::dot(wo_world, glm::dvec3(hit.geometric_normal)) *
            glm::dot(wi_world, glm::dvec3(hit.geometric_normal)) <=
        0) {
        return glm::dvec3(0);
    }
    const auto wi = frame_.to_local(wi_world);
    return wi.z > 0 ? bsdf_.evaluate(wo_, wi) * wi.z : glm::dvec3(0);
}
glm::dvec3 SppmSurface::gather(glm::dvec3 wi_world) const {
    const double cosine = std::abs(glm::dot(glm::dvec3(hit.geometric_normal), wi_world));
    return cosine > 0 ? response(wi_world) / cosine : glm::dvec3(0);
}
double SppmSurface::pdf(glm::dvec3 wi_world) const {
    return bsdf_.pdf(wo_, frame_.to_local(wi_world));
}
bool SppmSurface::delta() const noexcept {
    return bsdf_.is_delta();
}
bool SppmSurface::glossy() const noexcept {
    return kind_ == MaterialKind::conductor || kind_ == MaterialKind::cloth;
}
std::optional<BsdfSample> SppmSurface::sample(double component, glm::dvec2 u,
                                              TransportMode mode) const {
    auto value = bsdf_.sample(wo_, component, u, mode);
    if (!value) {
        return {};
    }
    const auto local = value->direction;
    const auto world = glm::normalize(frame_.to_world(local));
    const auto ng = glm::dvec3(hit.geometric_normal);
    const double a = glm::dot(ng, wo_world), b = glm::dot(ng, world);
    const bool transmission = value->event == ScatteringEvent::specular_transmission;
    if (transmission ? a * b >= 0 : a * b <= 0) {
        return {};
    }
    if (mode == TransportMode::importance) {
        const double denominator = a * local.z;
        if (denominator == 0) {
            return {};
        }
        value->weight *= std::abs(wo_.z * b / denominator);
    }
    value->direction = world;
    return value;
}
} // namespace minirender::detail
