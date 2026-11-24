// 实球谐前三个频带的基函数与环境光投影。系数均为线性 RGB，方向使用世界坐标。
#pragma once
#include <array>
#include <minirender/core/image.hpp>

namespace minirender {
using ShBasis = std::array<double, 9>;
using ShLight = std::array<glm::dvec3, 9>;
// 按 l=0,1,2，m 从负到正排列。输入方向会归一化。
ShBasis sh_basis(glm::dvec3 direction);
// 读取经纬图的像素中心与精确球面立体角；常量环境单独投影到零阶。
ShLight project_environment(const Image& image);
ShLight constant_environment(glm::dvec3 radiance);
// 面序为 +X,-X,+Y,-Y,+Z,-Z，使用立方体纹素的四角立体角积分。
ShLight project_cubemap(const std::array<Image, 6>& faces);
// 环境绕世界 Y 轴旋转，直接变换九个系数。
ShLight rotate_environment(const ShLight& source, double radians);
} // namespace minirender
