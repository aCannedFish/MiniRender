#pragma once

#include <minirender/scene/scene.hpp>

#include <array>
#include <limits>
#include <optional>

namespace minirender {

// 闭区间 [minimum, maximum]，t 与单位方向射线的世界空间距离一致，单位为米。
// minimum 允许为零，maximum 允许为正无穷；路径积分器自行选择射线起点偏移。
struct RayInterval {
    double minimum = 1e-5;
    double maximum = std::numeric_limits<double>::infinity();
    void validate() const;
};

// 轴对齐包围盒。默认构造为空盒，expand 用于从点或其他盒累积范围。
// 面积使用 double，避免有限 float 坐标的乘积在 SAH 中溢出。
struct Bounds3 {
    glm::vec3 minimum{std::numeric_limits<float>::infinity()};
    glm::vec3 maximum{-std::numeric_limits<float>::infinity()};
    bool empty() const noexcept;
    void expand(glm::vec3 point);
    void expand(const Bounds3& bounds);
    double surface_area() const noexcept;
};

// barycentric 的三个分量依次对应输入三角形的三个顶点，和为一。
struct TriangleHit {
    double distance = 0;
    glm::dvec3 barycentric{0};
};

// 每条射线只准备一次倒数、主方向置换和剪切系数，供 BVH 的多个图元测试复用。
// 构造时拒绝非有限起点、非单位或零方向。对象无可变状态，可在查询中安全复用。
class RayQuery {
  public:
    explicit RayQuery(const Ray& ray);
    // slab 法求盒的进入距离。显式处理零方向，避免 0 * infinity 生成 NaN。
    std::optional<double> intersect(const Bounds3& bounds, RayInterval interval) const;
    // 射线坐标系中的二维边函数，双面求交，精确边界可归属于两侧三角形。
    // 采用 Woop/Benthin/Wald 2013 的置换与剪切形式，算术统一使用 double。
    // 排除数值误差范围内无法与零区分的起点命中。
    std::optional<TriangleHit> intersect(const std::array<glm::vec3, 3>& vertices,
                                         RayInterval interval) const;

  private:
    glm::dvec3 origin_;
    glm::dvec3 direction_;
    glm::dvec3 inverse_;
    int x_ = 0;
    int y_ = 1;
    int z_ = 2;
    double shear_x_ = 0;
    double shear_y_ = 0;
    double scale_z_ = 1;
};

} // namespace minirender
