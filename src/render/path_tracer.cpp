#include <minirender/geometry/uv_tangents.hpp>
#include <minirender/render/path_tracer.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender {
namespace {

bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
double maximum(glm::dvec3 value) {
    return std::max({value.x, value.y, value.z});
}
void check_radiance(glm::dvec3 value) {
    if (!finite(value) || glm::any(glm::lessThan(value, glm::dvec3(0)))) {
        throw std::runtime_error("Path contribution is nonfinite or negative");
    }
}

} // namespace

std::string sampling_mode_name(SamplingMode mode) {
    switch (mode) {
    case SamplingMode::bsdf:
        return "bsdf";
    case SamplingMode::light:
        return "light";
    case SamplingMode::mis:
        return "mis";
    }
    throw std::invalid_argument("Unknown sampling mode");
}
SamplingMode parse_sampling_mode(const std::string& name) {
    if (name == "bsdf") {
        return SamplingMode::bsdf;
    }
    if (name == "light") {
        return SamplingMode::light;
    }
    if (name == "mis") {
        return SamplingMode::mis;
    }
    throw std::invalid_argument("Sampling mode must be bsdf, light or mis");
}
void IntegratorSettings::validate() const {
    sampling_mode_name(mode);
    if (kind != IntegratorKind::path_tracing && kind != IntegratorKind::sppm) {
        throw std::invalid_argument("Unknown integrator kind");
    }
    sppm.validate();
    if (maximum_depth > 128 || roulette_depth > 128) {
        throw std::invalid_argument("Path and roulette depths must not exceed 128");
    }
}
void RenderSettings::validate() const {
    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        width * height > 64U * 1024U * 1024U || samples_per_pixel == 0) {
        throw std::invalid_argument("Render dimensions or sample count exceed the image budget");
    }
}
double power_heuristic(double sampled_pdf, double other_pdf) {
    if (!std::isfinite(sampled_pdf) || !std::isfinite(other_pdf) || sampled_pdf < 0 ||
        other_pdf < 0) {
        throw std::invalid_argument("MIS probabilities must be finite and nonnegative");
    }
    const double scale = std::max(sampled_pdf, other_pdf);
    if (scale == 0) {
        return 0;
    }
    const double a = sampled_pdf / scale, b = other_pdf / scale;
    return a * a / (a * a + b * b);
}
glm::dvec3 offset_origin(const SurfaceHit& hit, glm::dvec3 direction) {
    auto normal = glm::normalize(glm::dvec3(hit.geometric_normal));
    if (glm::dot(normal, direction) < 0) {
        normal = -normal;
    }
    // 零坐标平面的误差可恰好为零。这个数值下界远小于 float 几何的最小间隔，
    // 同时避免后续边函数乘法把非正规数偏移再次舍入为零。
    const double numeric_floor =
        static_cast<double>(std::numeric_limits<float>::min()) * std::numeric_limits<float>::min();
    const double distance = std::max(glm::dot(glm::abs(normal), hit.position_error), numeric_floor);
    auto origin = hit.position + distance * normal;
    for (int axis = 0; axis < 3; ++axis) {
        if (normal[axis] != 0) {
            origin[axis] = std::nextafter(
                origin[axis], std::copysign(std::numeric_limits<double>::infinity(), normal[axis]));
        }
    }
    return origin;
}
Ray spawn_ray(const SurfaceHit& hit, glm::dvec3 direction) {
    return {offset_origin(hit, direction), glm::normalize(direction)};
}

