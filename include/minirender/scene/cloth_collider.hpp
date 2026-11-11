#pragma once

#include <glm/vec3.hpp>

namespace minirender {

enum class ClothColliderKind { sphere, plane };

// 附着在普通场景物体上的布料碰撞体。point/normal/radius 使用物体局部坐标。
// visible 控制显示，enabled 独立控制碰撞；球体实例使用等比缩放。
struct ClothCollider {
    ClothColliderKind kind = ClothColliderKind::sphere;
    glm::dvec3 point{0};
    glm::dvec3 normal{0, 1, 0};
    double radius = 1;
    double friction = .3;
    bool enabled = true;
    void validate() const;
};

} // namespace minirender
