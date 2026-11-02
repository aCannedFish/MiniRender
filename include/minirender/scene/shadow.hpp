#pragma once

#include <minirender/scene/scene.hpp>

namespace minirender {

// 根据可见实例的世界空间包围盒拟合方向光正交投影。
// 输出 Vulkan clip space（Z∈[0,1]，Y 向下）；不依赖主相机，避免相机移动引起阴影漂移。
glm::mat4 directional_shadow_matrix(const SceneSnapshot& scene, glm::vec3 light_direction);

} // namespace minirender
