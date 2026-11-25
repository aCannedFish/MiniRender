// 由几何或投影误差控制的自适应细分与层级平衡。

#pragma once

#include <minirender/geometry/displacement.hpp>

#include <array>
#include <optional>

namespace minirender {

// 米制误差在物体局部空间比较位置；像素误差使用任务启动时固定的投影视图。
enum class AdaptiveMetric { local_distance, screen_pixels };

// 固定本次误差评估的投影和视口，将局部几何误差换算成像素。
struct AdaptiveProjection {
    // 从物体局部坐标到裁剪空间的固定变换。
    glm::dmat4 clip_from_object{1};
    // 投影误差换算使用的视口像素尺寸。
    glm::dvec2 viewport_pixels{1};
    // 检查数值范围和预算组合，非法设置在修改网格前抛出异常。
    void validate() const;
};

// max_level 定义有限均匀 Loop 参照，输出仅细化需要的区域。
// 求值点预算包含法线一环和误差查询的支持点，独立于最终三角形预算。
struct AdaptiveSettings {
    // 所有区域至少细分到的层级。
    std::uint32_t minimum_level = 0;
    // 有限层参考曲面与自适应输出允许的最大层级。
    std::uint32_t maximum_level = 5;
    double        tolerance     = 0.002;
    // 输出网格的三角形数量上限，分配前检查预算。
    std::size_t maximum_triangles = 2000000;
    // 包括邻域支持点在内的曲面求值预算。
    std::size_t maximum_evaluation_points = 4000000;
    // 比较位置误差时使用局部距离或屏幕像素。
    AdaptiveMetric metric = AdaptiveMetric::local_distance;
    // 像素误差模式使用的固定投影，与任务启动时视图一致。
    std::optional<AdaptiveProjection> projection;
    // 可选的高度位移配置，参与参考曲面和误差计算。
    std::optional<DisplacementSettings> displacement;
    // 检查数值范围和预算组合，非法设置在修改网格前抛出异常。
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

// 跟踪自适应细分的当前阶段，以及该阶段的完成量与总量。
struct AdaptiveProgress {
    // 当前执行阶段，completed 和 total 在阶段内解释。
    AdaptiveStage stage = AdaptiveStage::topology;
    // 当前阶段已完成的工作项数量。
    std::size_t completed = 0;
    // 当前阶段工作项总数。
    std::size_t total = 1;
    // 当前工作网格的面数。
    std::size_t current_triangles = 0;
    // 实际创建并缓存的层级求值点数量。
    std::size_t evaluation_points = 0;
};

using AdaptiveProgressCallback = std::function<void(const AdaptiveProgress&)>;

// 三角形层级取所属 red 叶子的层级，过渡面计入该叶子。
// 误差为实际终端三角形上的采样误差，单位由 metric 决定。
struct AdaptiveResult {
    // 成功结果的共享只读网格，调用方可以直接保存或交给渲染器。
    std::shared_ptr<const MeshData> mesh;
    // 算法开始时输入网格的拓扑统计。
    TopologyStatistics input_topology;
    // 输出网格通过拓扑检查后的统计。
    TopologyStatistics topology;
    // 比较位置误差时使用局部距离或屏幕像素。
    AdaptiveMetric metric = AdaptiveMetric::local_distance;
    // 本次算法实际采用的误差阈值。
    double applied_tolerance = 0;
    // 终端网格在检查点处的最大误差，单位由 metric 决定。
    double maximum_sampled_error = 0;
    // 输出位置相对输入或参考曲面的最大位移长度。
    double maximum_displacement = 0;
    // 接缝两侧最大位移距离差，单位为局部米。
    double maximum_seam_difference = 0;
    // 由误差驱动执行的常规四分细化次数。
    std::size_t red_refinements = 0;
    // 为保持相邻层级平衡而补充的细化次数。
    std::size_t balancing_refinements = 0;
    // 为消除 T 接点而生成的过渡三角形数量。
    std::size_t transition_triangles = 0;
    // 实际创建并缓存的层级求值点数量。
    std::size_t evaluation_points = 0;
    // 对参考曲面执行的采样查询次数。
    std::size_t reference_queries = 0;
    // 实际到达的最深细分层。
    std::uint32_t deepest_level = 0;
    // 各层输出三角形数量，包含属于该层的过渡面。
    std::array<std::size_t, 9> triangles_per_level{};
    // 与输出三角形顺序对应的层级标记。
    std::vector<std::uint8_t> triangle_levels;
    // 是否因达到最高层级而停止继续细化。
    bool maximum_level_reached = false;
};

// 原网格有向流形拓扑保持，shared point ID 与终端过渡消除 T 接点。
// 输入、视图和高度纹理固定；预算失败或取消不发布部分结果。
AdaptiveResult subdivide_adaptive(std::shared_ptr<const MeshData> input,
                                  std::shared_ptr<const Texture>  height       = {},
                                  AdaptiveSettings                settings     = {},
                                  CancellationToken               cancellation = {},
                                  AdaptiveProgressCallback        progress     = {});

} // namespace minirender
