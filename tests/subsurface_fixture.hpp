#pragma once

#include <minirender/render/path_tracer.hpp>

#include <array>
#include <map>
#include <set>
#include <stdexcept>

namespace sss_test {
using namespace minirender;

// 从整格体素边界生成共享顶点、朝外绕序的闭合网格，可构造有凹槽的单个连通体。
inline std::shared_ptr<MeshData> voxel_mesh(const std::set<std::array<int, 3>>& cells) {
    auto mesh = std::make_shared<MeshData>();
    std::map<std::array<int, 3>, std::uint32_t> vertices;
    for (const auto& cell : cells) {
        for (int axis = 0; axis < 3; ++axis) {
            for (const int sign : {-1, 1}) {
                auto neighbor = cell;
                neighbor[std::size_t(axis)] += sign;
                if (cells.contains(neighbor)) {
                    continue;
                }
                std::array<std::uint32_t, 4> quad{};
                constexpr std::array<std::array<int, 2>, 4> square{
                    {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
                for (std::size_t c = 0; c < 4; ++c) {
                    auto point = cell;
                    for (auto& value : point) {
                        value *= 2;
                    }
                    point[std::size_t(axis)] += sign;
                    point[std::size_t((axis + 1) % 3)] += square[c][0];
                    point[std::size_t((axis + 2) % 3)] += square[c][1];
                    const auto [entry, inserted] =
                        vertices.emplace(point, std::uint32_t(vertices.size()));
                    if (inserted) {
                        mesh->positions.emplace_back(float(point[0]) / 2, float(point[1]) / 2,
                                                     float(point[2]) / 2);
                    }
                    quad[c] = entry->second;
                }
                if (sign < 0) {
                    std::swap(quad[1], quad[3]);
                }
                glm::vec3 normal(0);
                normal[axis] = float(sign);
                for (const auto corners : {std::array{0, 1, 2}, std::array{0, 2, 3}}) {
                    MeshTriangle triangle;
                    for (std::size_t c = 0; c < 3; ++c) {
                        triangle.corners[c] = {quad[std::size_t(corners[c])], normal, {}};
                    }
                    mesh->triangles.push_back(triangle);
                }
            }
        }
    }
    return mesh;
}

inline SceneDocument scene(bool concave = false) {
    SceneDocument result;
    result.environment = glm::vec3(1);
    result.camera.position = {0, 0, 3};
    result.camera.target = {0, 0, 0};
    result.camera.near_plane = .01F;
    Material material;
    material.id = "sss";
    material.kind = MaterialKind::subsurface;
    material.ior = 1.3F;
    material.subsurface = std::make_shared<const SubsurfaceParameters>(
        SubsurfaceParameters{{.1, .2, .3}, {1, 2, 3}, 1});
    result.materials.push_back(material);
    const std::set<std::array<int, 3>> cells =
        concave ? std::set<std::array<int, 3>>{{-1, 0, -1}, {0, 0, -1}, {1, 0, -1}, {-1, 0, 0},
                                               {1, 0, 0},   {-1, 0, 1}, {1, 0, 1}}
                : std::set<std::array<int, 3>>{{0, 0, 0}};
    result.meshes.push_back({"mesh", {}, voxel_mesh(cells)});
    result.objects.push_back({"object", "SSS", "mesh", "sss", {}});
    return result;
}

inline SurfaceHit entry(const SceneBvh& bvh, glm::dvec3 local = {0, 0, .5}) {
    const auto& object = bvh.scene().instances[0];
    const glm::dvec3 point(object.object_to_world * glm::vec4(glm::vec3(local), 1));
    const auto normal = glm::normalize(glm::dmat3(object.normal_to_world) *
                                       (local.y == .5 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1)));
    const auto hit = bvh.intersect({point + 5.0 * normal, -normal});
    if (!hit) {
        throw std::runtime_error("Subsurface fixture entry ray missed");
    }
    return *hit;
}

inline std::array<glm::dvec3, 3> world_triangle(const RenderInstance& object,
                                                const MeshTriangle& face) {
    std::array<glm::dvec3, 3> points;
    for (std::size_t c = 0; c < 3; ++c) {
        points[c] = glm::vec3(object.object_to_world *
                              glm::vec4(object.mesh->positions[face.corners[c].position], 1));
    }
    return points;
}
} // namespace sss_test
