#pragma once

#include <minirender/scene/scene.hpp>

#include <functional>

namespace minirender {

// 点云使用物体局部坐标，法线可为空或与位置等长。已有法线须为有限非零方向。
// 点没有面连接；重建输出通过普通 MeshData 接入场景。
struct PointCloud {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    void validate(CancellationToken cancellation = {}) const;
};

using PointProgress = std::function<void(std::size_t completed, std::size_t total)>;

// 按三角形面积采样位置，并插值面角法线。相同网格、数量和种子生成相同点集。
// 进度包括输入面的面积累积与点采样，输入保持不变。
PointCloud sample_mesh_surface(const MeshData& mesh, std::size_t count, std::uint64_t seed = 101,
                               CancellationToken cancellation = {}, PointProgress progress = {});

} // namespace minirender
