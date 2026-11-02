#include <minirender/scene/shadow.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace minirender {

glm::mat4 directional_shadow_matrix(const SceneSnapshot& scene, glm::vec3 light_direction) {
    const float length = glm::length(light_direction);
    if (!std::isfinite(length) || length <= 1e-6F) {
        throw std::invalid_argument("Shadow light direction must be finite and nonzero");
    }
    glm::vec3 minimum(std::numeric_limits<float>::infinity());
    glm::vec3 maximum(-std::numeric_limits<float>::infinity());
    bool has_geometry = false;
    for (const auto& instance : scene.instances) {
        for (const auto position : instance.mesh->positions) {
            const auto world = glm::vec3(instance.object_to_world * glm::vec4(position, 1));
            minimum = glm::min(minimum, world);
            maximum = glm::max(maximum, world);
            has_geometry = true;
        }
    }
    if (!has_geometry) {
        return glm::mat4(1);
    }
    const glm::vec3 center = (minimum + maximum) * 0.5F;
    const float radius = std::max(glm::length(maximum - minimum) * 0.5F, 0.1F);
    const auto direction = light_direction / length;
    const glm::vec3 up = std::abs(direction.y) < 0.95F ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    const auto view = glm::lookAtRH(center - direction * (radius * 2 + 1), center, up);
    glm::vec3 low(std::numeric_limits<float>::infinity());
    glm::vec3 high(-std::numeric_limits<float>::infinity());
    for (unsigned corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{(corner & 1U) ? maximum.x : minimum.x,
                              (corner & 2U) ? maximum.y : minimum.y,
                              (corner & 4U) ? maximum.z : minimum.z};
        const auto light = glm::vec3(view * glm::vec4(point, 1));
        low = glm::min(low, light);
        high = glm::max(high, light);
    }
    const float padding = radius * 0.05F + 0.01F;
    auto projection =
        glm::orthoRH_ZO(low.x - padding, high.x + padding, low.y - padding, high.y + padding,
                        std::max(0.01F, -high.z - padding), -low.z + padding);
    projection[1][1] *= -1;
    return projection * view;
}

} // namespace minirender
