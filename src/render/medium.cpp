#include <minirender/render/bsdf.hpp>
#include <minirender/render/medium.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {
constexpr double infinity = std::numeric_limits<double>::infinity();

void check_g(double g) {
    if (!std::isfinite(g) || std::abs(g) >= 1) {
        throw std::invalid_argument("HG requires finite |g| < 1");
    }
}
void check_direction(glm::dvec3 value) {
    const double squared = glm::dot(value, value);
    if (!std::isfinite(squared) || std::abs(squared - 1) > 1e-6) {
        throw std::invalid_argument("Phase directions must have unit length");
    }
}
void check_sample(double u) {
    if (!std::isfinite(u) || u < 0 || u >= 1) {
        throw std::invalid_argument("Medium random sample must be in [0,1)");
    }
}
void check_length(double length) {
    if (std::isnan(length) || length < 0) {
        throw std::invalid_argument("Medium distance must be nonnegative");
    }
}
double phase_value(double cosine, double g) {
    // (1-|g|)^2 + 2|g|(1+sign(g)*cosine) 保留高度前向峰附近的有效位。
    const double a = std::abs(g);
    const double denominator = (1 - a) * (1 - a) + 2 * a * (1 + std::copysign(1.0, g) * cosine);
    return ((1 - g) * (1 + g)) / (4 * std::numbers::pi * denominator * std::sqrt(denominator));
}
double optical_depth(double extinction, double distance) {
    return extinction == 0 ? 0 : extinction * distance;
}

// 混合概率在 log 空间求和。长距离/稀薄系数下 PDF 可能下溢，权重比仍有限。
glm::dvec3 mixture_weight(const HomogeneousMedium& medium, double distance, bool scattered) {
    const auto extinction = medium.extinction();
    glm::dvec3 log_density(-infinity), log_numerator(-infinity);
    for (int c = 0; c < 3; ++c) {
        const double attenuation = -optical_depth(extinction[c], distance);
        if (!scattered) {
            log_density[c] = attenuation;
            log_numerator[c] = attenuation;
        } else {
            if (extinction[c] > 0) {
                log_density[c] = std::log(extinction[c]) + attenuation;
            }
            if (medium.scattering[c] > 0) {
                log_numerator[c] = std::log(medium.scattering[c]) + attenuation;
            }
        }
    }
    const double largest = std::max({log_density.x, log_density.y, log_density.z});
    if (!std::isfinite(largest)) {
        // 精确采样无法选择零质量事件；拒绝不可表示的输入区间，避免发布 NaN。
        throw std::runtime_error("Medium sample has zero mixture probability");
    }
    double sum = 0;
    for (int c = 0; c < 3; ++c) {
        sum += std::exp(log_density[c] - largest);
    }
    const double log_probability = largest + std::log(sum / 3);
    glm::dvec3 result;
    for (int c = 0; c < 3; ++c) {
        result[c] = std::exp(log_numerator[c] - log_probability);
    }
    return result;
}
} // namespace

double henyey_greenstein(glm::dvec3 wo, glm::dvec3 wi, double g) {
    check_g(g);
    check_direction(wo);
    check_direction(wi);
    return phase_value(std::clamp(glm::dot(wo, wi), -1.0, 1.0), g);
}
PhaseSample sample_henyey_greenstein(glm::dvec3 wo, double g, glm::dvec2 u) {
    check_g(g);
    check_direction(wo);
    check_sample(u.x);
    check_sample(u.y);
    // c 是沿旧传播方向 -wo 的余弦。以下两式由 HG 逆 CDF 代数化简，
    // 不含 1/g；按 g 的符号选择靠近主要峰值的一端，避免 g≈0 时相减。
    double c;
    if (g >= 0) {
        const double denominator = (1 - g) + 2 * g * u.x;
        c = 1 - 2 * (1 - g) * (1 - g) * (1 - u.x) * (1 + g * u.x) / (denominator * denominator);
    } else {
        const double denominator = (1 + g) - 2 * g * (1 - u.x);
        c = -1 + 2 * (1 + g) * (1 + g) * u.x * (1 - g * (1 - u.x)) / (denominator * denominator);
    }
    c = std::clamp(c, -1.0, 1.0);
    const double sine = std::sqrt(std::max(0.0, (1 - c) * (1 + c)));
    const double phi = 2 * std::numbers::pi * u.y;
    const auto direction =
        glm::normalize(ShadingFrame(-wo).to_world({sine * std::cos(phi), sine * std::sin(phi), c}));
    return {direction, henyey_greenstein(wo, direction, g)};
}

