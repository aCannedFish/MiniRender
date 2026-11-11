#include <minirender/render/sdf_render.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender {
namespace {

// 光线起点沿朝向观察者的法线偏移，避开主射线容差范围中的自相交。
float visibility(const SdfField& field, const SdfDocument& document, const SdfTraceResult& hit,
                 glm::dvec3 normal, CancellationToken cancellation) {
    const double epsilon = document.trace.epsilon + document.trace.relative_epsilon * hit.distance;
    const Ray shadow{hit.position + normal * (4 * epsilon),
                     -glm::normalize(glm::dvec3(document.light.direction))};
    auto settings = document.trace;
    settings.maximum_steps = settings.shadow_steps;
    const auto result =
        trace_sdf(field, shadow, settings, 2 * epsilon, settings.maximum_distance, cancellation);
    return result.state == SdfTraceState::miss ? 1.0F : 0.0F;
}

} // namespace

glm::vec3 sdf_step_color(std::uint32_t steps, std::uint32_t budget) {
    const float t = std::clamp(static_cast<float>(steps) / static_cast<float>(std::max(1U, budget)),
                               0.0F, 1.0F);
    return glm::clamp(glm::vec3(1.5F) - glm::abs(4.0F * t - glm::vec3(3, 2, 1)), glm::vec3(0),
                      glm::vec3(1));
}

SdfRenderResult render_sdf(const SdfDocument& document, SdfRenderOptions options,
                           CancellationToken cancellation, SdfProgress progress) {
    if (options.width == 0 || options.height == 0 || options.width > 2048 ||
        options.height > 2048 ||
        (options.view != SdfView::surface && options.view != SdfView::normals &&
         options.view != SdfView::depth && options.view != SdfView::steps)) {
        throw std::invalid_argument("Invalid SDF image dimensions or view");
    }
    const SdfField field(document, cancellation);
    SdfRenderResult result{Image(options.width, options.height)};
    const auto forward =
        glm::normalize(glm::dvec3(document.camera.target) - glm::dvec3(document.camera.position));
    const auto toward_light = -glm::normalize(glm::dvec3(document.light.direction));
    const double aspect = static_cast<double>(options.width) / static_cast<double>(options.height);
    const auto count = options.width * options.height;
    if (progress) {
        progress(0, count);
    }
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        cancellation.check();
        const auto x = pixel % options.width;
        const auto y = pixel / options.width;
        const auto ray = document.camera.generate_ray(
            {(static_cast<double>(x) + .5) / static_cast<double>(options.width),
             (static_cast<double>(y) + .5) / static_cast<double>(options.height)},
            aspect);
        const auto hit = trace_sdf(field, ray, document.trace, document.camera.near_plane,
                                   document.camera.far_plane, cancellation);
        result.steps += hit.steps;
        result.hits += hit.state == SdfTraceState::hit ? 1 : 0;
        auto color =
            options.view == SdfView::surface ? glm::vec3(.015F, .022F, .035F) : glm::vec3(0);
        if (hit.state == SdfTraceState::step_limit) {
            color = {1, 0, 1};
            ++result.limited;
        } else if (options.view == SdfView::steps) {
            color = sdf_step_color(hit.steps, document.trace.maximum_steps);
        } else if (hit.state == SdfTraceState::hit) {
            if (options.view == SdfView::depth) {
                // 与三角形预览保持相同的线性观察深度显示约定。
                const double depth = hit.distance * glm::dot(ray.direction, forward);
                color = glm::vec3(static_cast<float>(1 / (1 + depth)));
            } else {
                glm::dvec3 normal;
                try {
                    normal = field.normal(hit.position, document.trace.normal_step);
                } catch (const std::runtime_error&) {
                    normal = -ray.direction;
                    ++result.normal_fallbacks;
                }
                if (glm::dot(normal, ray.direction) > 0) {
                    normal = -normal;
                }
                if (options.view == SdfView::normals) {
                    color = glm::vec3(normal) * .5F + .5F;
                } else {
                    const float cosine =
                        static_cast<float>(std::max(0.0, glm::dot(normal, toward_light)));
                    const float visible =
                        options.shadows && cosine > 0
                            ? visibility(field, document, hit, normal, cancellation)
                            : 1.0F;
                    color = field.nodes()[hit.material].color *
                            (document.ambient + document.light.color * document.light.intensity *
                                                    (cosine * visible / glm::pi<float>()));
                }
            }
        }
        result.image.at(x, y) = color;
        if (progress && ((pixel + 1) % 256 == 0 || pixel + 1 == count)) {
            progress(pixel + 1, count);
        }
    }
    return result;
}

} // namespace minirender
