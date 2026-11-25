// 布料散射模型的方向求值、混合采样与概率密度。

#pragma once

#include <minirender/scene/cloth_material.hpp>

#include <glm/vec2.hpp>
#include <optional>

namespace minirender {

// 局部 +Z 为宏观法线。u/v 由 UV 梯度变换到该坐标系，可以保留变形后的剪切夹角。
struct ClothAxes {
    glm::dvec3 u{1, 0, 0};
    glm::dvec3 v{0, 1, 0};
};

// Sadeghi 微圆柱反射模型。response 返回单位入射辐射的出射响应，evaluate=response/wi.z，
// 与引擎表面 BSDF 的 sr^-1 契约一致。纹理和底色仅调节内部散射，界面反射保持白色。
class ClothBsdf final {
  public:
    // 固定经纬纱外观、折射率与染色项，axes 使用当前交点的局部坐标。
    ClothBsdf(std::shared_ptr<const ClothAppearance> appearance, double ior,
              glm::dvec3 dye = glm::dvec3(1), ClothAxes axes = {});
    // 返回含入射余弦的响应，供直接光照和采样权重使用。
    glm::dvec3 response(glm::dvec3 wo, glm::dvec3 wi) const;
    // 返回局部方向对的 BSDF，供积分器按自身测度组合。
    glm::dvec3 evaluate(glm::dvec3 wo, glm::dvec3 wi) const;
    // 查询与经纬混合方向采样相匹配的立体角密度。
    double pdf(glm::dvec3 wo, glm::dvec3 wi) const;
    // 20% 宏观余弦与 80% 纱线采样混合。背半球样本保留为空事件，pdf 不重新归一。
    // component 选择混合分量，u 生成入射方向 wi，空结果表示零贡献。
    std::optional<glm::dvec3> sample(glm::dvec3 wo, double component, glm::dvec2 u) const;

  private:
    glm::dvec3 response_normalized(glm::dvec3 wo, glm::dvec3 wi) const;
    double pdf_normalized(glm::dvec3 wo, glm::dvec3 wi) const;
    std::shared_ptr<const ClothAppearance> appearance_;
    double                                 ior_;
    glm::dvec3                             dye_;
    // 两组纱线在着色坐标系中的轴向，按经、纬顺序存储。
    std::array<glm::dvec3, 2> axes_;
};

} // namespace minirender