glm::dvec3 medium_transmittance(const HomogeneousMedium& medium, double distance) {
    medium.validate();
    check_length(distance);
    const auto extinction = medium.extinction();
    glm::dvec3 result;
    for (int c = 0; c < 3; ++c) {
        result[c] = std::exp(-optical_depth(extinction[c], distance));
    }
    return result;
}
MediumDistanceSample sample_medium_distance(const HomogeneousMedium& medium, double length,
                                            double channel_u, double distance_u) {
    medium.validate();
    check_length(length);
    check_sample(channel_u);
    check_sample(distance_u);
    const auto extinction = medium.extinction();
    const auto channel = std::min(static_cast<int>(channel_u * 3), 2);
    const double distance =
        extinction[channel] == 0 ? infinity : -std::log1p(-distance_u) / extinction[channel];
    const bool scattered = distance < length;
    const double actual = scattered ? distance : length;
    return {scattered, actual, mixture_weight(medium, actual, scattered)};
}

MediumField::MediumField(std::span<const MediumRegion> regions, CancellationToken cancellation) {
    validate_medium_regions(regions, cancellation);
    regions_.assign(regions.begin(), regions.end());
}
MediumSegments MediumField::segments(const Ray& ray, RayInterval interval,
                                     CancellationToken cancellation) const {
    cancellation.check();
    interval.validate();
    static_cast<void>(RayQuery(ray));
    MediumSegments result;
    for (std::size_t index = 0; index < regions_.size(); ++index) {
        cancellation.check();
        const auto& region = regions_[index];
        if (!region.enabled) {
            continue;
        }
        double enter = interval.minimum, leave = interval.maximum;
        for (int axis = 0; axis < 3; ++axis) {
            if (ray.direction[axis] == 0) {
                // 平行且落在盒边界的射线按外侧处理，避免相邻盒在共享面上重叠。
                if (ray.origin[axis] <= region.minimum[axis] ||
                    ray.origin[axis] >= region.maximum[axis]) {
                    leave = enter;
                    break;
                }
                continue;
            }
            double near = (region.minimum[axis] - ray.origin[axis]) / ray.direction[axis];
            double far = (region.maximum[axis] - ray.origin[axis]) / ray.direction[axis];
            if (near > far) {
                std::swap(near, far);
            }
            enter = std::max(enter, near);
            leave = std::min(leave, far);
        }
        if (leave > enter) {
            result.values[result.count++] = {index, {enter, leave}};
        }
    }
    std::sort(result.values.begin(), result.values.begin() + std::ptrdiff_t(result.count),
              [](const auto& a, const auto& b) { return a.interval.minimum < b.interval.minimum; });
    return result;
}
glm::dvec3 MediumField::transmittance(const Ray& ray, RayInterval interval,
                                      CancellationToken cancellation) const {
    const auto parts = segments(ray, interval, cancellation);
    glm::dvec3 result(1);
    for (std::size_t i = 0; i < parts.count; ++i) {
        const auto& part = parts.values[i];
        result *= medium_transmittance(regions_[part.region].medium,
                                       part.interval.maximum - part.interval.minimum);
    }
    return result;
}
MediumSample MediumField::sample(const Ray& ray, RayInterval interval, Sampler& sampler,
                                 CancellationToken cancellation) const {
    const auto parts = segments(ray, interval, cancellation);
    MediumSample result;
    for (std::size_t i = 0; i < parts.count; ++i) {
        cancellation.check();
        const auto& part = parts.values[i];
        const auto& medium = regions_[part.region].medium;
        const double length = part.interval.maximum - part.interval.minimum;
        if (medium.scattering == glm::dvec3(0)) {
            result.weight *= medium_transmittance(medium, length);
            continue;
        }
        const double channel = sampler.next_1d();
        const double distance = sampler.next_1d();
        const auto sampled = sample_medium_distance(medium, length, channel, distance);
        result.weight *= sampled.weight;
        if (sampled.scattered) {
            const double t = part.interval.minimum + sampled.distance;
            result.interaction = MediumInteraction{ray.origin + t * ray.direction, t,
                                                   medium.anisotropy, part.region};
            break;
        }
    }
    return result;
}
} // namespace minirender
