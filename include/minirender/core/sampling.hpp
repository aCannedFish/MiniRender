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
    explicit Sampler(std::uint64_t seed = 0, std::uint64_t sequence = 1);
    static Sampler for_sample(std::uint64_t seed, std::uint64_t pixel, std::uint64_t sample);
    std::uint32_t next_u32();
    double next_1d();
    glm::dvec2 next_2d();

  private:
    std::uint64_t state_ = 0;
    std::uint64_t increment_ = 1;
};

// 以下映射接收 [0,1)^2。半球位于局部 +Z，方向采用立体角测度。
// 圆盘半径为 1；三角形返回三个顶点的重心权重，面积 PDF 由调用方按面积计算。
glm::dvec2 sample_disk(glm::dvec2 u);
glm::dvec3 sample_sphere(glm::dvec2 u);
glm::dvec3 sample_hemisphere(glm::dvec2 u);
glm::dvec3 sample_cosine_hemisphere(glm::dvec2 u);
glm::dvec3 sample_triangle(glm::dvec2 u);
double cosine_hemisphere_pdf(glm::dvec3 direction);

struct DiscreteSample {
    std::size_t index = 0;
    double probability = 0;
    double remapped = 0; // 选中区间内重新映射到 [0,1)，可复用为后续条件采样。
};

// 非负有限权重的离散 CDF。空分布不产生样本，全零权重退化为均匀分布。
// 概率由最终 CDF 区间宽度定义，sample 与 probability 使用相同的浮点分布。
class DiscreteDistribution {
  public:
    explicit DiscreteDistribution(std::span<const double> weights = {});
    std::optional<DiscreteSample> sample(double u) const;
    double probability(std::size_t index) const;
    std::size_t size() const noexcept;

  private:
    std::vector<double> cdf_;
};

} // namespace minirender
