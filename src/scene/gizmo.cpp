#include <minirender/scene/gizmo.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {

bool finite(glm::dvec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
glm::dvec3 axis(GizmoHandle handle) {
    switch (handle) {
    case GizmoHandle::x:
        return {1, 0, 0};
    case GizmoHandle::y:
        return {0, 1, 0};
    case GizmoHandle::z:
        return {0, 0, 1};
    default:
        return {0, 0, 0};
    }
}

std::optional<glm::dvec3> plane_point(const Ray& ray, glm::dvec3 point, glm::dvec3 normal) {
    const auto denominator = glm::dot(ray.direction, normal);
    if (std::abs(denominator) < 1e-7) {
        return {};
    }
    const auto distance = glm::dot(point - ray.origin, normal) / denominator;
    if (distance <= 0 || !std::isfinite(distance)) {
        return {};
    }
    return ray.origin + distance * ray.direction;
}

// 变换的旋转约定为 Rz*Ry*Rx。奇异姿态令 z=0，用仍可辨识的 x/z 组合恢复同一矩阵。
// 保存的欧拉角可以跨分支变化，实际旋转矩阵保持连续。
glm::vec3 euler_zyx(const glm::dmat4& rotation) {
    const auto y = std::asin(std::clamp(-rotation[0][2], -1.0, 1.0));
    double x = 0, z = 0;
    if (std::abs(std::cos(y)) > 1e-8) {
        x = std::atan2(rotation[1][2], rotation[2][2]);
        z = std::atan2(rotation[0][1], rotation[0][0]);
    } else {
        x = std::atan2(-rotation[2][1], rotation[1][1]);
    }
    return glm::vec3(glm::dvec3(x, y, z) * (180.0 / std::numbers::pi));
}

glm::dmat4 rotation_matrix(const Transform& transform) {
    const auto r = glm::radians(glm::dvec3(transform.rotation_degrees));
    return glm::rotate(glm::dmat4(1), r.z, glm::dvec3(0, 0, 1)) *
           glm::rotate(glm::dmat4(1), r.y, glm::dvec3(0, 1, 0)) *
           glm::rotate(glm::dmat4(1), r.x, glm::dvec3(1, 0, 0));
}

} // namespace

void GizmoView::validate() const {
    camera.validate();
    if (!finite(size) || size.x <= 0 || size.y <= 0 || !std::isfinite(radius_pixels) ||
        radius_pixels < 8 || radius_pixels > 512) {
        throw std::invalid_argument(
            "Gizmo requires a positive viewport size and an 8..512 pixel radius");
    }
}

std::optional<glm::dvec2> GizmoView::project(glm::dvec3 position) const {
    validate();
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        throw std::invalid_argument("Gizmo position must be finite");
    }
    const auto matrix =
        camera.projection_matrix(static_cast<float>(size.x / size.y)) * camera.view_matrix();
    const auto clip = glm::dmat4(matrix) * glm::dvec4(position, 1);
    if (clip.w <= 0 || clip.z < 0 || clip.z > clip.w) {
        return {};
    }
    return (glm::dvec2(clip) / clip.w * 0.5 + 0.5) * size;
}

Ray GizmoView::ray(glm::dvec2 pointer) const {
    validate();
    if (!finite(pointer)) {
        throw std::invalid_argument("Gizmo pointer must be finite");
    }
    return camera.generate_ray(pointer / size, size.x / size.y);
}

GizmoLayout make_gizmo_layout(const GizmoView& view, const Transform& transform, GizmoMode mode) {
    view.validate();
    transform.validate();
    GizmoLayout layout;
    layout.mode = mode;
    layout.pivot = glm::dvec3(transform.translation);
    const auto center = view.project(layout.pivot);
    if (!center || mode == GizmoMode::select) {
        return layout;
    }
    layout.center = *center;
    layout.visible = true;
    const auto forward =
        glm::normalize(glm::dvec3(view.camera.target) - glm::dvec3(view.camera.position));
    const auto distance = glm::dot(layout.pivot - glm::dvec3(view.camera.position), forward);
    layout.world_radius =
        2 * distance *
        std::tan(glm::radians(static_cast<double>(view.camera.vertical_fov_degrees)) / 2) *
        view.radius_pixels / view.size.y;
    const auto segment = [&](glm::dvec3 a, glm::dvec3 b, GizmoHandle handle) {
        const auto pa = view.project(a), pb = view.project(b);
        if (pa && pb && glm::length(*pb - *pa) > 1e-6) {
            layout.segments.push_back(
                {*pa, *pb, a, b, handle,
                 glm::dot((a + b) / 2.0 - glm::dvec3(view.camera.position), forward)});
        }
    };
    for (const auto handle : {GizmoHandle::x, GizmoHandle::y, GizmoHandle::z}) {
        const auto direction = axis(handle);
        if (mode == GizmoMode::translate) {
            segment(layout.pivot + direction * layout.world_radius * 0.16,
                    layout.pivot + direction * layout.world_radius, handle);
        } else {
            const auto basis = handle == GizmoHandle::x ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            const auto other = glm::cross(direction, basis);
            constexpr int count = 96;
            for (int i = 0; i < count; ++i) {
                const auto a = i * 2 * std::numbers::pi / count;
                const auto b = (i + 1) * 2 * std::numbers::pi / count;
                segment(layout.pivot +
                            layout.world_radius * (std::cos(a) * basis + std::sin(a) * other),
                        layout.pivot +
                            layout.world_radius * (std::cos(b) * basis + std::sin(b) * other),
                        handle);
            }
        }
    }
    return layout;
}

