// 表面与介质路径积分，连接散射采样、光源采样和 MIS。

#pragma once

#include <minirender/render/bsdf.hpp>
#include <minirender/render/light.hpp>
#include <minirender/render/medium.hpp>
#include <minirender/render/sppm_options.hpp>
#include <minirender/render/subsurface.hpp>

namespace minirender {

enum class SamplingMode { bsdf, light, mis };
enum class IntegratorKind { path_tracing, sppm };
// 返回采样策略的稳定名称，供命令行与文件参数使用。
std::string sampling_mode_name(SamplingMode mode);
// 把名称转换为采样策略；未知名称抛出参数异常。
SamplingMode parse_sampling_mode(const std::string& name);

struct IntegratorSettings {
    // 直接光采用 BSDF、光源或 MIS 采样。
    SamplingMode  mode          = SamplingMode::mis;
    std::uint32_t maximum_depth = 8; // 允许的散射事件数；0 只记录相机直接可见的发射/环境。
    // 启用基于路径吞吐量的随机终止。
    bool russian_roulette = true;
    // 达到该散射深度后才进行俄罗斯轮盘赌。
    std::uint32_t roulette_depth = 3;
    // 选择路径追踪或 SPPM 后端。
    IntegratorKind kind = IntegratorKind::path_tracing;
    // SPPM 后端的光子预算和半径更新参数。
    SppmOptions sppm{};
    // 检查参数范围，非法配置在建立渲染任务前抛出异常。
    void validate() const;
};

struct RenderSettings {
    // 输出图像宽度，单位为像素。
    std::size_t width = 256;
    // 输出图像高度，单位为像素。
    std::size_t height = 256;
    // 每像素采样预算；SPPM 中表示迭代次数。
    std::uint32_t samples_per_pixel = 64;
    // 决定各像素和样本的可复现随机序列。
    std::uint64_t seed = 1;
    // 检查参数范围，非法配置在建立渲染任务前抛出异常。
    void validate() const;
};

struct PathStatistics {
    // BVH 包围盒及三角形求交计数。
    TraceStatistics geometry;
    // 实际发射的相机路径数量。
    std::uint64_t paths = 0;
    // 直接光可见性查询数量。
    std::uint64_t shadow_rays = 0;
    // 路径中记录的散射事件总数。
    std::uint64_t scattering_events = 0;
    // 被俄罗斯轮盘赌提前终止的路径数。
    std::uint64_t roulette_terminations = 0;
    // 发生在介质内部的散射事件数。
    std::uint64_t medium_scattering_events = 0;
    // 发生在材质内部、跨表面位置的散射事件数。
    std::uint64_t subsurface_events   = 0;
    double        subsurface_distance = 0; // 已采样表面位置跳转的总世界距离，单位为米。
};

// 单样本 power heuristic，先缩放再平方，避免两个大 PDF 的平方溢出。
double power_heuristic(double sampled_pdf, double other_pdf);
// 从位置误差界沿几何法线偏移，再向外舍入。方向不朝向某个固定相机。
glm::dvec3 offset_origin(const SurfaceHit& hit, glm::dvec3 direction);
// 从表面误差范围之外产生二次射线，避免再次命中当前表面。
Ray spawn_ray(const SurfaceHit& hit, glm::dvec3 direction);

// 单线程参考路径积分器。场景、加速结构、光源和积分设置均在构造后保持不变。
// trace/sample_pixel 可供多个调用方并发使用，统计量由各调用方独占。
class PathTracer {
  public:
    // 固定场景快照，构建 BVH、光源与介质采样数据；构建过程响应取消。
    explicit PathTracer(std::shared_ptr<const SceneSnapshot> scene,
                        IntegratorSettings settings = {}, CancellationToken cancellation = {});
    // 累计一条主射线的线性辐射；primary_interval 只约束首次求交，统计累加到调用方对象。
    glm::dvec3 trace(Ray ray, Sampler& sampler,
                     RayInterval primary_interval = {0, std::numeric_limits<double>::infinity()},
                     PathStatistics* statistics   = nullptr) const;
    // sample_index 与线程调度无关。每次调用只计算一个像素样本，供后续任务调度复用。
    glm::dvec3 sample_pixel(std::size_t x, std::size_t y, std::uint64_t sample_index,
                            const RenderSettings& settings,
                            PathStatistics*       statistics = nullptr) const;
    // 串行计算完整图像，像素样本由固定种子生成；适合作为并行实现的参考。
    Image render(const RenderSettings& settings, PathStatistics* statistics = nullptr) const;
    // 返回积分器持有的只读场景，引用在积分器存活期间有效。
    const SceneSnapshot& scene() const noexcept;

  private:
    // 一个闭合材质边界的反射或跨位置扩散，合并为一次外部散射事件。
    struct SubsurfaceStep {
        glm::dvec3 radiance{0};
        glm::dvec3 weight{0};
        glm::dvec3 reference{0};
        Ray        ray;
        double     probability = 0;
        bool       delta       = false;
    };

    // 处理材质界面的反射或跨位置扩散，返回辐射、吞吐量和后续射线。
    SubsurfaceStep scatter_subsurface(const SurfaceHit& hit, glm::dvec3 wo, Sampler& sampler,
                                      PathStatistics* statistics) const;
    // 计算光源连接线段的介质透射率，并查询表面遮挡。
    glm::dvec3 shadow_transmittance(Ray ray, const LightSample& light,
                                    PathStatistics* statistics) const;
    // 共享不可变场景，保证后台积分期间资产仍然存活。
    std::shared_ptr<const SceneSnapshot> scene_;
    // 当前快照对应的表面加速结构。
    SceneBvh bvh_;
    // 快照内面积光、方向光和环境光的采样分布。
    LightSampler lights_;
    // 射线穿过有界介质时使用的自由程与透射率模型。
    MediumField media_;
    // 为次表面散射提供同一物体上的位置采样。
    SubsurfaceSampler subsurface_;
    // 构造时固定的积分设置。
    IntegratorSettings settings_;
    // 在求交和采样过程检查的共享取消状态。
    CancellationToken cancellation_;
};

// 保留原始 HDR 图像；返回应用曝光和 Reinhard 后的线性显示图，由 PNG 编码器做 sRGB。
Image display_image(const Image& linear, double exposure = 0);

} // namespace minirender
