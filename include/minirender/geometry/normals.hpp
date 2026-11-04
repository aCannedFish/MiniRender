#pragma once

#include <minirender/scene/scene.hpp>

#include <functional>

namespace minirender {

using MeshNormalProgress = std::function<void(std::size_t completed, std::size_t total)>;

// 在物体局部坐标中累积面积加权法线，跨面角 UV/硬法线接缝共享方向。
// 输入须有有效位置索引。退化面或抵消的法线报错；孤立位置返回零向量。
// 进度单位为逐面累积与逐位置归一化，回调在调用线程中执行。
std::vector<glm::vec3> smooth_vertex_normals(const MeshData& mesh,
                                             CancellationToken cancellation = {},
                                             MeshNormalProgress progress = {});

// 将上述方向写回每个面角。总量还包含 3*面数次写回，不改变位置、UV 和连接。
void recompute_smooth_normals(MeshData& mesh, CancellationToken cancellation = {},
                              MeshNormalProgress progress = {});

} // namespace minirender
