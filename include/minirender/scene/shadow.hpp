#pragma once

#include <minirender/scene/scene.hpp>

namespace minirender {

// 根据可见实例的世界空间包围盒拟合方向光正交投影。
// 输出 Vulkan clip space（Z∈[0,1]，Y 向下）；不依赖主相机，避免相机移动引起阴影漂移。
glm::mat4 directional_shadow_matrix(const SceneSnapshot& scene, glm::vec3 light_direction);

// 使用已缓存的世界空间包围盒，避免材质或灯光编辑时重新遍历所有顶点。
glm::mat4 directional_shadow_matrix(glm::vec3 minimum, glm::vec3 maximum,
                                    glm::vec3 light_direction);

} // namespace minirender
