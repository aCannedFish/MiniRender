#pragma once

#include <minirender/core/sampling.hpp>
#include <minirender/render/cloth_bsdf.hpp>
#include <minirender/scene/scene.hpp>

#include <optional>

namespace minirender {

// 正交右手坐标系，局部 +Z 为传入的法线。变换不改变向量长度，也不隐式翻转法线。
class ShadingFrame {
  public:
    explicit ShadingFrame(glm::dvec3 normal);
    glm::dvec3 to_local(glm::dvec3 world) const noexcept;
    glm::dvec3 to_world(glm::dvec3 local) const noexcept;
    const glm::dvec3& normal() const noexcept;

  private:
    glm::dvec3 tangent_;
    glm::dvec3 bitangent_;
    glm::dvec3 normal_;
};

enum class ScatteringEvent {
    diffuse_reflection,
    glossy_reflection,
    specular_reflection,
    specular_transmission
};
enum class TransportMode { radiance, importance };

// 连续事件的 probability 是 sr^-1，离散事件是无量纲概率质量。
// weight 是已经除以采样概率的吞吐乘子。eta 为 eta_t/eta_i，反射时为 1。
struct BsdfSample {
    glm::dvec3 direction{0};
    glm::dvec3 weight{0};
    double probability = 0;
    SampleMeasure measure = SampleMeasure::solid_angle;
    ScatteringEvent event = ScatteringEvent::diffuse_reflection;
    double eta = 1;
};

// 非吸收介质的精确 Fresnel。cosine 为入射侧与法线的非负夹角余弦。
double fresnel_dielectric(double cosine, double eta_i, double eta_t);

// 局部空间 BSDF，wo/wi 均离开表面，+Z 面向 wo，因而 wo.z 必须为正。
// front_face 选择玻璃入/出射 IOR，与局部框架朝向分开保存。
// 金属采用 GGX、Smith G1 乘积及 Schlick F0，alpha=roughness^2。
class Bsdf {
  public:
    explicit Bsdf(const Material& material, bool front_face = true,
                  glm::vec3 texture_color = glm::vec3(1), ClothAxes cloth_axes = {});
    // eval 为 sr^-1，pdf 为立体角密度；delta 材质二者均返回零。
    glm::dvec3 evaluate(glm::dvec3 wo, glm::dvec3 wi) const;
    double pdf(glm::dvec3 wo, glm::dvec3 wi) const;
    // component 选择 Fresnel 离散事件，u 用于连续方向；全部输入位于 [0,1)。
    // GGX 的下半球反射为零贡献空事件，不重新抽样或修改其概率密度。
    std::optional<BsdfSample> sample(glm::dvec3 wo, double component, glm::dvec2 u,
                                     TransportMode mode = TransportMode::radiance) const;
    bool is_delta() const noexcept;

  private:
    glm::dvec3 evaluate_normalized(glm::dvec3 wo, glm::dvec3 wi) const;
    double pdf_normalized(glm::dvec3 wo, glm::dvec3 wi) const;
    MaterialKind kind_;
    glm::dvec3 color_;
    double alpha_;
    double eta_i_;
    double eta_t_;
    std::optional<ClothBsdf> cloth_;
};

} // namespace minirender
