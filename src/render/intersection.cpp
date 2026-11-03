#include <minirender/render/intersection.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender {
namespace {

// 用 FMA 补偿第二个乘积的舍入，减少细长三角形边函数相减时的消去误差。
double difference_of_products(double a, double b, double c, double d) {
    const double cd = c * d;
    const double correction = std::fma(-c, d, cd);
    return std::fma(a, b, -cd) + correction;
}

} // namespace

void RayInterval::validate() const {
    if (!std::isfinite(minimum) || minimum < 0 || std::isnan(maximum) || maximum < minimum) {
        throw std::invalid_argument("Ray interval must satisfy 0 <= minimum <= maximum");
    }
}

bool Bounds3::empty() const noexcept {
    return glm::any(glm::greaterThan(minimum, maximum));
}
void Bounds3::expand(glm::vec3 point) {
    minimum = glm::min(minimum, point);
    maximum = glm::max(maximum, point);
}
void Bounds3::expand(const Bounds3& bounds) {
    if (!bounds.empty()) {
        expand(bounds.minimum);
        expand(bounds.maximum);
    }
}
double Bounds3::surface_area() const noexcept {
    if (empty()) {
        return 0;
    }
    const auto extent = glm::dvec3(maximum) - glm::dvec3(minimum);
    return 2 * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
}

RayQuery::RayQuery(const Ray& ray) : origin_(ray.origin), direction_(ray.direction) {
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(origin_[axis]) || !std::isfinite(direction_[axis])) {
            throw std::invalid_argument("Ray origin and direction must be finite");
        }
        inverse_[axis] = direction_[axis] == 0 ? 0 : 1 / direction_[axis];
    }
    if (std::abs(glm::dot(direction_, direction_) - 1) > 1e-4) {
        throw std::invalid_argument("Ray direction must have unit length");
    }
    const auto absolute = glm::abs(direction_);
    z_ = absolute.x > absolute.y ? 0 : 1;
    if (absolute.z > absolute[z_]) {
        z_ = 2;
    }
    x_ = (z_ + 1) % 3;
    y_ = (x_ + 1) % 3;
    if (direction_[z_] < 0) {
        std::swap(x_, y_);
    }
    shear_x_ = direction_[x_] / direction_[z_];
    shear_y_ = direction_[y_] / direction_[z_];
    scale_z_ = 1 / direction_[z_];
}

std::optional<double> RayQuery::intersect(const Bounds3& bounds, RayInterval interval) const {
    interval.validate();
    if (bounds.empty()) {
        return std::nullopt;
    }
    double enter = interval.minimum;
    double leave = interval.maximum;
    for (int axis = 0; axis < 3; ++axis) {
        if (direction_[axis] == 0) {
            if (origin_[axis] < bounds.minimum[axis] || origin_[axis] > bounds.maximum[axis]) {
                return std::nullopt;
            }
            continue;
        }
        double near = (static_cast<double>(bounds.minimum[axis]) - origin_[axis]) * inverse_[axis];
        double far = (static_cast<double>(bounds.maximum[axis]) - origin_[axis]) * inverse_[axis];
        if (near > far) {
            std::swap(near, far);
        }
        // 向外舍入 slab 距离，保留恰好接触盒面的射线。
        near = std::nextafter(near, -std::numeric_limits<double>::infinity());
        far = std::nextafter(far, std::numeric_limits<double>::infinity());
        enter = std::max(enter, near);
        leave = std::min(leave, far);
        if (enter > leave) {
            return std::nullopt;
        }
    }
    return enter;
}

std::optional<TriangleHit> RayQuery::intersect(const std::array<glm::vec3, 3>& vertices,
                                               RayInterval interval) const {
    interval.validate();
    std::array<glm::dvec3, 3> p;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto translated = glm::dvec3(vertices[index]) - origin_;
        p[index] = {translated[x_] - shear_x_ * translated[z_],
                    translated[y_] - shear_y_ * translated[z_], translated[z_] * scale_z_};
    }
    // 三个二维边函数分别对应三个重心权重的分子。
    // 共享端点使用相同的变换；反向共享边只改变边函数的符号。
    const double a = difference_of_products(p[1].x, p[2].y, p[1].y, p[2].x);
    const double b = difference_of_products(p[2].x, p[0].y, p[2].y, p[0].x);
    const double c = difference_of_products(p[0].x, p[1].y, p[0].y, p[1].x);
    if ((a < 0 || b < 0 || c < 0) && (a > 0 || b > 0 || c > 0)) {
        return std::nullopt;
    }
    const double determinant = a + b + c;
    if (determinant == 0) {
        return std::nullopt;
    }
    const double distance = (a * p[0].z + b * p[1].z + c * p[2].z) / determinant;
    if (distance < interval.minimum || distance > interval.maximum) {
        return std::nullopt;
    }
    return TriangleHit{distance, glm::vec3(glm::dvec3(a, b, c) / determinant)};
}

} // namespace minirender
