#include "adaptive_surface.hpp"
#include "height_field.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender::detail {

AdaptiveSurface::AdaptiveSurface(std::shared_ptr<const MeshData> input,
                                 const HalfedgeMesh& topology,
                                 std::shared_ptr<const Texture> height,
                                 const AdaptiveSettings& settings, CancellationToken cancellation)
    : input_(std::move(input)), height_(std::move(height)), settings_(settings),
      cancellation_(std::move(cancellation)),
      hierarchy_(input_, topology, settings.maximum_evaluation_points, cancellation_) {
    if (settings_.displacement && settings_.displacement->scale != 0 && !height_) {
        throw std::invalid_argument("Adaptive displacement requires a height texture");
    }
}

AdaptiveFace AdaptiveSurface::root_face(MeshIndex root) const {
    const auto& corners = input_->triangles.at(root).corners;
    return {{{{corners[0].position, {1, 0, 0}},
              {corners[1].position, {0, 1, 0}},
              {corners[2].position, {0, 0, 1}}}},
            root,
            0};
}

std::array<AdaptiveFace, 4> AdaptiveSurface::children(const AdaptiveFace& parent) {
    cancellation_.check();
    const auto& c = parent.corners;
    const AdaptiveCorner ab{hierarchy_.midpoint(c[0].point, c[1].point, parent.level),
                            (c[0].barycentric + c[1].barycentric) / 2.0};
    const AdaptiveCorner bc{hierarchy_.midpoint(c[1].point, c[2].point, parent.level),
                            (c[1].barycentric + c[2].barycentric) / 2.0};
    const AdaptiveCorner ca{hierarchy_.midpoint(c[2].point, c[0].point, parent.level),
                            (c[2].barycentric + c[0].barycentric) / 2.0};
    return {{{{c[0], ab, ca}, parent.root, parent.level + 1},
             {{c[1], bc, ab}, parent.root, parent.level + 1},
             {{c[2], ca, bc}, parent.root, parent.level + 1},
             {{ab, bc, ca}, parent.root, parent.level + 1}}};
}

glm::dvec2 AdaptiveSurface::parameter_uv(MeshIndex root, glm::dvec3 barycentric) const {
    const auto& c = input_->triangles.at(root).corners;
    return barycentric.x * glm::dvec2(c[0].uv) + barycentric.y * glm::dvec2(c[1].uv) +
           barycentric.z * glm::dvec2(c[2].uv);
}
glm::vec2 AdaptiveSurface::uv(MeshIndex root, glm::dvec3 barycentric) const {
    return glm::vec2(parameter_uv(root, barycentric));
}

glm::dvec3 AdaptiveSurface::position(const AdaptiveCorner& corner, MeshIndex root) {
    cancellation_.check();
    const auto id = corner.point;
    auto candidate =
        glm::dvec3(glm::vec3(hierarchy_.control_position(id, settings_.maximum_level)));
    const bool displaced = settings_.displacement && settings_.displacement->scale != 0;
    if (displaced) {
        auto found = normals_.find(id);
        if (found == normals_.end()) {
            found =
                normals_.emplace(id, hierarchy_.smooth_normal(id, settings_.maximum_level)).first;
        }
        const auto& settings = *settings_.displacement;
        const double value =
            height_value(height_->level(0), parameter_uv(root, corner.barycentric));
        const auto offset = found->second * settings.scale * (value - settings.midlevel);
        maximum_displacement_ = std::max(maximum_displacement_, glm::length(offset));
        candidate = glm::dvec3(glm::vec3(candidate + offset));
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(candidate[axis])) {
            throw std::invalid_argument("Adaptive reference position is not finite");
        }
    }
    const auto [entry, inserted] = positions_.emplace(id, candidate);
    if (!inserted && displaced) {
        const double difference = glm::length(candidate - entry->second);
        maximum_seam_difference_ = std::max(maximum_seam_difference_, difference);
        if (difference > settings_.displacement->seam_tolerance) {
            throw std::invalid_argument("Adaptive displacement is discontinuous across a UV seam");
        }
    }
    return entry->second;
}

