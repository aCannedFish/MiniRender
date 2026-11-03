#include <minirender/core/sampling.hpp>
#include <minirender/render/ssao.hpp>

#include <cmath>
#include <stdexcept>

namespace minirender {

void SsaoSettings::validate() const {
    if (!std::isfinite(radius) || radius <= 0 || radius > 1000 || !std::isfinite(bias) ||
        bias < 0 || bias > radius || !std::isfinite(strength) || strength < 0 || strength > 4 ||
        samples == 0 || samples > ssao_kernel_size) {
        throw std::invalid_argument("SSAO requires radius in (0,1000], bias in [0,radius], "
                                    "strength in [0,4], and 1..64 samples");
    }
}
std::array<glm::vec4, ssao_kernel_size> make_ssao_kernel(std::uint64_t seed) {
    Sampler random(seed);
    std::array<glm::vec4, ssao_kernel_size> kernel{};
    for (auto& point : kernel) {
        const auto direction = sample_cosine_hemisphere(random.next_2d());
        const auto u = random.next_1d();
        const auto distance = 0.1 + 0.9 * u * u;
        point = glm::vec4(glm::vec3(direction * distance), 0);
    }
    return kernel;
}

} // namespace minirender
