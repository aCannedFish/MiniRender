#include "ball_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender::detail {

std::optional<ContactBalls> contact_balls(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, double radius) {
    if (!std::isfinite(radius) || radius <= 0) {
        throw std::invalid_argument("Contact ball radius must be positive and finite");
    }
    const auto u = b - a;
    const auto v = c - a;
    const auto cross = glm::cross(u, v);
    const double squared = glm::dot(cross, cross);
    const double scale = std::max(glm::dot(u, u), glm::dot(v, v));
    // 绝对门槛与 MeshData 一致，相对门槛拒绝近共线三点的不稳定外接圆。
    if (!std::isfinite(squared) || squared <= 1e-20 || squared <= scale * scale * 1e-24) {
        return {};
    }
    const auto relative =
        (glm::dot(u, u) * glm::cross(v, cross) + glm::dot(v, v) * glm::cross(cross, u)) /
        (2 * squared);
    const double height_squared = radius * radius - glm::dot(relative, relative);
    if (height_squared < -radius * radius * 1e-12) {
        return {};
    }
    const auto center = a + relative;
    const auto normal = cross / std::sqrt(squared);
    const auto offset = normal * std::sqrt(std::max(0.0, height_squared));
    return ContactBalls{center, normal, center + offset, center - offset};
}

double contact_angle(glm::dvec3 axis, glm::dvec3 start, glm::dvec3 end) {
    if (glm::dot(start, start) == 0 || glm::dot(end, end) == 0) {
        return 0;
    }
    return std::atan2(glm::dot(axis, glm::cross(start, end)), glm::dot(start, end));
}

} // namespace minirender::detail
