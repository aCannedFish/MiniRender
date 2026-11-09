#pragma once

#include <minirender/geometry/point_cloud.hpp>

#include <limits>

namespace minirender {

// neighbors 包含中心点。距离限制同时约束 PCA 邻域和方向传播图，单位为局部米。
// estimate=false 保留已有方向；orient 使用最小生成森林统一符号，flip 最后整体翻转。
struct PointNormalSettings {
    std::size_t neighbors = 16;
    double maximum_distance = std::numeric_limits<double>::infinity();
    std::size_t maximum_neighbor_links = 16000000;
    bool estimate = true;
    bool orient = true;
    bool flip = false;
    void validate() const;
};

enum class PointNormalStage { validation, duplicates, index, estimation, orientation, complete };
struct PointNormalProgress {
    PointNormalStage stage;
    std::size_t completed;
    std::size_t total;
};
using PointNormalCallback = std::function<void(const PointNormalProgress&)>;

// 输出合并精确重复点，source_indices 指向各唯一点的首条输入记录。
// 间距统计采用距离限制内最近的其他点；orientation_components=0 表示未执行方向传播。
struct PointNormalResult {
    std::shared_ptr<const PointCloud> cloud;
    std::vector<std::uint32_t> source_indices;
    std::size_t duplicate_points = 0;
    std::size_t orientation_components = 0;
    std::size_t reversed_normals = 0;
    double spacing_p10 = 0;
    double spacing_median = 0;
    double spacing_p90 = 0;
};

// PCA 取局部协方差最小特征向量，近共线/无稳定法线的邻域报错。
// 方向传播按 1-|dot| 的最小生成森林执行，各分量最高 Z 点朝 +Z 初始化。
PointNormalResult prepare_point_normals(std::shared_ptr<const PointCloud> input,
                                        PointNormalSettings settings = {},
                                        CancellationToken cancellation = {},
                                        PointNormalCallback progress = {});

} // namespace minirender
