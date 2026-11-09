#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>

namespace minirender::detail {

// Loop 的内部顶点权重，均匀细分与按需层级使用同一约定。
inline double loop_beta(std::uint32_t valence) {
    const double n = valence;
    const double lambda = 3.0 / 8.0 + std::cos(2 * std::numbers::pi / n) / 4.0;
    return (5.0 / 8.0 - lambda * lambda) / n;
}

} // namespace minirender::detail
