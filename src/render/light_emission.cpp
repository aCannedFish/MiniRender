#include <minirender/render/light.hpp>
#include <minirender/render/path_tracer.hpp>

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {
void check(glm::dvec2 u) {
    if (!(u.x >= 0 && u.x < 1 && u.y >= 0 && u.y < 1)) {
        throw std::invalid_argument("Emission samples must be in [0,1)");
    }
}
} // namespace

std::optional<PhotonEmission> LightSampler::sample_emission(double light_u, glm::dvec2 position_u,
                                                            glm::dvec2 direction_u) const {
    check(position_u);
    check(direction_u);
    const auto selected = selection_.sample(light_u);
    if (!selected) {
        return std::nullopt;
    }
    const auto& choice = choices_[selected->index];
    const double pi = std::numbers::pi;
    if (choice.kind == LightKind::area) {
        const auto& light = areas_[choice.index];
        const auto barycentric = sample_triangle(position_u);
        SurfaceHit endpoint;
        endpoint.position = barycentric.x * light.vertices[0] + barycentric.y * light.vertices[1] +
                            barycentric.z * light.vertices[2];
        endpoint.geometric_normal = glm::vec3(light.normal);
        endpoint.position_error = (glm::abs(barycentric.x * light.vertices[0]) +
                                   glm::abs(barycentric.y * light.vertices[1]) +
                                   glm::abs(barycentric.z * light.vertices[2])) *
                                  (7 * std::numeric_limits<double>::epsilon());
        double side = 1;
        if (light.double_sided) {
            side = direction_u.x < .5 ? 1 : -1;
            direction_u.x = direction_u.x < .5 ? 2 * direction_u.x : 2 * direction_u.x - 1;
        }
        const auto local = sample_cosine_hemisphere(direction_u);
        if (local.z <= 0) {
            return std::nullopt;
        }
        const auto direction = glm::normalize(ShadingFrame(side * light.normal).to_world(local));
        const double hemispheres = light.double_sided ? 2 : 1;
        // Le*cos/(pA*pDirection*pLight)，约去余弦，掠射处也保留有限功率。
        return PhotonEmission{spawn_ray(endpoint, direction),
                              light.radiance *
                                  (hemispheres * pi * light.area / selected->probability),
                              choice.kind,
                              selected->probability,
                              1 / light.area,
                              local.z / (hemispheres * pi)};
    }
    glm::dvec3 outward, radiance;
    double direction_pdf;
    if (choice.kind == LightKind::directional) {
        const auto& light = directional_[choice.index];
        outward = light.direction;
        radiance = light.radiance;
        direction_pdf = 1;
    } else {
        const auto sample = environment_.sample(direction_u);
        outward = sample.direction;
        radiance = sample.radiance;
        direction_pdf = sample.probability;
    }
    const double area = pi * emission_radius_ * emission_radius_;
    const auto disk = sample_disk(position_u) * emission_radius_;
    const auto origin = emission_center_ + 2 * emission_radius_ * outward +
                        ShadingFrame(outward).to_world({disk.x, disk.y, 0});
    return PhotonEmission{
        {origin, -outward}, radiance * (area / (selected->probability * direction_pdf)),
        choice.kind,        selected->probability,
        1 / area,           direction_pdf};
}
} // namespace minirender
