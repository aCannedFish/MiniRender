// 从三角形外边界建立矩形，裁剪地平线后在单位球面上做边积分。
#include "ltc_tables.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/packing.hpp>
#include <minirender/render/rectangle_light.hpp>
#include <numbers>

namespace minirender {
namespace {
using Polygon = std::array<glm::dvec3, 5>;

// 逆 LTC 变换把拟合分布变为余弦分布。先裁剪 z>=0，跨过地平线的边会增加一个顶点。
double cosine_integral(const std::array<glm::dvec3, 4>& corners, const glm::dmat3& transform) {
    Polygon     clipped{};
    std::size_t count = 0;
    auto        prev  = transform * corners.back();
    for (const auto& corner : corners) {
        const auto cur = transform * corner;
        if ((cur.z > 0) != (prev.z > 0)) {
            clipped[count++] = prev + (cur - prev) * (prev.z / (prev.z - cur.z));
        }
        if (cur.z > 0) {
            clipped[count++] = cur;
        }
        prev = cur;
    }
    double sum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto a = clipped[i], b = clipped[(i + 1) % count];
        const auto cross  = glm::cross(a, b);
        const auto length = glm::length(cross);
        if (length > 1e-16) {
            sum += cross.z * std::atan2(length, glm::dot(a, b)) / length;
        }
    }
    return std::clamp(std::abs(sum) / (2 * std::numbers::pi), 0., 1.);
}

glm::dvec3 lookup(const Image& image, double roughness, double mu) {
    const double x  = std::clamp(roughness, 0., 1.) * 63,
                 y  = std::sqrt(1 - std::clamp(mu, 0., 1.)) * 63;
    const auto   ix = static_cast<std::size_t>(x), iy = static_cast<std::size_t>(y);
    auto         row = [&](std::size_t j) {
        return glm::mix(glm::dvec3(image.at(ix, j)),
                        glm::dvec3(image.at(std::min<std::size_t>(ix + 1, 63), j)), x - double(ix));
    };
    return glm::mix(row(iy), row(std::min<std::size_t>(iy + 1, 63)), y - double(iy));
}

float half_value(const unsigned char* bytes, std::size_t i) {
    const auto word = static_cast<unsigned short>(static_cast<unsigned>(bytes[2 * i]) |
                                                  (static_cast<unsigned>(bytes[2 * i + 1]) << 8U));
    return glm::unpackHalf1x16(word);
}
} // namespace

const std::array<Image, 2>& rectangle_fit() {
    static const auto tables = [] {
        std::array images{Image(64, 64), Image(64, 64)};
        for (std::size_t y = 0; y < 64; ++y) {
            for (std::size_t x = 0; x < 64; ++x) {
                const auto i       = (y * 64 + x) * 4;
                images[0].at(x, y) = {half_value(ltc_data::matrix, i),
                                      half_value(ltc_data::matrix, i + 1),
                                      half_value(ltc_data::matrix, i + 2)};
                images[1].at(x, y) = {half_value(ltc_data::matrix, i + 3),
                                      half_value(ltc_data::magnitude, i),
                                      half_value(ltc_data::magnitude, i + 1)};
            }
        }
        return images;
    }();
    return tables;
}

std::shared_ptr<const MeshData> make_emitter_quad() {
    auto mesh       = std::make_shared<MeshData>();
    mesh->positions = {{-.5F, 0, -.5F}, {-.5F, 0, .5F}, {.5F, 0, .5F}, {.5F, 0, -.5F}};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    for (std::size_t t = 0; t < 2; ++t) {
        MeshTriangle triangle;
        for (std::size_t c = 0; c < 3; ++c) {
            const auto index    = indices[t * 3 + c];
            const auto p        = mesh->positions[index];
            triangle.corners[c] = {index, {0, 1, 0}, {p.x + .5F, p.z + .5F}};
        }
        mesh->triangles.push_back(triangle);
    }
    return mesh;
}

