#include <minirender/render/bsdf.hpp>
#include <minirender/render/dipole.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {
constexpr double pi = std::numbers::pi;

void check_radius(double radius) {
    if (!std::isfinite(radius) || radius < 0) {
        throw std::invalid_argument("Dipole radius must be finite and nonnegative");
    }
}

// 外侧余弦半球矩采用 64 点 Gauss-Legendre 积分。内部矩由折射互易关系得到，
// 避免直接跨越内部全反射临界角积分。eta=1 时严格退化为透射率 1。
double transmission_moment(double eta) {
    if (eta == 1) {
        return 1;
    }
    constexpr int count = 64;
    double sum = 0;
    for (int i = 0; i < count / 2; ++i) {
        double z = std::cos(pi * (i + .75) / (count + .5));
        double derivative = 0;
        for (int iteration = 0; iteration < 20; ++iteration) {
            double previous = 0, value = 1;
            for (int order = 1; order <= count; ++order) {
                const double next = ((2 * order - 1) * z * value - (order - 1) * previous) / order;
                previous = value;
                value = next;
            }
            derivative = count * (z * value - previous) / (z * z - 1);
            const double delta = value / derivative;
            z -= delta;
            if (std::abs(delta) < 2e-16) {
                break;
            }
        }
        const double weight = 1 / ((1 - z * z) * derivative * derivative);
        for (const double cosine : {(1 - z) / 2, (1 + z) / 2}) {
            sum += weight * 2 * cosine * (1 - fresnel_dielectric(cosine, 1, eta));
        }
    }
    return sum;
}
} // namespace

DipoleProfile::DipoleProfile(SubsurfaceParameters parameters, double ior) : ior_(ior) {
    parameters.validate();
    if (!std::isfinite(ior) || ior < 1 || ior > 3) {
        throw std::invalid_argument("Dipole IOR must be in 1..3");
    }
    external_transmission_ = transmission_moment(ior);
    const double escape = external_transmission_ / (ior * ior);
    internal_reflectance_ = 1 - escape;
    const double boundary = (2 - escape) / escape;
    for (int c = 0; c < 3; ++c) {
        const double absorption = parameters.absorption[c] * parameters.density;
        const double scattering = parameters.reduced_scattering[c] * parameters.density;
        const double extinction = absorption + scattering;
        auto& channel = channels_[c];
        channel.albedo = scattering / extinction;
        channel.real_depth = 1 / extinction;
        channel.virtual_depth = channel.real_depth * (1 + 4 * boundary / 3);
        channel.extinction = std::sqrt(3 * absorption * extinction);
        reflectance_[c] = channel.albedo / 2 *
                          (std::exp(-channel.extinction * channel.real_depth) +
                           std::exp(-channel.extinction * channel.virtual_depth));
    }
}

glm::dvec3 DipoleProfile::evaluate(double radius) const {
    check_radius(radius);
    glm::dvec3 value(0);
    for (int c = 0; c < 3; ++c) {
        const auto& channel = channels_[c];
        for (const double z : {channel.real_depth, channel.virtual_depth}) {
            const double d = std::hypot(radius, z);
            const double t = channel.extinction * d;
            if (std::isfinite(t)) {
                value[c] += channel.albedo / (4 * pi) * (z / d) / d / d * std::exp(-t) * (1 + t);
            }
        }
    }
    return value;
}

double DipoleProfile::integral(const Channel& channel, double radius) const {
    double value = 0;
    for (const double z : {channel.real_depth, channel.virtual_depth}) {
        const double d = std::hypot(radius, z);
        // d-z 在小半径时相消；用 r*(r/d)/(1+z/d) 计算同一个差值。
        const double difference = radius * (radius / d) / (1 + z / d);
        const double log_tail = -std::log1p(difference / z) - channel.extinction * difference;
        value += std::exp(-channel.extinction * z) * -std::expm1(log_tail);
    }
    return channel.albedo / 2 * value;
}

glm::dvec3 DipoleProfile::cumulative(double radius) const {
    check_radius(radius);
    return {integral(channels_[0], radius), integral(channels_[1], radius),
            integral(channels_[2], radius)};
}
glm::dvec3 DipoleProfile::reflectance() const noexcept {
    return reflectance_;
}
double DipoleProfile::sample_radius(int channel, double u, double maximum_radius) const {
    check_radius(maximum_radius);
    if (channel < 0 || channel > 2 || !(u >= 0 && u < 1) || maximum_radius == 0) {
        throw std::invalid_argument(
            "Dipole sampling requires a channel, u in [0,1) and radius > 0");
    }
    const auto& parameters = channels_[channel];
    const double total = integral(parameters, maximum_radius);
    if (total == 0) {
        throw std::invalid_argument("Dipole channel has no scattering mass");
    }
    if (u == 0) {
        return 0;
    }
    const double target = u * total;
    double low = 0, high = std::min(maximum_radius, parameters.real_depth);
    while (integral(parameters, high) < target && high < maximum_radius) {
        high = std::min(maximum_radius, high * 2);
    }
    for (int iteration = 0; iteration < 64; ++iteration) {
        const double middle = low + (high - low) / 2;
        if (integral(parameters, middle) < target) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low + (high - low) / 2;
}
double DipoleProfile::directional(double cosine) const {
    return (1 - fresnel_dielectric(cosine, 1, ior_)) / pi;
}
double DipoleProfile::external_transmission() const noexcept {
    return external_transmission_;
}
double DipoleProfile::internal_reflectance() const noexcept {
    return internal_reflectance_;
}
double DipoleProfile::ior() const noexcept {
    return ior_;
}

} // namespace minirender
