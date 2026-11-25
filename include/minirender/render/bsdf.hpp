// 基础表面材质的求值、方向采样与概率密度计算。

#pragma once

#include <minirender/core/sampling.hpp>
#include <minirender/render/cloth_bsdf.hpp>
#include <minirender/scene/scene.hpp>

#include <optional>

namespace minirender {

// 正交右手坐标系，局部 +Z 为传入的法线。变换不改变向量长度，也不隐式翻转法线。
class ShadingFrame {
  public:
    // 根据法线建立正交切线基，供同一命中点上的求值与采样复用。
    explicit ShadingFrame(glm::dvec3 normal);
    // 将世界方向分别投影到切线、副切线与法线。
    glm::dvec3 to_local(glm::dvec3 world) const noexcept;
    // 用基向量的线性组合将局部方向还原到世界空间。
    glm::dvec3 to_world(glm::dvec3 local) const noexcept;
    // 返回局部正 Z 轴的世界方向。
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
    glm::dvec3      direction{0};
    glm::dvec3      weight{0};
    double          probability = 0;
    SampleMeasure   measure     = SampleMeasure::solid_angle;
    ScatteringEvent event       = ScatteringEvent::diffuse_reflection;
    double          eta         = 1;
};

// 非吸收介质的精确 Fresnel。cosine 为入射侧与法线的非负夹角余弦。
double fresnel_dielectric(double cosine, double eta_i, double eta_t);

// 局部空间 BSDF，wo/wi 均离开表面，+Z 面向 wo，因而 wo.z 必须为正。
// front_face 选择玻璃入/出射 IOR，与局部框架朝向分开保存。
// 金属采用 GGX、Smith G1 乘积及 Schlick F0，alpha=roughness^2。
class Bsdf {
  public:
    // 固定接触面两侧介质的折射率，并将已解码的纹理颜色应用于材质底色。
    explicit Bsdf(const Material& material, bool front_face = true,
                  glm::vec3 texture_color = glm::vec3(1), ClothAxes cloth_axes = {});
    // eval 为 sr^-1，pdf 为立体角密度；delta 材质二者均返回零。
    glm::dvec3 evaluate(glm::dvec3 wo, glm::dvec3 wi) const;
    // 与 sample 使用相同的混合概率。连续方向的密度单位为 sr⁻¹。
    double pdf(glm::dvec3 wo, glm::dvec3 wi) const;
    // component 选择 Fresnel 离散事件，u 用于连续方向；全部输入位于 [0,1)。
    // GGX 的下半球反射为零贡献空事件，不重新抽样或修改其概率密度。
    std::optional<BsdfSample> sample(glm::dvec3 wo, double component, glm::dvec2 u,
                                     TransportMode mode = TransportMode::radiance) const;
    // 指出反射或折射方向是否只以离散事件出现。
    bool is_delta() const noexcept;

  private:
    // 供已验证的单位方向复用，避免对同一方向反复归一化。
    glm::dvec3 evaluate_normalized(glm::dvec3 wo, glm::dvec3 wi) const;
    // 计算单位方向对的连续密度，与公开 pdf 的测度相同。
    double pdf_normalized(glm::dvec3 wo, glm::dvec3 wi) const;
    // 补偿瓣采样选择余弦半球，单次瓣仍按 GGX 可见法线采样。
    double single_probability(double cosine) const;
    // 启用多次散射的导体借用只读能量表，roughness_ 用作查表坐标。
    const Image* energy_    = nullptr;
    double       roughness_ = 0;
    MaterialKind kind_;
    // 纹理调制后的底色。alpha_ 是 GGX 的斜率参数，eta_i_ 与 eta_t_ 对应当前穿越方向。
    glm::dvec3               color_;
    double                   alpha_;
    double                   eta_i_;
    double                   eta_t_;
    std::optional<ClothBsdf> cloth_;
};

} // namespace minirender
