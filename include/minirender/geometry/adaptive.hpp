#pragma once

#include <minirender/geometry/displacement.hpp>

#include <array>
#include <optional>

namespace minirender {

// 米制误差在物体局部空间比较位置；像素误差使用任务启动时固定的投影视图。
enum class AdaptiveMetric { local_distance, screen_pixels };
struct AdaptiveProjection {
    glm::dmat4 clip_from_object{1};
    glm::dvec2 viewport_pixels{1};
    void validate() const;
};

// max_level 定义有限均匀 Loop 参照，输出仅细化需要的区域。
// 求值点预算包含法线一环和误差查询的支持点，独立于最终三角形预算。
struct AdaptiveSettings {
    std::uint32_t minimum_level = 0;
    std::uint32_t maximum_level = 5;
    double tolerance = 0.002;
    std::size_t maximum_triangles = 2000000;
    std::size_t maximum_evaluation_points = 4000000;
    AdaptiveMetric metric = AdaptiveMetric::local_distance;
    std::optional<AdaptiveProjection> projection;
    std::optional<DisplacementSettings> displacement;
    void validate() const;
};

enum class AdaptiveStage {
    topology,
    seams,
    sampling,
    refinement,
    balance,
    transitions,
    normals,
    output,
    complete
};
struct AdaptiveProgress {
    AdaptiveStage stage = AdaptiveStage::topology;
    std::size_t completed = 0;
    std::size_t total = 1;
    std::size_t current_triangles = 0;
    std::size_t evaluation_points = 0;
};
using AdaptiveProgressCallback = std::function<void(const AdaptiveProgress&)>;

// 三角形层级取所属 red 叶子的层级，过渡面计入该叶子。
// 误差为实际终端三角形上的采样误差，单位由 metric 决定。
struct AdaptiveResult {
    std::shared_ptr<const MeshData> mesh;
    TopologyStatistics input_topology;
    TopologyStatistics topology;
    AdaptiveMetric metric = AdaptiveMetric::local_distance;
    double applied_tolerance = 0;
    double maximum_sampled_error = 0;
    double maximum_displacement = 0;
    double maximum_seam_difference = 0;
    std::size_t red_refinements = 0;
    std::size_t balancing_refinements = 0;
    std::size_t transition_triangles = 0;
    std::size_t evaluation_points = 0;
    std::size_t reference_queries = 0;
    std::uint32_t deepest_level = 0;
    std::array<std::size_t, 9> triangles_per_level{};
    std::vector<std::uint8_t> triangle_levels;
    bool maximum_level_reached = false;
};

// 原网格有向流形拓扑保持，shared point ID 与终端过渡消除 T 接点。
// 输入、视图和高度纹理固定；预算失败或取消不发布部分结果。
AdaptiveResult subdivide_adaptive(std::shared_ptr<const MeshData> input,
                                  std::shared_ptr<const Texture> height = {},
                                  AdaptiveSettings settings = {},
                                  CancellationToken cancellation = {},
                                  AdaptiveProgressCallback progress = {});

} // namespace minirender
