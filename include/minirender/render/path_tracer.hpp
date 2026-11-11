#pragma once

#include <minirender/render/bsdf.hpp>
#include <minirender/render/light.hpp>
#include <minirender/render/medium.hpp>
#include <minirender/render/sppm_options.hpp>
#include <minirender/render/subsurface.hpp>

namespace minirender {

enum class SamplingMode { bsdf, light, mis };
enum class IntegratorKind { path_tracing, sppm };
std::string sampling_mode_name(SamplingMode mode);
SamplingMode parse_sampling_mode(const std::string& name);

struct IntegratorSettings {
    SamplingMode mode = SamplingMode::mis;
    std::uint32_t maximum_depth = 8; // 允许的散射事件数；0 只记录相机直接可见的发射/环境。
    bool russian_roulette = true;
    std::uint32_t roulette_depth = 3;
    IntegratorKind kind = IntegratorKind::path_tracing;
    SppmOptions sppm{};
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
    std::uint64_t medium_scattering_events = 0;
    std::uint64_t subsurface_events = 0;
    double subsurface_distance = 0; // 已采样表面位置跳转的总世界距离，单位为米。
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
    // 一个闭合材质边界的反射或跨位置扩散，合并为一次外部散射事件。
    struct SubsurfaceStep {
        glm::dvec3 radiance{0}, weight{0}, reference{0};
        Ray ray;
        double probability = 0;
        bool delta = false;
    };
    SubsurfaceStep scatter_subsurface(const SurfaceHit& hit, glm::dvec3 wo, Sampler& sampler,
                                      PathStatistics* statistics) const;
    glm::dvec3 shadow_transmittance(Ray ray, const LightSample& light,
                                    PathStatistics* statistics) const;
    std::shared_ptr<const SceneSnapshot> scene_;
    SceneBvh bvh_;
    LightSampler lights_;
    MediumField media_;
    SubsurfaceSampler subsurface_;
    IntegratorSettings settings_;
    CancellationToken cancellation_;
};

// 保留原始 HDR 图像；返回应用曝光和 Reinhard 后的线性显示图，由 PNG 编码器做 sRGB。
Image display_image(const Image& linear, double exposure = 0);

} // namespace minirender
