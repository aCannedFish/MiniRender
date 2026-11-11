#include <minirender/scene/cloth_material.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace minirender {
namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}
bool range(double value, double low, double high) {
    return std::isfinite(value) && value >= low && value <= high;
}
double curve_angle(const ClothTangentCurve& curve, double position) {
    for (std::size_t segment = 0; segment < curve.lengths.size(); ++segment) {
        const double length = curve.lengths[segment];
        if (length > 0 && position < length) {
            const double t = position / length;
            return (1 - t) * curve.angles[segment] + t * curve.angles[segment + 1];
        }
        position -= length;
    }
    return curve.angles.back();
}

} // namespace

void ClothAppearanceSettings::validate() const {
    require(samples_per_thread >= 1 && samples_per_thread <= 16,
            "Cloth tangent sample count must be in 1..16");
    require(std::isfinite(rotation_degrees) && range(correlation_width_degrees, 1, 90),
            "Invalid cloth rotation or correlation width");
    require(range(coverage[0], 0, 1) && range(coverage[1], 0, 1) && coverage[0] + coverage[1] > 0 &&
                coverage[0] + coverage[1] <= 1,
            "Cloth coverage must be nonnegative with sum in (0,1]");
    for (const auto& thread : threads) {
        require(range(thread.albedo.x, 0, 1) && range(thread.albedo.y, 0, 1) &&
                    range(thread.albedo.z, 0, 1) && range(thread.kd, 0, 1),
                "Cloth albedo and isotropic fraction must be in 0..1");
        require(range(thread.surface_width_degrees, 1, 90) &&
                    range(thread.volume_width_degrees, 1, 90),
                "Cloth scattering widths must be in 1..90 degrees");
        const auto& curve = thread.curve;
        require(curve.angles.size() >= 2 && curve.angles.size() <= 16 &&
                    curve.lengths.size() + 1 == curve.angles.size(),
                "Cloth tangent curve requires 2..16 angles and one length per segment");
        for (const auto angle : curve.angles) {
            require(range(angle, -90, 90), "Cloth tangent angles must be in -90..90 degrees");
        }
        double total = 0;
        for (const auto length : curve.lengths) {
            require(range(length, 0, 1e4), "Cloth segment lengths must be in 0..10000");
            total += length;
        }
        require(total > 0, "Cloth tangent curve has zero total length");
    }
}

ClothAppearance::ClothAppearance(ClothAppearanceSettings settings)
    : settings_(std::move(settings)) {
    settings_.validate();
    for (std::uint32_t thread = 0; thread < 2; ++thread) {
        const auto& curve = settings_.threads[thread].curve;
        const double length = std::accumulate(curve.lengths.begin(), curve.lengths.end(), 0.0);
        for (std::uint32_t sample = 0; sample < settings_.samples_per_thread; ++sample) {
            const double position = (sample + .5) * length / settings_.samples_per_thread;
            const double angle = curve_angle(curve, position) * std::numbers::pi / 180;
            samples_.push_back({std::cos(angle), std::sin(angle),
                                settings_.coverage[thread] / settings_.samples_per_thread, thread});
        }
    }
}
const ClothAppearanceSettings& ClothAppearance::settings() const noexcept {
    return settings_;
}
std::span<const ClothTangentSample> ClothAppearance::samples() const noexcept {
    return samples_;
}
double ClothAppearance::coverage() const noexcept {
    return settings_.coverage[0] + settings_.coverage[1];
}

ClothPreset make_cloth_preset(const std::string& name) {
    ClothAppearanceSettings settings;
    double ior = 1.46;
    if (name == "linen") {
        // 默认值对应 Table II(a)，两个方向均覆盖 0.33。
    } else if (name == "silk") {
        ior = 1.345;
        settings.coverage = {.75, .25};
        settings.threads[0] = {{.12, .114, .006}, .2, 5, 10, {{-35, -35, 35, 35}, {1, 1, 1}}};
        settings.threads[1] = {{.16, .152, .008}, .3, 18, 32, {{0, 0}, {1}}};
    } else if (name == "satin") {
        ior = 1.539;
        settings.coverage = {.9, .1};
        settings.threads[0] = {
            {.035, .01295, .0105},
            .1,
            2.5,
            5,
            {{-32, -32, -18, 0, 0, 18, 32, 32}, {1.33, .66, 2, 2, 2, .66, 1.33}}};
        settings.threads[1] = {{.2, .074, .06}, .7, 30, 60, {{0, 0}, {1}}};
    } else if (name == "shot") {
        ior = 1.345;
        settings.coverage = {.86, .14};
        settings.threads[0] = {{.02, .2, .08}, .1, 4, 8, {{-25, -25, 25, 25}, {1.33, 2.67, 1.33}}};
        settings.threads[1] = {{.6, 0, .06}, .1, 5, 10, {{0, 0}, {1}}};
    } else {
        throw std::invalid_argument("Unknown cloth preset: " + name);
    }
    return {name, ior, std::make_shared<const ClothAppearance>(std::move(settings))};
}

} // namespace minirender
