#pragma once

#include <minirender/geometry/point_cloud.hpp>

namespace minirender {

// 后台准备的实例属性。预览位置已在 double 中减去中心并缩放，避免 GPU 大坐标相减。
struct PointPreviewVertex {
    glm::vec3 position{0};
    glm::vec3 normal{0};
};
static_assert(sizeof(PointPreviewVertex) == 24);

// 原始坐标与网格保持不变；预览将最长轴映射到 [-1,1]，相机使用这一独立空间。
// 法线按输入顺序均匀抽样，边界包含全部边段，均可在 GUI 线程直接上传连续缓冲。
struct PointPreviewData {
    std::vector<PointPreviewVertex> points;
    std::vector<PointPreviewVertex> normal_samples;
    std::vector<PointPreviewVertex> boundary_vertices;
    glm::vec3 minimum{0};
    glm::vec3 maximum{0};
    glm::dvec3 center{0};
    double scale = 1;
    bool has_normals = false;
    std::size_t boundary_edges = 0;
};

// 诊断显示参数使用物理像素与归一预览空间，不参与重建或修改原始数据。
struct PointPreviewSettings {
    float point_radius = 2;
    float normal_length = .06F;
    bool normal_colors = false;
    bool show_normals = false;
    bool show_boundaries = true;
    void validate() const;
};

std::shared_ptr<const PointPreviewData>
prepare_point_preview(const PointCloud& cloud, const MeshData* reconstruction = nullptr,
                      CancellationToken cancellation = {}, PointProgress progress = {},
                      std::size_t maximum_normal_samples = 10000);

} // namespace minirender
