#pragma once

#include <glm/glm.hpp>

#include <optional>

namespace minirender::detail {

// 三点外接圆及半径为 r 的两个接触球，outward 位于三角形正面半空间。
struct ContactBalls {
    glm::dvec3 circumcenter;
    glm::dvec3 normal;
    glm::dvec3 outward;
    glm::dvec3 inward;
};
std::optional<ContactBalls> contact_balls(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, double radius);

// 绕单位轴从 start 到 end 的有向角，输出 [-pi,pi]。零向量用于共球退化接触。
double contact_angle(glm::dvec3 axis, glm::dvec3 start, glm::dvec3 end);

} // namespace minirender::detail
