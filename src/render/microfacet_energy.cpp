// 用白色粗糙导体积分方向反射能量，给离线 BSDF 与实时反射共用同一张表。
#include <algorithm>
#include <cmath>
#include <minirender/render/bsdf.hpp>
#include <minirender/render/ibl.hpp>
#include <minirender/render/microfacet_energy.hpp>
#include <numbers>
#include <stdexcept>

namespace minirender {
double integrate_directional_energy(double mu, double roughness, std::uint32_t samples,
                                    std::uint64_t seed, EnergyMethod method) {
    if (!std::isfinite(mu) || mu < 0 || mu > 1 || !std::isfinite(roughness) || roughness < 0 ||
        roughness > 1 || !samples) {
        throw std::invalid_argument("Invalid directional energy parameters");
    }
    if (method == EnergyMethod::split_sum) {
        auto ab = integrate_ibl_brdf(mu, roughness, samples, seed);
        return ab.x + ab.y;
    }
    if (roughness * roughness <= 1e-6) {
        return 1;
    }
    mu = std::max(1e-6, mu);
    const glm::dvec3 wo{std::sqrt(1 - mu * mu), 0, mu};
    Material         white{"white", "white", MaterialKind::conductor, glm::vec3(1)};
    white.roughness = static_cast<float>(roughness);
    const Bsdf bsdf(white);
    Sampler    sampler(seed);
    double     sum = 0;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const auto u = sampler.next_2d();
        if (method == EnergyMethod::visible_normal) {
            // GGX 的下半球反射是零贡献样本，仍计入分母。
            if (auto event = bsdf.sample(wo, 0, u)) {
                sum += event->weight.x;
            }
        } else {
            const double z = u.x, phi = 2 * std::numbers::pi * u.y;
            const double radius = std::sqrt(1 - z * z);
            sum += 2 * std::numbers::pi * z *
                   bsdf.evaluate(wo, {radius * std::cos(phi), radius * std::sin(phi), z}).x;
        }
    }
    return sum / samples;
}

void store_average_energy(Image& table, std::size_t row) {
    // E 在每对相邻 LUT 节点之间线性插值。乘 μ 后是二次式，Simpson 积分精确。
    double     integral = 0;
    const auto step     = 1. / static_cast<double>(table.width() - 1);
    for (std::size_t x = 1; x < table.width(); ++x) {
        auto         a = table.at(x - 1, row), b = table.at(x, row);
        const double left  = std::clamp(double(a.r + a.g), 0., 1.),
                     right = std::clamp(double(b.r + b.g), 0., 1.);
        const double mu    = static_cast<double>(x) * step;
        integral +=
            step / 3 * ((mu - step) * left + 2 * (mu - step * .5) * (left + right) + mu * right);
    }
    for (std::size_t x = 0; x < table.width(); ++x) {
        table.at(x, row).b = static_cast<float>(std::clamp(integral, 0., 1.));
    }
}

const Image& conductor_energy_table() {
    static const auto table = bake_ibl_brdf();
    return *table;
}

glm::dvec2 sample_energy(const Image& table, double mu, double roughness) {
    const auto x  = std::clamp(mu, 0., 1.) * static_cast<double>(table.width() - 1);
    const auto y  = std::clamp(roughness, 0., 1.) * static_cast<double>(table.height() - 1);
    const auto ix = static_cast<std::size_t>(x), iy = static_cast<std::size_t>(y);
    const auto jx = std::min(ix + 1, table.width() - 1), jy = std::min(iy + 1, table.height() - 1);
    const auto interpolate = [&](std::size_t row) {
        return glm::mix(glm::dvec3(table.at(ix, row)), glm::dvec3(table.at(jx, row)),
                        x - static_cast<double>(ix));
    };
    const auto e = glm::mix(interpolate(iy), interpolate(jy), y - static_cast<double>(iy));
    return {std::clamp(e.r + e.g, 0., 1.), std::clamp(e.b, 0., 1.)};
}

glm::dvec3 multiple_scattering(glm::dvec3 f0, double ei, double eo, double average) {
    if (average >= 1. - 1e-6) {
        return glm::dvec3(0);
    }
    const auto f     = f0 + (1. - f0) / 21.;
    const auto color = f * f * average / glm::max(1. - f * (1. - average), glm::dvec3(1e-8));
    return color * ((1. - ei) * (1. - eo) / (std::numbers::pi * (1. - average)));
}
} // namespace minirender