PathTracer::PathTracer(std::shared_ptr<const SceneSnapshot> scene, IntegratorSettings settings,
                       CancellationToken cancellation)
    : scene_(std::move(scene)), bvh_(scene_, cancellation), lights_(scene_, cancellation),
      media_(scene_->media, cancellation), settings_(settings),
      cancellation_(std::move(cancellation)) {
    settings_.validate();
    if (settings_.kind != IntegratorKind::path_tracing) {
        throw std::invalid_argument("PathTracer requires the path tracing backend");
    }
}
const SceneSnapshot& PathTracer::scene() const noexcept {
    return *scene_;
}
glm::dvec3 PathTracer::shadow_transmittance(Ray ray, const LightSample& light,
                                            PathStatistics* statistics) const {
    if (statistics) {
        ++statistics->shadow_rays;
    }
    double limit = std::numeric_limits<double>::infinity();
    if (std::isfinite(light.distance)) {
        SurfaceHit endpoint;
        endpoint.position = light.position;
        endpoint.geometric_normal = glm::vec3(light.normal);
        endpoint.position_error = light.position_error;
        const auto target = offset_origin(endpoint, -light.direction);
        const auto difference = target - ray.origin;
        limit = glm::length(difference);
        if (limit == 0) {
            return glm::dvec3(0);
        }
        ray.direction = difference / limit;
        limit = std::nextafter(limit, 0.0);
    }
    if (bvh_.occluded(ray, {0, limit}, statistics ? &statistics->geometry : nullptr,
                      cancellation_)) {
        return glm::dvec3(0);
    }
    return media_.transmittance(ray, {0, limit}, cancellation_);
}
glm::dvec3 PathTracer::trace(Ray ray, Sampler& sampler, RayInterval primary_interval,
                             PathStatistics* statistics) const {
    if (statistics) {
        ++statistics->paths;
    }
    glm::dvec3 result(0), beta(1);
    glm::dvec3 previous_reference(0);
    double previous_pdf = 0, eta_scale = 1;
    bool previous_delta = true;
    for (std::uint32_t depth = 0;; ++depth) {
        cancellation_.check();
        const auto interval =
            depth == 0 ? primary_interval : RayInterval{0, std::numeric_limits<double>::infinity()};
        const auto hit = bvh_.intersect(ray, interval, statistics ? &statistics->geometry : nullptr,
                                        cancellation_);
        const auto medium =
            media_.sample(ray, {interval.minimum, hit ? hit->distance : interval.maximum}, sampler,
                          cancellation_);
        beta *= medium.weight;
        check_radiance(beta);
        if (maximum(beta) == 0) {
            break;
        }
        if (medium.interaction) {
            if (depth == settings_.maximum_depth) {
                break;
            }
            const auto& vertex = *medium.interaction;
            const auto wo = -ray.direction;
            const double light_u = sampler.next_1d();
            const auto light_surface_u = sampler.next_2d();
            const auto phase_u = sampler.next_2d();
            const auto light = lights_.sample(vertex.position, light_u, light_surface_u);
            if (light && (settings_.mode != SamplingMode::bsdf ||
                          light->measure == SampleMeasure::discrete)) {
                const double phase = henyey_greenstein(wo, light->direction, vertex.anisotropy);
                const auto transmission =
                    shadow_transmittance({vertex.position, light->direction}, *light, statistics);
                double weight = 1;
                if (settings_.mode == SamplingMode::mis &&
                    light->measure == SampleMeasure::solid_angle) {
                    weight = power_heuristic(light->probability, phase);
                }
                result +=
                    beta * transmission * light->radiance * (phase * weight / light->probability);
            }
            const auto phase = sample_henyey_greenstein(wo, vertex.anisotropy, phase_u);
            // HG 的采样 PDF 与相函数值相同，延续权重为 1；体积事件没有表面余弦。
            previous_reference = vertex.position;
            previous_pdf = phase.probability;
            previous_delta = false;
            ray = {vertex.position, phase.direction};
            if (statistics) {
                ++statistics->scattering_events;
                ++statistics->medium_scattering_events;
            }
            const double roulette_u = sampler.next_1d();
            if (settings_.russian_roulette && depth + 1 >= settings_.roulette_depth) {
                const double survival = std::clamp(maximum(beta * eta_scale), 0.05, 0.95);
                if (roulette_u >= survival) {
                    if (statistics) {
                        ++statistics->roulette_terminations;
                    }
                    break;
                }
                beta /= survival;
            }
            continue;
        }
        const auto emission_weight = [&](double light_pdf) {
            if (depth == 0 || previous_delta || settings_.mode == SamplingMode::bsdf) {
                return 1.0;
            }
            return settings_.mode == SamplingMode::mis ? power_heuristic(previous_pdf, light_pdf)
                                                       : 0.0;
        };
        if (!hit) {
            result += beta * lights_.environment().evaluate(ray.direction) *
                      emission_weight(lights_.environment_pdf(ray.direction));
            break;
        }
        const auto& instance = scene_->instances[hit->instance_index];
        result += beta * glm::dvec3(instance.material.emission) *
                  emission_weight(lights_.area_pdf(previous_reference, *hit));
        if (depth == settings_.maximum_depth) {
            break;
        }
        auto normal =
            glm::dvec3(instance.material.kind == MaterialKind::dielectric ? hit->geometric_normal
                                                                          : hit->shading_normal);
        const auto wo_world = -ray.direction;
        if (glm::dot(normal, wo_world) < 0) {
            normal = -normal;
        }
        const ShadingFrame frame(normal);
        const auto wo = frame.to_local(wo_world);
        const auto texture_color =
            instance.base_color_texture
                ? instance.base_color_texture->sample(hit->uv, 0, instance.material.texture_filter)
                : glm::vec3(1);
        ClothAxes cloth_axes;
        if (instance.material.kind == MaterialKind::cloth) {
            const auto& face = instance.mesh->triangles[hit->triangle_index];
            std::array<glm::vec3, 3> positions;
            std::array<glm::vec2, 3> coordinates;
            for (std::size_t i = 0; i < 3; ++i) {
                positions[i] = instance.mesh->positions[face.corners[i].position];
                coordinates[i] = face.corners[i].uv;
            }
            const auto tangents = uv_tangents(positions, coordinates);
            if (!tangents) {
                throw std::logic_error("Cloth snapshot has invalid UV coordinates");
            }
            const glm::dmat3 linear(instance.object_to_world);
            cloth_axes = {frame.to_local(linear * tangents->u),
                          frame.to_local(linear * tangents->v)};
        }
        const Bsdf bsdf(instance.material, hit->front_face, texture_color, cloth_axes);

        // 每种模式按相同次序分配随机维度，切换估计器不改变后续 BSDF 的随机数。
        const double light_u = sampler.next_1d();
        const auto surface_u = sampler.next_2d();
        const double component = sampler.next_1d();
        const auto direction_u = sampler.next_2d();
        if (!bsdf.is_delta()) {
            const auto light = lights_.sample(hit->position, light_u, surface_u);
            if (light && (settings_.mode != SamplingMode::bsdf ||
                          light->measure == SampleMeasure::discrete)) {
                const auto wi = frame.to_local(light->direction);
                const bool reflection_side =
                    glm::dot(wo_world, glm::dvec3(hit->geometric_normal)) *
                        glm::dot(light->direction, glm::dvec3(hit->geometric_normal)) >
                    0;
                if (wi.z > 0 && reflection_side) {
                    const auto transmission =
                        shadow_transmittance(spawn_ray(*hit, light->direction), *light, statistics);
                    if (maximum(transmission) > 0) {
                        double weight = 1;
                        if (settings_.mode == SamplingMode::mis &&
                            light->measure == SampleMeasure::solid_angle) {
                            weight = power_heuristic(light->probability, bsdf.pdf(wo, wi));
                        }
                        result += beta * transmission * bsdf.evaluate(wo, wi) * wi.z *
                                  light->radiance * (weight / light->probability);
                    }
                }
            }
        }
        const auto sample = bsdf.sample(wo, component, direction_u);
        if (!sample || maximum(sample->weight) == 0) {
            break;
        }
        const auto wi_world = glm::normalize(frame.to_world(sample->direction));
        const double sides = glm::dot(wo_world, glm::dvec3(hit->geometric_normal)) *
                             glm::dot(wi_world, glm::dvec3(hit->geometric_normal));
        const bool transmission = sample->event == ScatteringEvent::specular_transmission;
        if ((transmission && sides >= 0) || (!transmission && sides <= 0)) {
            break;
        }
        beta *= sample->weight;
        check_radiance(beta);
        previous_reference = hit->position;
        previous_pdf = sample->probability;
        previous_delta = sample->measure == SampleMeasure::discrete;
        eta_scale *= sample->eta * sample->eta;
        if (statistics) {
            ++statistics->scattering_events;
        }
        ray = spawn_ray(*hit, wi_world);
        const double roulette_u = sampler.next_1d();
        if (settings_.russian_roulette && depth + 1 >= settings_.roulette_depth) {
            const double survival = std::clamp(maximum(beta * eta_scale), 0.05, 0.95);
            if (roulette_u >= survival) {
                if (statistics) {
                    ++statistics->roulette_terminations;
                }
                break;
            }
            beta /= survival;
        }
    }
    check_radiance(result);
    return result;
}
glm::dvec3 PathTracer::sample_pixel(std::size_t x, std::size_t y, std::uint64_t sample_index,
                                    const RenderSettings& settings,
                                    PathStatistics* statistics) const {
    settings.validate();
    if (x >= settings.width || y >= settings.height) {
        throw std::out_of_range("Pixel coordinates are outside the render image");
    }
    auto sampler = Sampler::for_sample(settings.seed, y * settings.width + x, sample_index);
    const auto film = sampler.next_2d();
    const auto lens = sampler.next_2d();
    const glm::dvec2 uv{(static_cast<double>(x) + film.x) / static_cast<double>(settings.width),
                        (static_cast<double>(y) + film.y) / static_cast<double>(settings.height)};
    const auto& camera = scene_->camera;
    const auto ray = camera.generate_lens_ray(
        uv, static_cast<double>(settings.width) / static_cast<double>(settings.height), lens);
    const auto forward = glm::normalize(glm::dvec3(camera.target) - glm::dvec3(camera.position));
    const double cosine = glm::dot(ray.direction, forward);
    return trace(ray, sampler, {camera.near_plane / cosine, camera.far_plane / cosine}, statistics);
}
Image PathTracer::render(const RenderSettings& settings, PathStatistics* statistics) const {
    settings.validate();
    Image result(settings.width, settings.height);
    for (std::size_t y = 0; y < settings.height; ++y) {
        for (std::size_t x = 0; x < settings.width; ++x) {
            glm::dvec3 sum(0);
            for (std::uint32_t sample = 0; sample < settings.samples_per_pixel; ++sample) {
                sum += sample_pixel(x, y, sample, settings, statistics);
            }
            const auto value = sum / static_cast<double>(settings.samples_per_pixel);
            if (maximum(value) > std::numeric_limits<float>::max()) {
                throw std::overflow_error("Rendered radiance exceeds the float image range");
            }
            result.at(x, y) = glm::vec3(value);
        }
    }
    return result;
}
Image display_image(const Image& linear, double exposure) {
    if (!std::isfinite(exposure) || std::abs(exposure) > 64) {
        throw std::invalid_argument("Display exposure must be finite and in [-64,64]");
    }
    Image result(linear.width(), linear.height());
    const double scale = std::exp2(exposure);
    for (std::size_t y = 0; y < linear.height(); ++y) {
        for (std::size_t x = 0; x < linear.width(); ++x) {
            const auto value = glm::dvec3(linear.at(x, y));
            if (!finite(value)) {
                throw std::invalid_argument("Display input contains nonfinite radiance");
            }
            const auto positive = glm::max(value, glm::dvec3(0)) * scale;
            result.at(x, y) = glm::vec3(positive / (1.0 + positive));
        }
    }
    return result;
}

} // namespace minirender
