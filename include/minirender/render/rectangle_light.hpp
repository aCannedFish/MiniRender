// 矩形发光面的几何识别、LTC 拟合表和多边形积分。单位采用米与线性辐亮度。
#pragma once
#include <minirender/scene/scene.hpp>
#include <optional>

namespace minirender {
// 创建局部 +Y 为正面的单位矩形，用普通网格与材质编辑面光源。
std::shared_ptr<const MeshData> make_emitter_quad();
// 从两三角形构成的矩形发光实例提取有向世界坐标边界。非矩形返回空。
std::optional<RectangleLight> rectangle_light(const RenderInstance& instance);

// 用单位余弦或拟合的 GGX 变换积分独立的漫反射和镜面项。
struct RectangleResponse {
    glm::dvec3 diffuse{0}, specular{0};
};

// point、normal、outgoing 位于世界空间，outgoing 从表面指向观察者。
// 漫反射使用余弦边积分，导体使用粗糙度和观察余弦选取 LTC 拟合；背面判断沿用灯具的绕序。
RectangleResponse evaluate_rectangle(const RectangleLight& light, glm::dvec3 point,
                                     glm::dvec3 normal, glm::dvec3 outgoing, glm::dvec3 albedo,
                                     double roughness, bool conductor);
// 两张表来自 Heitz 等的 64×64 GGX 拟合，第一张存逆矩阵前三项，第二张存第四项和 Fresnel 系数。
const std::array<Image, 2>& rectangle_fit();
} // namespace minirender
