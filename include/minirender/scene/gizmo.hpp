#pragma once

#include <minirender/scene/scene.hpp>

#include <optional>

namespace minirender {

enum class GizmoMode { select, translate, rotate };
enum class GizmoHandle { none, x, y, z, screen };

// 屏幕尺寸与鼠标坐标使用逻辑像素，原点在左上角。相机与物体坐标均为世界空间米。
// 操纵器使用世界坐标轴，以物体局部原点为旋转中心；屏幕大小随相机距离保持稳定。
struct GizmoView {
    Camera camera;
    glm::dvec2 size{1};
    double radius_pixels = 85;
    void validate() const;
    std::optional<glm::dvec2> project(glm::dvec3 position) const;
    Ray ray(glm::dvec2 pointer) const;
};

// 线段同时用于显示与命中，world_a/b 为对应世界位置。depth 用于交叉处的前后选择。
struct GizmoSegment {
    glm::dvec2 a{0}, b{0};
    glm::dvec3 world_a{0}, world_b{0};
    GizmoHandle handle = GizmoHandle::none;
    double depth = 0;
};
struct GizmoHit {
    GizmoHandle handle = GizmoHandle::none;
    glm::dvec3 anchor{0};
    double distance_pixels = 0;
};
struct GizmoLayout {
    GizmoMode mode = GizmoMode::select;
    glm::dvec2 center{0};
    glm::dvec3 pivot{0};
    double world_radius = 0;
    bool visible = false;
    std::vector<GizmoSegment> segments;
    GizmoHit hit(glm::dvec2 pointer, double tolerance_pixels = 7) const;
};

GizmoLayout make_gizmo_layout(const GizmoView& view, const Transform& transform, GizmoMode mode);

// 拖拽固定启动时的相机、物体和约束平面。update 返回完整变换，不修改场景或历史。
// 旋转累计跨越 ±pi 的增量，支持连续多圈；近乎侧对相机的旋转环使用投影切向位移。
class GizmoDrag {
  public:
    static std::optional<GizmoDrag> begin(const GizmoView& view, const Transform& transform,
                                          const GizmoLayout& layout, const GizmoHit& hit,
                                          glm::dvec2 pointer);
    Transform update(glm::dvec2 pointer);
    GizmoHandle handle() const noexcept;

  private:
    GizmoView view_;
    Transform original_;
    Transform current_;
    GizmoMode mode_ = GizmoMode::select;
    GizmoHandle handle_ = GizmoHandle::none;
    glm::dvec3 pivot_{0}, axis_{0}, plane_normal_{0}, initial_point_{0}, radial_{0};
    glm::dvec2 pointer_{0}, tangent_{0};
    double last_angle_ = 0, total_angle_ = 0;
    bool plane_rotation_ = false;
};

} // namespace minirender
