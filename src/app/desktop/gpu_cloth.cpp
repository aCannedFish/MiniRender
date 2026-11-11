#include "gpu_cloth.hpp"

#include <cmath>
#include <numbers>

GpuCloth pack_cloth(const minirender::Material& material) {
    GpuCloth result{};
    if (material.kind != minirender::MaterialKind::cloth) {
        return result;
    }
    const auto& appearance = *material.cloth;
    const auto& settings = appearance.settings();
    constexpr double radians = std::numbers::pi / 180;
    const double angle = std::remainder(settings.rotation_degrees, 360.0) * radians;
    result.settings = {material.ior, settings.correlation_width_degrees * radians, std::cos(angle),
                       std::sin(angle)};
    result.counts = {appearance.samples().size(), 1, 0, 0};
    result.coverage = {settings.coverage[0], settings.coverage[1], 0, 0};
    for (std::size_t thread = 0; thread < 2; ++thread) {
        const auto& input = settings.threads[thread];
        const double surface = input.surface_width_degrees * radians;
        const double volume = input.volume_width_degrees * radians;
        result.albedo[thread] = {input.albedo, input.kd};
        result.widths[thread] = {surface, volume, 1 / (std::sqrt(2 * std::numbers::pi) * surface),
                                 1 / (std::sqrt(2 * std::numbers::pi) * volume)};
    }
    for (std::size_t i = 0; i < appearance.samples().size(); ++i) {
        const auto& input = appearance.samples()[i];
        result.tangents[i] = {input.cosine, input.sine, input.weight,
                              static_cast<float>(input.thread)};
    }
    return result;
}
