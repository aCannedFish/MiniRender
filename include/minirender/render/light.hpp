#pragma once

#include <minirender/core/sampling.hpp>
#include <minirender/render/bvh.hpp>

#include <limits>
#include <map>

namespace minirender {

struct EnvironmentSample {
    glm::dvec3 direction{0};
    glm::dvec3 radiance{0};
    double probability = 0; // sr^-1
};

// Latlong 原点为图像左上，北极为 +Y，经度零为 +X，经度增加朝 +Z。
// 采样使用不超过 1024x512 的 mip 估计亮度分布，辐射值始终读取原图。
// 非均匀分布混入 5% 均匀球面密度，覆盖双线性插值和零权重像素附近的支持集。
class EnvironmentLight {
  public:
    explicit EnvironmentLight(const SceneSnapshot& scene, CancellationToken cancellation = {});
    glm::dvec3 evaluate(glm::dvec3 direction) const;
    double pdf(glm::dvec3 direction) const;
    EnvironmentSample sample(glm::dvec2 u) const;
    double luminance_integral() const noexcept; // 估计的积分 ∫ luminance(L) dω，用于选择光源。

  private:
    glm::dvec2 coordinates(glm::dvec3 direction) const;
    glm::dvec3 lookup(glm::dvec2 uv) const;
    glm::dvec3 constant_;
    std::shared_ptr<const Texture> texture_;
    double strength_ = 1;
    double rotation_ = 0;
    double integral_ = 0;
    bool uniform_ = true;
    std::size_t grid_level_ = 0;
    std::vector<double> solid_angles_;
    DiscreteDistribution pixels_;
};

enum class LightKind { area, directional, environment };

// direction 从参考点指向光源；distance 对无限光为正无穷。
// probability 包含光源选择 PMF。方向光为离散事件，另外两类采用 sr^-1。
struct LightSample {
    glm::dvec3 direction{0};
    glm::dvec3 radiance{0};
    double distance = std::numeric_limits<double>::infinity();
    double probability = 0;
    SampleMeasure measure = SampleMeasure::solid_angle;
    LightKind kind = LightKind::area;
    std::size_t emitter_index = 0;
    // 面积光终点及其误差，用于同时偏移阴影线段两端；无限光不使用这些字段。
    glm::dvec3 position{0};
    glm::dvec3 normal{0};
    glm::dvec3 position_error{0};
};

// 发光三角形作为双面漫发射面积光，与现有 emission 预览约定一致。
// 光源选择按估计功率加权。查询结果不包含可见性，积分器负责有限/无限阴影射线。
// 接收 make_snapshot 生成的有效快照；构造完成后只读，可供渲染线程共享。
class LightSampler {
  public:
    explicit LightSampler(std::shared_ptr<const SceneSnapshot> scene,
                          CancellationToken cancellation = {});
    std::optional<LightSample> sample(glm::dvec3 reference, double light_u,
                                      glm::dvec2 surface_u) const;
    double area_pdf(glm::dvec3 reference, const SurfaceHit& hit) const;
    double environment_pdf(glm::dvec3 direction) const;
    const EnvironmentLight& environment() const noexcept;
    std::size_t size() const noexcept;
    double selection_probability(std::size_t emitter) const;

  private:
    struct Area {
        std::array<glm::dvec3, 3> vertices;
        glm::dvec3 normal;
        glm::dvec3 radiance;
        double area;
    };
    struct Directional {
        glm::dvec3 direction;
        glm::dvec3 radiance;
    };
    struct Choice {
        LightKind kind;
        std::size_t index;
    };
    std::shared_ptr<const SceneSnapshot> scene_;
    EnvironmentLight environment_;
    std::vector<Area> areas_;
    std::vector<Directional> directional_;
    std::vector<Choice> choices_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> area_choices_;
    std::optional<std::size_t> environment_choice_;
    DiscreteDistribution selection_;
};

} // namespace minirender
