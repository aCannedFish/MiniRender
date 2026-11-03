#include <minirender/core/sampling.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {

constexpr double pi = std::numbers::pi;

void check_unit(double u) {
    if (!(u >= 0 && u < 1)) {
        throw std::invalid_argument("Sample coordinate must be in [0,1)");
    }
}
void check_unit(glm::dvec2 u) {
    check_unit(u.x);
    check_unit(u.y);
}

// SplitMix64 的整数混合只用于派生初始状态，PCG32 负责连续随机输出。
std::uint64_t mix(std::uint64_t value) {
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

} // namespace

Sampler::Sampler(std::uint64_t seed, std::uint64_t sequence) : increment_((sequence << 1U) | 1U) {
    // PCG XSH-RR 的初始化与输出变换参考 Melissa O'Neill 的 PCG32；来源见 SAMPLING.md。
    next_u32();
    state_ += seed;
    next_u32();
}
Sampler Sampler::for_sample(std::uint64_t seed, std::uint64_t pixel, std::uint64_t sample) {
    return Sampler(mix(seed ^ mix(pixel + 0x9e3779b97f4a7c15ULL)),
                   mix(sample + 0xd1b54a32d192ed03ULL) ^ mix(pixel));
}
std::uint32_t Sampler::next_u32() {
    const auto previous = state_;
    state_ = previous * 6364136223846793005ULL + increment_;
    const auto shifted = static_cast<std::uint32_t>(((previous >> 18U) ^ previous) >> 27U);
    const auto rotation = static_cast<std::uint32_t>(previous >> 59U);
    return (shifted >> rotation) | (shifted << ((0U - rotation) & 31U));
}
double Sampler::next_1d() {
    const auto high = next_u32() >> 5U;
    const auto low = next_u32() >> 6U;
    return (static_cast<double>(high) * 67108864.0 + low) / 9007199254740992.0;
}
glm::dvec2 Sampler::next_2d() {
    const double x = next_1d();
    return {x, next_1d()};
}

glm::dvec2 sample_disk(glm::dvec2 u) {
    check_unit(u);
    const auto square = 2.0 * u - 1.0;
    if (square.x == 0 && square.y == 0) {
        return {0, 0};
    }
    // Shirley–Chiu 同心映射，半径与方位角同时由方形所在扇区确定。
    const bool horizontal = std::abs(square.x) > std::abs(square.y);
    const double radius = horizontal ? square.x : square.y;
    const double angle =
        horizontal ? (pi / 4) * (square.y / square.x) : pi / 2 - (pi / 4) * (square.x / square.y);
    return radius * glm::dvec2(std::cos(angle), std::sin(angle));
}
glm::dvec3 sample_sphere(glm::dvec2 u) {
    check_unit(u);
    const double z = 1 - 2 * u.x;
    const double radius = std::sqrt(std::max(0.0, 1 - z * z));
    return {radius * std::cos(2 * pi * u.y), radius * std::sin(2 * pi * u.y), z};
}
glm::dvec3 sample_hemisphere(glm::dvec2 u) {
    check_unit(u);
    const double radius = std::sqrt(std::max(0.0, 1 - u.x * u.x));
    return {radius * std::cos(2 * pi * u.y), radius * std::sin(2 * pi * u.y), u.x};
}
glm::dvec3 sample_cosine_hemisphere(glm::dvec2 u) {
    const auto disk = sample_disk(u);
    return {disk.x, disk.y, std::sqrt(std::max(0.0, 1 - glm::dot(disk, disk)))};
}
glm::dvec3 sample_triangle(glm::dvec2 u) {
    check_unit(u);
    const double root = std::sqrt(u.x);
    return {1 - root, root * (1 - u.y), root * u.y};
}
double cosine_hemisphere_pdf(glm::dvec3 direction) {
    return std::max(0.0, direction.z) / pi;
}

DiscreteDistribution::DiscreteDistribution(std::span<const double> weights)
    : cdf_(weights.size() + 1, 0) {
    double maximum = 0;
    for (const auto weight : weights) {
        if (!std::isfinite(weight) || weight < 0) {
            throw std::invalid_argument("Distribution weights must be finite and nonnegative");
        }
        maximum = std::max(maximum, weight);
    }
    for (std::size_t index = 0; index < weights.size(); ++index) {
        cdf_[index + 1] = cdf_[index] + (maximum > 0 ? weights[index] / maximum : 1);
    }
    if (!weights.empty()) {
        const double integral = cdf_.back();
        for (auto& value : cdf_) {
            value /= integral;
        }
    }
}
std::optional<DiscreteSample> DiscreteDistribution::sample(double u) const {
    check_unit(u);
    if (size() == 0) {
        return std::nullopt;
    }
    const auto upper = std::upper_bound(cdf_.begin(), cdf_.end(), u);
    const auto index = static_cast<std::size_t>(upper - cdf_.begin() - 1);
    const double mass = probability(index);
    const double remapped = std::min((u - cdf_[index]) / mass, std::nextafter(1.0, 0.0));
    return DiscreteSample{index, mass, remapped};
}
double DiscreteDistribution::probability(std::size_t index) const {
    if (index >= size()) {
        throw std::out_of_range("Distribution index is out of range");
    }
    return cdf_[index + 1] - cdf_[index];
}
std::size_t DiscreteDistribution::size() const noexcept {
    return cdf_.size() - 1;
}

} // namespace minirender