std::optional<RectangleLight> rectangle_light(const RenderInstance& instance) {
    const auto& material  = instance.material;
    const auto& triangles = instance.mesh->triangles;
    if (glm::all(glm::equal(material.emission, glm::vec3(0))) || triangles.size() != 2) {
        return std::nullopt;
    }
    // 两个三角形的公共边在相反方向出现，删去后四条有向外边构成一个闭环。
    using Edge = std::pair<std::uint32_t, std::uint32_t>;
    std::vector<Edge> boundary;
    for (const auto& triangle : triangles) {
        for (std::size_t i = 0; i < 3; ++i) {
            const auto a = triangle.corners[i].position, b = triangle.corners[(i + 1) % 3].position;
            auto       match = std::find(boundary.begin(), boundary.end(), Edge{b, a});
            if (match == boundary.end()) {
                boundary.emplace_back(a, b);
            } else {
                boundary.erase(match);
            }
        }
    }
    if (boundary.size() != 4) {
        return std::nullopt;
    }
    RectangleLight light{{}, material.emission, material.double_sided, instance.object_id};
    auto           next  = boundary.front().first;
    const auto     start = next;
    for (auto& p : light.corners) {
        auto edge =
            std::find_if(boundary.begin(), boundary.end(), [&](auto e) { return e.first == next; });
        if (edge == boundary.end()) {
            return std::nullopt;
        }
        p    = glm::vec3(instance.object_to_world * glm::vec4(instance.mesh->positions[next], 1));
        next = edge->second;
        boundary.erase(edge);
    }
    const auto u = light.corners[1] - light.corners[0], v = light.corners[3] - light.corners[0];
    const auto scale = std::max(glm::length(u), glm::length(v));
    if (next != start || scale <= 1e-8F ||
        std::abs(glm::dot(u, v)) > 1e-4F * glm::length(u) * glm::length(v) ||
        glm::length(light.corners[0] + u + v - light.corners[2]) > 1e-4F * scale) {
        return std::nullopt;
    }
    if (glm::determinant(glm::mat3(instance.object_to_world)) < 0) {
        std::swap(light.corners[1], light.corners[3]);
    }
    return light;
}

RectangleResponse evaluate_rectangle(const RectangleLight& light, glm::dvec3 point,
                                     glm::dvec3 normal, glm::dvec3 outgoing, glm::dvec3 albedo,
                                     double roughness, bool conductor) {
    const auto   n = glm::normalize(normal), v = glm::normalize(outgoing);
    const double mu     = glm::dot(n, v);
    const auto   facing = glm::dot(glm::dvec3(glm::cross(light.corners[1] - light.corners[0],
                                                         light.corners[3] - light.corners[0])),
                                   point - glm::dvec3(light.corners[0]));
    if (mu <= 0 || std::abs(facing) < 1e-14 || (!light.double_sided && facing <= 0)) {
        return {};
    }
    auto tangent = v - n * mu;
    if (glm::dot(tangent, tangent) < 1e-12) {
        tangent = glm::cross(std::abs(n.z) < .9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0), n);
    }
    tangent          = glm::normalize(tangent);
    const auto basis = glm::transpose(glm::dmat3(tangent, glm::cross(n, tangent), n));
    std::array<glm::dvec3, 4> corners;
    for (std::size_t i = 0; i < 4; ++i) {
        corners[i] = basis * (glm::dvec3(light.corners[i]) - point);
    }
    const auto radiance = glm::dvec3(light.radiance);
    if (!conductor) {
        return {radiance * albedo * cosine_integral(corners, glm::dmat3(1)), glm::dvec3(0)};
    }
    const auto& fit = rectangle_fit();
    const auto  a = lookup(fit[0], roughness, mu), b = lookup(fit[1], roughness, mu);
    const auto  inverse = glm::dmat3({a.x, 0, a.y}, {0, 1, 0}, {a.z, 0, b.x});
    return {glm::dvec3(0),
            radiance * (albedo * b.y + (1. - albedo) * b.z) * cosine_integral(corners, inverse)};
}
} // namespace minirender
