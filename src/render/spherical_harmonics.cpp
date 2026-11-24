// real SH 的环境投影与旋转。立体角归一化和基函数顺序由所有 PRT 输入共用。
#include <cmath>
#include <glm/geometric.hpp>
#include <minirender/render/spherical_harmonics.hpp>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {
constexpr double pi = std::numbers::pi;

void accumulate(ShLight& light, glm::dvec3 dir, glm::dvec3 color, double omega) {
    const auto basis = sh_basis(dir);
    for (std::size_t k = 0; k < 9; ++k) {
        light[k] += color * (omega * basis[k]);
    }
}

glm::dvec3 cube_direction(std::size_t face, double u, double v) {
    switch (face) {
    case 0:
        return {1, -v, -u};
    case 1:
        return {-1, -v, u};
    case 2:
        return {u, 1, v};
    case 3:
        return {u, -1, -v};
    case 4:
        return {u, -v, 1};
    default:
        return {-u, -v, -1};
    }
}

double solid_angle(double x, double y) {
    return std::atan2(x * y, std::sqrt(x * x + y * y + 1));
}
} // namespace

ShBasis sh_basis(glm::dvec3 dir) {
    const auto   d = glm::normalize(dir);
    const double x = d.x, y = d.y, z = d.z;
    return {.28209479177387814,
            .4886025119029199 * y,
            .4886025119029199 * z,
            .4886025119029199 * x,
            1.0925484305920792 * x * y,
            1.0925484305920792 * y * z,
            .31539156525252005 * (3 * z * z - 1),
            1.0925484305920792 * x * z,
            .5462742152960396 * (x * x - y * y)};
}

ShLight constant_environment(glm::dvec3 radiance) {
    ShLight result{};
    result[0] = std::sqrt(4 * pi) * radiance;
    return result;
}

ShLight project_environment(const Image& image) {
    ShLight result{};
    for (std::size_t y = 0; y < image.height(); ++y) {
        const double t0    = pi * double(y) / double(image.height()),
                     t1    = pi * double(y + 1) / double(image.height());
        const double theta = .5 * (t0 + t1),
                     omega = (std::cos(t0) - std::cos(t1)) * 2 * pi / double(image.width());
        for (std::size_t x = 0; x < image.width(); ++x) {
            const double phi = 2 * pi * (double(x) + .5) / double(image.width());
            accumulate(
                result,
                {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)},
                image.at(x, y), omega);
        }
    }
    return result;
}

ShLight project_cubemap(const std::array<Image, 6>& faces) {
    ShLight    result{};
    const auto n = faces[0].width();
    for (std::size_t face = 0; face < 6; ++face) {
        if (faces[face].width() != n || faces[face].height() != n) {
            throw std::invalid_argument("Cube faces must be equally sized squares");
        }
        for (std::size_t y = 0; y < n; ++y) {
            for (std::size_t x = 0; x < n; ++x) {
                const double u0 = 2. * double(x) / double(n) - 1,
                             v0 = 2. * double(y) / double(n) - 1, step = 2. / double(n);
                const double u1 = u0 + step, v1 = v0 + step;
                const double omega = solid_angle(u1, v1) - solid_angle(u0, v1) -
                                     solid_angle(u1, v0) + solid_angle(u0, v0);
                accumulate(result, cube_direction(face, (u0 + u1) * .5, (v0 + v1) * .5),
                           faces[face].at(x, y), omega);
            }
        }
    }
    return result;
}

ShLight rotate_environment(const ShLight& source, double radians) {
    // 把 l=1 当作线性函数，把 l=2 当作无迹二次型。正交变换相当于对基函数换元；
    // 五点 Gauss–Legendre 与均匀方位角对这些最高四次的投影乘积精确积分。
    ShLight                     rotated{};
    const std::array<double, 5> nodes{-.906179845938664, -.538469310105683, 0, .538469310105683,
                                      .906179845938664};
    const std::array<double, 5> weights{.236926885056189, .478628670499366, .568888888888889,
                                        .478628670499366, .236926885056189};
    const auto                  c = std::cos(radians), s = std::sin(radians);
    for (std::size_t j = 0; j < nodes.size(); ++j) {
        const auto y = nodes[j], r = std::sqrt(1 - y * y);
        for (int i = 0; i < 12; ++i) {
            const double     phi = 2 * pi * i / 12;
            const glm::dvec3 d{r * std::cos(phi), y, r * std::sin(phi)};
            const auto       basis = sh_basis({c * d.x - s * d.z, d.y, s * d.x + c * d.z});
            glm::dvec3       value(0);
            for (std::size_t k = 0; k < 9; ++k) {
                value += source[k] * basis[k];
            }
            accumulate(rotated, d, value, weights[j] * 2 * pi / 12);
        }
    }
    return rotated;
}
} // namespace minirender
