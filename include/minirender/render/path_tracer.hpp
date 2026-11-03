#pragma once

#include <minirender/render/bsdf.hpp>
#include <minirender/render/light.hpp>

namespace minirender {

enum class SamplingMode { bsdf, light, mis };
std::string sampling_mode_name(SamplingMode mode);
SamplingMode parse_sampling_mode(const std::string& name);

struct IntegratorSettings {
    SamplingMode mode = SamplingMode::mis;
    std::uint32_t maximum_depth = 8; // 允许的散射事件数；0 只记录相机直接可见的发射/环境。
    bool russian_roulette = true;
    std::uint32_t roulette_depth = 3;
    void validate() const;
};

struct RenderSettings {
    std::size_t width = 256;
    std::size_t height = 256;
    std::uint32_t samples_per_pixel = 64;
    std::uint64_t seed = 1;
    void validate() const;
};

struct PathStatistics {
    TraceStatistics geometry;
    std::uint64_t paths = 0;
    std::uint64_t shadow_rays = 0;
    std::uint64_t scattering_events = 0;
    std::uint64_t roulette_terminations = 0;
};

// 单样本 power heuristic，先缩放再平方，避免两个大 PDF 的平方溢出。
double power_heuristic(double sampled_pdf, double other_pdf);
// 从位置误差界沿几何法线偏移，再向外舍入。方向不朝向某个固定相机。
glm::dvec3 offset_origin(const SurfaceHit& hit, glm::dvec3 direction);
Ray spawn_ray(const SurfaceHit& hit, glm::dvec3 direction);

// 单线程参考路径积分器。场景、加速结构、光源和积分设置均在构造后保持不变。
// trace/sample_pixel 可供多个调用方并发使用，统计量由各调用方独占。
class PathTracer {
  public:
    explicit PathTracer(std::shared_ptr<const SceneSnapshot> scene,
                        IntegratorSettings settings = {}, CancellationToken cancellation = {});
    glm::dvec3 trace(Ray ray, Sampler& sampler,
                     RayInterval primary_interval = {0, std::numeric_limits<double>::infinity()},
                     PathStatistics* statistics = nullptr) const;
    // sample_index 与线程调度无关。每次调用只计算一个像素样本，供后续任务调度复用。
    glm::dvec3 sample_pixel(std::size_t x, std::size_t y, std::uint64_t sample_index,
                            const RenderSettings& settings,
                            PathStatistics* statistics = nullptr) const;
    Image render(const RenderSettings& settings, PathStatistics* statistics = nullptr) const;
    const SceneSnapshot& scene() const noexcept;

  private:
    bool visible(const SurfaceHit& hit, const LightSample& light, PathStatistics* statistics) const;
    std::shared_ptr<const SceneSnapshot> scene_;
    SceneBvh bvh_;
    LightSampler lights_;
    IntegratorSettings settings_;
    CancellationToken cancellation_;
};

// 保留原始 HDR 图像；返回应用曝光和 Reinhard 后的线性显示图，由 PNG 编码器做 sRGB。
Image display_image(const Image& linear, double exposure = 0);

} // namespace minirender
