#include <minirender/scene/medium.hpp>

#include <cmath>
#include <glm/glm.hpp>
#include <set>
#include <stdexcept>

namespace minirender {
namespace {
bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

glm::dvec3 HomogeneousMedium::extinction() const noexcept {
    return absorption + scattering;
}
void HomogeneousMedium::validate() const {
    if (!finite(absorption) || !finite(scattering) || !finite(extinction()) ||
        glm::any(glm::lessThan(absorption, glm::dvec3(0))) ||
        glm::any(glm::lessThan(scattering, glm::dvec3(0))) || !std::isfinite(anisotropy) ||
        std::abs(anisotropy) >= 1) {
        throw std::invalid_argument("Medium coefficients must be finite/nonnegative and |g| < 1");
    }
}
void MediumRegion::validate() const {
    medium.validate();
    if (id.empty() || !finite(minimum) || !finite(maximum) || !finite(maximum - minimum) ||
        glm::any(glm::greaterThanEqual(minimum, maximum))) {
        throw std::invalid_argument("Medium region requires an ID and a finite positive box");
    }
}
void validate_medium_regions(std::span<const MediumRegion> regions,
                             CancellationToken cancellation) {
    cancellation.check();
    if (regions.size() > maximum_medium_regions) {
        throw std::invalid_argument("Scene supports at most 32 medium regions");
    }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        cancellation.check();
        const auto& region = regions[i];
        region.validate();
        if (!ids.insert(region.id).second) {
            throw std::invalid_argument("Duplicate medium region ID: " + region.id);
        }
        if (!region.enabled) {
            continue;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = regions[j];
            if (other.enabled && glm::all(glm::lessThan(glm::max(region.minimum, other.minimum),
                                                        glm::min(region.maximum, other.maximum)))) {
                throw std::invalid_argument("Medium region interiors overlap: " + region.id +
                                            " and " + other.id);
            }
        }
    }
}
} // namespace minirender
