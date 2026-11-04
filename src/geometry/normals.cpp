#include <minirender/geometry/normals.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

namespace minirender {
namespace {

// 两个公共入口共用同一计算过程；调用方决定总工作量是否包含面角写回。
std::vector<glm::vec3> calculate(const MeshData& mesh, CancellationToken cancellation,
                                 const MeshNormalProgress& progress, std::size_t total) {
    const auto report = [&](std::size_t completed) {
        cancellation.check();
        if (progress) {
            progress(completed, total);
        }
        cancellation.check();
    };
    report(0);
    std::size_t completed = 0;
    const auto advance = [&] {
        if (++completed % 1024 == 0) {
            report(completed);
        }
    };
    std::vector<glm::dvec3> sums(mesh.positions.size(), glm::dvec3(0));
    std::vector<double> areas(mesh.positions.size(), 0);
    for (std::size_t face = 0; face < mesh.triangles.size(); ++face) {
        const auto& corners = mesh.triangles[face].corners;
        const glm::dvec3 a = mesh.positions.at(corners[0].position);
        const glm::dvec3 b = mesh.positions.at(corners[1].position);
        const glm::dvec3 c = mesh.positions.at(corners[2].position);
        const auto area = glm::cross(b - a, c - a);
        const auto squared = glm::dot(area, area);
        if (!std::isfinite(squared) || squared <= 1e-20) {
            throw std::invalid_argument("Face " + std::to_string(face) + " is degenerate");
        }
        const auto length = std::sqrt(squared);
        for (const auto& corner : corners) {
            sums[corner.position] += area;
            areas[corner.position] += length;
        }
        advance();
    }
    std::vector<glm::vec3> normals(mesh.positions.size(), glm::vec3(0));
    for (std::size_t vertex = 0; vertex < sums.size(); ++vertex) {
        if (areas[vertex] > 0) {
            const auto length = glm::length(sums[vertex]);
            if (length <= 1e-12 * areas[vertex]) {
                throw std::invalid_argument("Vertex " + std::to_string(vertex) +
                                            " has no stable smooth normal");
            }
            normals[vertex] = glm::vec3(sums[vertex] / length);
        }
        advance();
    }
    report(completed);
    return normals;
}

} // namespace

std::vector<glm::vec3> smooth_vertex_normals(const MeshData& mesh, CancellationToken cancellation,
                                             MeshNormalProgress progress) {
    return calculate(mesh, cancellation, progress, mesh.triangles.size() + mesh.positions.size());
}

void recompute_smooth_normals(MeshData& mesh, CancellationToken cancellation,
                              MeshNormalProgress progress) {
    const auto total = mesh.triangles.size() * 4 + mesh.positions.size();
    const auto normals = calculate(mesh, cancellation, progress, total);
    auto completed = mesh.triangles.size() + mesh.positions.size();
    for (auto& triangle : mesh.triangles) {
        for (auto& corner : triangle.corners) {
            corner.normal = normals[corner.position];
            if (++completed % 1024 == 0) {
                cancellation.check();
                if (progress) {
                    progress(completed, total);
                }
                cancellation.check();
            }
        }
    }
    cancellation.check();
    if (progress) {
        progress(completed, total);
    }
    cancellation.check();
}

} // namespace minirender
