#pragma once

#include <array>
#include <glm/glm.hpp>
#include <optional>

namespace minirender {

// 三角形局部空间中沿 UV 增加的单位方向，可以保持剪切后的夹角。
struct UvTangents {
    glm::dvec3 u{0};
    glm::dvec3 v{0};
};

// 从位置与 UV 的线性映射求方向。退化 UV 或几何返回空值，符号保留镜像 UV 的朝向。
std::optional<UvTangents> uv_tangents(const std::array<glm::vec3, 3>& positions,
                                      const std::array<glm::vec2, 3>& coordinates);

} // namespace minirender
