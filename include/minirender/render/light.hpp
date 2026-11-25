// 场景发光三角形、方向光和环境光的统一采样。

#pragma once

#include <minirender/core/sampling.hpp>
#include <minirender/render/bvh.hpp>

#include <limits>
#include <map>

namespace minirender {

// 一次环境方向采样。方向位于世界空间，radiance 是该方向入射的线性辐亮度。
struct EnvironmentSample {
    glm::dvec3 direction{0};
    glm::dvec3 radiance{0};
    double     probability = 0; // sr^-1
};

// Latlong 原点为图像左上，北极为 +Y，经度零为 +X，经度增加朝 +Z。
// 采样使用不超过 1024x512 的 mip 估计亮度分布，辐射值始终读取原图。
// 非均匀分布混入 5% 均匀球面密度，覆盖双线性插值和零权重像素附近的支持集。
class EnvironmentLight {
  public:
    // 取出环境常量和纹理，构造按亮度与纹素立体角加权的离散分布。
    explicit EnvironmentLight(const SceneSnapshot& scene, CancellationToken cancellation = {});
    // 按世界方向查询常量照明与旋转后环境图的和。
    glm::dvec3 evaluate(glm::dvec3 direction) const;
    // 返回独立采样该方向的立体角密度，单位 sr⁻¹。
    double pdf(glm::dvec3 direction) const;
    // 用一对 [0,1) 样本生成环境方向，同时返回可直接用于估计器的辐亮度与密度。
    EnvironmentSample sample(glm::dvec2 u) const;
    double luminance_integral() const noexcept; // 估计的积分 ∫ luminance(L) dω，用于选择光源。

  private:
    // 把世界方向变换为已考虑环境旋转的经纬纹理坐标。
    glm::dvec2 coordinates(glm::dvec3 direction) const;
    // 在原分辨率环境图上插值，使用经度周期边界。
    glm::dvec3 lookup(glm::dvec2 uv) const;
    // 常量分量与按共享所有权保留的环境图构成照明输入。
    glm::dvec3                     constant_;
    std::shared_ptr<const Texture> texture_;
    // strength_ 缩放纹理分量，rotation_ 为弧度，integral_ 为球面亮度积分。
    double strength_ = 1;
    double rotation_ = 0;
    double integral_ = 0;
    bool   uniform_  = true;
    // 采样分布采用的 mip 层。solid_angles_ 按纬度行保存单个纹素的立体角。
    std::size_t          grid_level_ = 0;
    std::vector<double>  solid_angles_;
    DiscreteDistribution pixels_;
};

enum class LightKind { area, directional, environment };

// direction 从参考点指向光源；distance 对无限光为正无穷。
// probability 包含光源选择 PMF。方向光为离散事件，另外两类采用 sr^-1。
struct LightSample {
    glm::dvec3    direction{0};
    glm::dvec3    radiance{0};
    double        distance      = std::numeric_limits<double>::infinity();
    double        probability   = 0;
    SampleMeasure measure       = SampleMeasure::solid_angle;
    LightKind     kind          = LightKind::area;
    std::size_t   emitter_index = 0;
    // 面积光终点及其误差，用于同时偏移阴影线段两端；无限光不使用这些字段。
    glm::dvec3 position{0};
    glm::dvec3 normal{0};
    glm::dvec3 position_error{0};
};

// 发射光子携带已经除以选灯、位置及方向概率的功率；累计结果再除以发射试验总数。
// 无限光的方向 PDF 对方向光为离散质量，对环境光为 sr^-1；位置 PDF 始终为 m^-2。
struct PhotonEmission {
    Ray        ray;
    glm::dvec3 flux{0};
    LightKind  kind          = LightKind::area;
    double     selection_pmf = 0;
    double     position_pdf  = 0;
    double     direction_pdf = 0;
};

// 发光三角形按材质 double_sided 选择单/双面漫发射，与表面可见性一致。
// 光源选择按估计功率加权。查询结果不包含可见性，积分器负责有限/无限阴影射线。
// 接收 make_snapshot 生成的有效快照；构造完成后只读，可供渲染线程共享。
class LightSampler {
  public:
    // 从快照提取发光三角形和方向光，建立按功率选择光源的分布。
    explicit LightSampler(std::shared_ptr<const SceneSnapshot> scene,
                          CancellationToken                    cancellation = {});
    // 从 reference 向所选光源采样。返回概率已包含选择光源的离散概率。
    std::optional<LightSample> sample(glm::dvec3 reference, double light_u,
                                      glm::dvec2 surface_u) const;
    // 选择光源、发射点与方向，为光子映射生成起始射线和通量。
    std::optional<PhotonEmission> sample_emission(double light_u, glm::dvec2 position_u,
                                                  glm::dvec2 direction_u) const;
    // 将命中三角形上的面积密度换算到着色点所见的立体角，包含光源选择概率。
    double area_pdf(glm::dvec3 reference, const SurfaceHit& hit) const;
    // 环境方向密度乘以选择环境光的概率，供逃逸光路计算 MIS 权重。
    double environment_pdf(glm::dvec3 direction) const;
    // 借用采样器持有的环境查询对象，生命周期与本采样器一致。
    const EnvironmentLight& environment() const noexcept;
    // 返回可供离散分布选择的光源条目数。
    std::size_t size() const noexcept;
    // 返回指定光源条目的概率质量，emitter 是 choices_ 的索引。
    double selection_probability(std::size_t emitter) const;

  private:
    // 世界空间发光三角形的绕序、面积与辐亮度，供采样和密度反查共用。
    struct Area {
        std::array<glm::dvec3, 3> vertices;
        glm::dvec3                normal;
        glm::dvec3                radiance;
        double                    area;
        bool                      double_sided;
    };

    // 无限远方向光在采样坐标系中的方向与线性辐亮度。
    struct Directional {
        glm::dvec3 direction;
        glm::dvec3 radiance;
    };

    // 将统一光源编号映射到对应种类的存储数组。
    struct Choice {
        LightKind   kind;
        std::size_t index;
    };

    EnvironmentLight         environment_;
    std::vector<Area>        areas_;
    std::vector<Directional> directional_;
    std::vector<Choice>      choices_;
    // 按实例与三角形索引反查发光面对应的统一编号。
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> area_choices_;
    std::optional<std::size_t>                                     environment_choice_;
    DiscreteDistribution                                           selection_;
    // 无限远光源发射光子时使用的场景包围球中心与半径。
    glm::dvec3 emission_center_{0};
    double     emission_radius_ = 1;
};

} // namespace minirender
