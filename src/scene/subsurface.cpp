#include <minirender/scene/subsurface.hpp>

#include <cmath>
#include <stdexcept>

namespace minirender {

void SubsurfaceParameters::validate() const {
    if (!std::isfinite(density) || density < 1e-6 || density > 1e6) {
        throw std::invalid_argument("Subsurface density must be in 1e-6..1e6");
    }
    for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(absorption[c]) || !std::isfinite(reduced_scattering[c]) ||
            absorption[c] < 0 || reduced_scattering[c] < 0 || absorption[c] > 1e6 ||
            reduced_scattering[c] > 1e6 || absorption[c] + reduced_scattering[c] < 1e-6) {
            throw std::invalid_argument(
                "Subsurface coefficients must be in 0..1e6 with positive extinction >= 1e-6");
        }
    }
}

SubsurfacePreset make_subsurface_preset(const std::string& name) {
    SubsurfaceParameters parameters;
    double ior = 1.5;
    if (name == "marble") {
        // 原文表格以 mm^-1 表示，默认参数已乘 1000 换成引擎的米制。
    } else if (name == "wax") {
        parameters.absorption = {12, 35, 80};
        parameters.reduced_scattering = {450, 550, 700};
        ior = 1.44;
    } else {
        throw std::invalid_argument("Unknown subsurface preset: " + name);
    }
    return {name, ior, std::make_shared<const SubsurfaceParameters>(parameters)};
}

} // namespace minirender