AdaptiveSample AdaptiveSurface::sample(MeshIndex root, glm::dvec3 barycentric) {
    cancellation_.check();
    ++queries_;
    auto face = root_face(root);
    auto local = barycentric;
    // 二进制参数点尽早命中层级顶点；其他点仅定位到参照层的一个三角形。
    while (face.level < settings_.maximum_level) {
        for (int corner = 0; corner < 3; ++corner) {
            if (local[corner] >= 1 - 1e-13) {
                const auto p = position(face.corners[static_cast<std::size_t>(corner)], root);
                return {p, hierarchy_.smooth_normal(
                               face.corners[static_cast<std::size_t>(corner)].point,
                               settings_.maximum_level)};
            }
        }
        const auto children_faces = children(face);
        if (local.x >= 0.5) {
            face = children_faces[0];
            local = {2 * local.x - 1, 2 * local.y, 2 * local.z};
        } else if (local.y >= 0.5) {
            face = children_faces[1];
            local = {2 * local.y - 1, 2 * local.z, 2 * local.x};
        } else if (local.z >= 0.5) {
            face = children_faces[2];
            local = {2 * local.z - 1, 2 * local.x, 2 * local.y};
        } else {
            face = children_faces[3];
            local = {1 - 2 * local.z, 1 - 2 * local.x, 1 - 2 * local.y};
        }
    }
    const auto a = position(face.corners[0], root);
    const auto b = position(face.corners[1], root);
    const auto c = position(face.corners[2], root);
    const auto normal = glm::cross(b - a, c - a);
    const auto length = glm::length(normal);
    if (!std::isfinite(length) || length <= 1e-10) {
        throw std::invalid_argument("Adaptive reference contains a degenerate triangle");
    }
    if (settings_.displacement && settings_.displacement->scale != 0) {
        const auto original_a = hierarchy_.control_position(face.corners[0].point, face.level);
        const auto original_b = hierarchy_.control_position(face.corners[1].point, face.level);
        const auto original_c = hierarchy_.control_position(face.corners[2].point, face.level);
        if (glm::dot(normal, glm::cross(original_b - original_a, original_c - original_a)) <= 0) {
            throw std::invalid_argument("Adaptive displacement flips a reference triangle");
        }
    }
    auto position = local.x * a + local.y * b + local.z * c;
    if (settings_.displacement && settings_.displacement->scale != 0) {
        // 参照几何在有限层内插值，高度仍按连续双线性纹理取值。
        // 因而达到最大层数后仍可报告尚未解析的纹理特征。
        glm::dvec3 base(0);
        glm::dvec3 direction(0);
        for (int i = 0; i < 3; ++i) {
            const auto id = face.corners[static_cast<std::size_t>(i)].point;
            base += local[i] * glm::dvec3(glm::vec3(hierarchy_.control_position(id, face.level)));
            direction += local[i] * hierarchy_.smooth_normal(id, face.level);
        }
        const auto& displacement = *settings_.displacement;
        const auto value = height_value(height_->level(0), parameter_uv(root, barycentric));
        const auto length = glm::length(direction);
        if (!std::isfinite(length) || length <= 1e-12) {
            throw std::invalid_argument("Adaptive reference has no interpolated smooth normal");
        }
        position = base + direction / length * displacement.scale * (value - displacement.midlevel);
    }
    return {position, normal / length};
}

void AdaptiveSurface::visit_height_features(const AdaptiveFace& face, double threshold,
                                            const std::function<void(glm::dvec3)>& visit) {
    if (!settings_.displacement || settings_.displacement->scale == 0) {
        return;
    }
    const auto a = parameter_uv(face.root, face.corners[0].barycentric);
    const auto b = parameter_uv(face.root, face.corners[1].barycentric);
    const auto c = parameter_uv(face.root, face.corners[2].barycentric);
    const auto ab = b - a;
    const auto ac = c - a;
    const double determinant = ab.x * ac.y - ab.y * ac.x;
    if (std::abs(determinant) < 1e-20) {
        return;
    }
    const auto& image = height_->level(0);
    const glm::dvec2 extent{image.width(), image.height()};
    const auto low = glm::ceil(glm::min(a, glm::min(b, c)) * extent - 0.5);
    const auto high = glm::floor(glm::max(a, glm::max(b, c)) * extent - 0.5);
    if (glm::any(glm::lessThan(high, low))) {
        return;
    }
    const auto count = (high.x - low.x + 1) * (high.y - low.y + 1);
    const auto budget = settings_.maximum_evaluation_points * 16;
    if (!std::isfinite(count) || count > static_cast<double>(budget - texture_tests_)) {
        throw std::length_error("Adaptive height inspection exceeds its work budget");
    }
    const std::array<double, 3> heights{height_value(image, a), height_value(image, b),
                                        height_value(image, c)};
    for (auto y = static_cast<std::int64_t>(low.y); y <= static_cast<std::int64_t>(high.y); ++y) {
        for (auto x = static_cast<std::int64_t>(low.x); x <= static_cast<std::int64_t>(high.x);
             ++x) {
            if (++texture_tests_ % 1024 == 0) {
                cancellation_.check();
            }
            const glm::dvec2 uv_point{(static_cast<double>(x) + 0.5) / extent.x,
                                      (static_cast<double>(y) + 0.5) / extent.y};
            const auto relative = uv_point - a;
            const double u = (relative.x * ac.y - relative.y * ac.x) / determinant;
            const double v = (ab.x * relative.y - ab.y * relative.x) / determinant;
            if (u < 0 || v < 0 || u + v > 1) {
                continue;
            }
            const double value = height_value(image, uv_point);
            const double linear = (1 - u - v) * heights[0] + u * heights[1] + v * heights[2];
            if (std::abs((value - linear) * settings_.displacement->scale) > threshold) {
                visit((1 - u - v) * face.corners[0].barycentric + u * face.corners[1].barycentric +
                      v * face.corners[2].barycentric);
            }
        }
    }
}

LoopHierarchy& AdaptiveSurface::hierarchy() noexcept {
    return hierarchy_;
}
std::size_t AdaptiveSurface::queries() const noexcept {
    return queries_;
}
double AdaptiveSurface::maximum_displacement() const noexcept {
    return maximum_displacement_;
}
double AdaptiveSurface::maximum_seam_difference() const noexcept {
    return maximum_seam_difference_;
}

} // namespace minirender::detail
