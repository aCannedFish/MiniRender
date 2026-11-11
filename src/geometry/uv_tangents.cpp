#include <minirender/geometry/uv_tangents.hpp>

#include <algorithm>
#include <cmath>

namespace minirender {

std::optional<UvTangents> uv_tangents(const std::array<glm::vec3, 3>& positions,
                                      const std::array<glm::vec2, 3>& coordinates) {
    const auto a = glm::dvec3(positions[1]) - glm::dvec3(positions[0]);
    const auto b = glm::dvec3(positions[2]) - glm::dvec3(positions[0]);
    auto s = glm::dvec2(coordinates[1]) - glm::dvec2(coordinates[0]);
    auto t = glm::dvec2(coordinates[2]) - glm::dvec2(coordinates[0]);
    const double scale = std::max({std::abs(s.x), std::abs(s.y), std::abs(t.x), std::abs(t.y)});
    if (!std::isfinite(scale) || scale == 0) {
        return std::nullopt;
    }
    s /= scale;
    t /= scale;
    const double determinant = s.x * t.y - s.y * t.x;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
        return std::nullopt;
    }
    // 只需方向，先约去 UV 尺度；求逆的符号仍参与两个方向。
    auto u = (t.y * a - s.y * b) / determinant;
    auto v = (-t.x * a + s.x * b) / determinant;
    const double lu = glm::length(u), lv = glm::length(v);
    if (!std::isfinite(lu) || !std::isfinite(lv) || lu == 0 || lv == 0) {
        return std::nullopt;
    }
    return UvTangents{u / lu, v / lv};
}

} // namespace minirender
