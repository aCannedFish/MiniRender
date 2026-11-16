// 可复现随机流、基础方向映射及离散概率分布。

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace minirender {

// 方向采样的连续立体角密度与离散事件概率质量不可直接参与同一次 MIS 比较。
enum class SampleMeasure { solid_angle, discrete };

// PCG32 随机流。next_1d 使用 53 个随机位生成 [0,1) 的 double。
// for_sample 按 seed/pixel/sample 创建独立可复现的流，线程调度不参与种子计算。
class Sampler {
  public:
    // 用 seed 和 sequence 初始化独立的 PCG 随机流。
    explicit Sampler(std::uint64_t seed = 0, std::uint64_t sequence = 1);
    // 由全局种子、像素和样本序号派生随机流，保证不同调度顺序得到相同样本。
    static Sampler for_sample(std::uint64_t seed, std::uint64_t pixel, std::uint64_t sample);
    // 推进随机状态并返回一个 32 位无符号随机值。
    std::uint32_t next_u32();
    // 返回 [0,1) 内的双精度随机数。
    double next_1d();
    // 连续取得两个独立的一维样本。
    glm::dvec2 next_2d();

  private:
    // PCG 当前状态，每次生成随机数后推进。
    std::uint64_t state_ = 0;
    // 区分随机流的奇数增量。
    std::uint64_t increment_ = 1;
};

// 以下映射接收 [0,1)^2。半球位于局部 +Z，方向采用立体角测度。
// 圆盘半径为 1；三角形返回三个顶点的重心权重，面积 PDF 由调用方按面积计算。
// 将单位方形样本映射到均匀单位圆盘。
glm::dvec2 sample_disk(glm::dvec2 u);
// 均匀采样单位球面方向。
glm::dvec3 sample_sphere(glm::dvec2 u);
// 均匀采样局部 +Z 半球方向。
glm::dvec3 sample_hemisphere(glm::dvec2 u);
// 按余弦分布采样局部 +Z 半球方向。
glm::dvec3 sample_cosine_hemisphere(glm::dvec2 u);
// 返回均匀面积三角形采样所需的三个重心权重。
glm::dvec3 sample_triangle(glm::dvec2 u);
// 计算局部方向的余弦立体角密度，背向半球密度为零。
double cosine_hemisphere_pdf(glm::dvec3 direction);

struct DiscreteSample {
    // 选中的离散项索引。
    std::size_t index = 0;
    // 该项的概率质量。
    double probability = 0;
    double remapped    = 0; // 选中区间内重新映射到 [0,1)，可复用为后续条件采样。
};

// 非负有限权重的离散 CDF。空分布不产生样本，全零权重退化为均匀分布。
// 概率由最终 CDF 区间宽度定义，sample 与 probability 使用相同的浮点分布。
class DiscreteDistribution {
  public:
    // 由非负权重构建累积分布，保留实际浮点区间宽度。
    explicit DiscreteDistribution(std::span<const double> weights = {});
    // 从 [0,1) 样本选择区间，并返回索引、概率及区间内重映射值。
    std::optional<DiscreteSample> sample(double u) const;
    // 查询指定离散项的概率质量。
    double probability(std::size_t index) const;
    // 返回分布包含的离散项数量。
    std::size_t size() const noexcept;

  private:
    // 累计概率边界，供二分定位与概率查询共用。
    std::vector<double> cdf_;
};

} // namespace minirender
