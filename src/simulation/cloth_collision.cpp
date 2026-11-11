#include "cloth_collision.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender::detail {
namespace {

// 去掉入射法向速度，以法向冲量限制切向速度损失。
void respond(glm::dvec3& velocity, glm::dvec3 normal, double friction) {
    const double vn = glm::dot(velocity, normal);
    if (vn >= 0) {
        return;
    }
    const auto tangent = velocity - vn * normal;
    const double length = glm::length(tangent);
    const double scale = length > 0 ? std::max(0.0, 1 + friction * vn / length) : 0;
    velocity = tangent * scale;
}

glm::dvec3 radial(glm::dvec3 delta, glm::dvec3 velocity) {
    const double length = glm::length(delta);
    if (length > 1e-14) {
        return delta / length;
    }
    const double speed = glm::length(velocity);
    return speed > 1e-14 ? -velocity / speed : glm::dvec3(0, 1, 0);
}

struct Contact {
    double fraction = 2;
    glm::dvec3 normal{0};
    double friction = 0;
};

Contact first_contact(glm::dvec3 start, glm::dvec3 motion, double thickness,
                      const ClothEnvironment& environment) {
    Contact result;
    for (const auto& plane : environment.planes) {
        const double distance = glm::dot(start - plane.point, plane.normal) - thickness;
        const double change = glm::dot(motion, plane.normal);
        if (change < 0) {
            const double fraction = -distance / change;
            if (fraction >= 0 && fraction <= 1 && fraction < result.fraction) {
                result = {fraction, plane.normal, plane.friction};
            }
        }
    }
    const double a = glm::dot(motion, motion);
    if (a == 0) {
        return result;
    }
    for (const auto& sphere : environment.spheres) {
        const double radius = sphere.radius + thickness;
        const auto delta = start - sphere.center;
        const double b = glm::dot(delta, motion);
        const double c = glm::dot(delta, delta) - radius * radius;
        const double discriminant = b * b - a * c;
        if (b < 0 && c >= 0 && discriminant >= 0) {
            // 入口根用乘积关系计算，避免接触起点附近相减丢失精度。
            const double fraction = c / (-b + std::sqrt(discriminant));
            if (fraction >= 0 && fraction <= 1 && fraction < result.fraction) {
                result = {fraction, radial(delta + fraction * motion, motion), sphere.friction};
            }
        }
    }
    return result;
}

} // namespace

ClothContacts move_cloth_particle(glm::dvec3& position, glm::dvec3& velocity, double seconds,
                                  double thickness, const ClothEnvironment& environment,
                                  CancellationToken cancellation) {
    ClothContacts result;
    const double offset = std::max(1e-9, thickness * 1e-5);
    bool projected = false;
    for (int iteration = 0; iteration < 16; ++iteration) {
        cancellation.check();
        projected = false;
        for (const auto& plane : environment.planes) {
            const double distance = glm::dot(position - plane.point, plane.normal);
            if (distance < thickness) {
                position += plane.normal * (thickness + offset - distance);
                respond(velocity, plane.normal, plane.friction);
                ++result.count;
                projected = true;
            }
        }
        for (const auto& sphere : environment.spheres) {
            const double radius = sphere.radius + thickness;
            if (glm::length(position - sphere.center) < radius) {
                const auto normal = radial(position - sphere.center, velocity);
                position = sphere.center + normal * (radius + offset);
                respond(velocity, normal, sphere.friction);
                ++result.count;
                projected = true;
            }
        }
        if (!projected) {
            break;
        }
    }
    if (projected) {
        for (const auto& plane : environment.planes) {
            if (glm::dot(position - plane.point, plane.normal) < thickness - 1e-10) {
                throw std::runtime_error("Cloth contact projection did not converge");
            }
        }
        for (const auto& sphere : environment.spheres) {
            if (glm::length(position - sphere.center) < sphere.radius + thickness - 1e-10) {
                throw std::runtime_error("Cloth contact projection did not converge");
            }
        }
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        cancellation.check();
        const auto motion = velocity * seconds;
        const auto contact = first_contact(position, motion, thickness, environment);
        if (contact.fraction > 1) {
            position += motion;
            return result;
        }
        position += motion * contact.fraction + contact.normal * offset;
        respond(velocity, contact.normal, contact.friction);
        seconds *= 1 - contact.fraction;
        ++result.count;
        if (seconds <= 1e-14) {
            return result;
        }
    }
    velocity = glm::dvec3(0);
    result.limited = true;
    return result;
}

} // namespace minirender::detail