GizmoHit GizmoLayout::hit(glm::dvec2 pointer, double tolerance_pixels) const {
    if (!finite(pointer) || !std::isfinite(tolerance_pixels) || tolerance_pixels <= 0) {
        throw std::invalid_argument("Gizmo hit requires a finite pointer and positive tolerance");
    }
    if (!visible) {
        return {};
    }
    if (mode == GizmoMode::translate && glm::length(pointer - center) <= tolerance_pixels) {
        return {GizmoHandle::screen, pivot, glm::length(pointer - center)};
    }
    GizmoHit best;
    best.distance_pixels = tolerance_pixels;
    double best_depth = std::numeric_limits<double>::infinity();
    for (const auto& segment : segments) {
        const auto edge = segment.b - segment.a;
        const auto t =
            std::clamp(glm::dot(pointer - segment.a, edge) / glm::dot(edge, edge), 0.0, 1.0);
        const auto distance = glm::length(pointer - segment.a - t * edge);
        if (distance < best.distance_pixels - 0.25 ||
            (distance <= best.distance_pixels &&
             std::abs(distance - best.distance_pixels) <= 0.25 && segment.depth < best_depth)) {
            best = {segment.handle, segment.world_a + t * (segment.world_b - segment.world_a),
                    distance};
            best_depth = segment.depth;
        }
    }
    return best;
}

std::optional<GizmoDrag> GizmoDrag::begin(const GizmoView& view, const Transform& transform,
                                          const GizmoLayout& layout, const GizmoHit& hit,
                                          glm::dvec2 pointer) {
    view.validate();
    transform.validate();
    if (!layout.visible || hit.handle == GizmoHandle::none || layout.mode == GizmoMode::select) {
        return {};
    }
    GizmoDrag drag;
    drag.view_ = view;
    drag.original_ = transform;
    drag.current_ = transform;
    drag.mode_ = layout.mode;
    drag.handle_ = hit.handle;
    drag.pivot_ = layout.pivot;
    drag.axis_ = axis(hit.handle);
    drag.pointer_ = pointer;
    const auto forward =
        glm::normalize(glm::dvec3(view.camera.target) - glm::dvec3(view.camera.position));
    if (layout.mode == GizmoMode::translate) {
        auto normal = hit.handle == GizmoHandle::screen
                          ? forward
                          : forward - drag.axis_ * glm::dot(forward, drag.axis_);
        if (glm::length(normal) < 1e-8) {
            return {};
        }
        drag.plane_normal_ = glm::normalize(normal);
        const auto point = plane_point(view.ray(pointer), drag.pivot_, drag.plane_normal_);
        if (!point) {
            return {};
        }
        drag.initial_point_ = *point;
    } else {
        if (hit.handle == GizmoHandle::screen || glm::length(hit.anchor - drag.pivot_) < 1e-12) {
            return {};
        }
        drag.radial_ = glm::normalize(hit.anchor - drag.pivot_);
        const auto point = plane_point(view.ray(pointer), drag.pivot_, drag.axis_);
        if (point && std::abs(glm::dot(view.ray(pointer).direction, drag.axis_)) > 0.05 &&
            glm::length(*point - drag.pivot_) > 1e-10) {
            drag.radial_ = glm::normalize(*point - drag.pivot_);
            drag.plane_rotation_ = true;
        }
        const auto tangent = glm::cross(drag.axis_, drag.radial_);
        const auto a = view.project(drag.pivot_ + layout.world_radius * drag.radial_);
        const auto b =
            view.project(drag.pivot_ + layout.world_radius * (drag.radial_ + tangent * 0.01));
        if (!a || !b || glm::length(*b - *a) < 1e-6) {
            return {};
        }
        drag.tangent_ = (*b - *a) / 0.01; // 每弧度的屏幕位移。
    }
    return drag;
}

Transform GizmoDrag::update(glm::dvec2 pointer) {
    if (!finite(pointer)) {
        throw std::invalid_argument("Gizmo pointer must be finite");
    }
    auto result = original_;
    if (mode_ == GizmoMode::translate) {
        const auto point = plane_point(view_.ray(pointer), pivot_, plane_normal_);
        if (!point) {
            return current_;
        }
        auto delta = *point - initial_point_;
        if (handle_ != GizmoHandle::screen) {
            delta = axis_ * glm::dot(delta, axis_);
        }
        result.translation = glm::vec3(glm::dvec3(original_.translation) + delta);
    } else {
        if (plane_rotation_) {
            const auto point = plane_point(view_.ray(pointer), pivot_, axis_);
            if (!point || glm::length(*point - pivot_) < 1e-10) {
                return current_;
            }
            const auto current = glm::normalize(*point - pivot_);
            const auto angle = std::atan2(glm::dot(axis_, glm::cross(radial_, current)),
                                          glm::dot(radial_, current));
            total_angle_ += std::remainder(angle - last_angle_, 2 * std::numbers::pi);
            last_angle_ = angle;
        } else {
            total_angle_ = glm::dot(pointer - pointer_, tangent_) / glm::dot(tangent_, tangent_);
        }
        if (std::abs(total_angle_) > 1e-14) {
            const auto rotation =
                glm::rotate(glm::dmat4(1), total_angle_, axis_) * rotation_matrix(original_);
            result.rotation_degrees = euler_zyx(rotation);
        }
    }
    result.validate();
    current_ = result;
    return current_;
}

GizmoHandle GizmoDrag::handle() const noexcept {
    return handle_;
}

} // namespace minirender
