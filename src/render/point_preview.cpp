#include <minirender/geometry/halfedge.hpp>
#include <minirender/render/point_preview.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace minirender {

void PointPreviewSettings::validate() const {
    if (!std::isfinite(point_radius) || point_radius < .5F || point_radius > 16 ||
        !std::isfinite(normal_length) || normal_length < 0 || normal_length > 2) {
        throw std::invalid_argument(
            "Point preview requires radius 0.5..16px and normal length 0..2");
    }
}

std::shared_ptr<const PointPreviewData> prepare_point_preview(const PointCloud& cloud,
                                                              const MeshData* reconstruction,
                                                              CancellationToken cancellation,
                                                              PointProgress progress,
                                                              std::size_t maximum_normal_samples) {
    cloud.validate(cancellation);
    if (cloud.positions.size() > 2000000) {
        throw std::length_error("Point preview exceeds its two million point budget");
    }
    std::optional<HalfedgeMesh> topology;
    if (reconstruction) {
        topology.emplace(*reconstruction, cancellation);
    }
    auto result = std::make_shared<PointPreviewData>();
    const auto count = cloud.positions.size();
    const auto normal_count = cloud.normals.empty() ? 0 : std::min(count, maximum_normal_samples);
    const auto total = count * 2 + normal_count + (topology ? topology->edges().size() : 0);
    const auto report = [&](std::size_t done) {
        cancellation.check();
        if (progress) {
            progress(done, total);
        }
        cancellation.check();
    };
    report(0);
    result->minimum = result->maximum = cloud.positions.front();
    for (std::size_t i = 0; i < count; ++i) {
        result->minimum = glm::min(result->minimum, cloud.positions[i]);
        result->maximum = glm::max(result->maximum, cloud.positions[i]);
        if (i % 1024 == 0) {
            report(i + 1);
        }
    }
    const auto extent = glm::dvec3(result->maximum) - glm::dvec3(result->minimum);
    result->center = glm::dvec3(result->minimum) + extent * .5;
    const double longest = std::max({extent.x, extent.y, extent.z});
    result->scale = longest > 0 ? 2 / longest : 1;
    const auto position = [&](glm::vec3 value) {
        const auto normalized = (glm::dvec3(value) - result->center) * result->scale;
        if (!std::isfinite(normalized.x) || !std::isfinite(normalized.y) ||
            !std::isfinite(normalized.z) ||
            glm::any(glm::greaterThan(glm::abs(normalized),
                                      glm::dvec3(std::numeric_limits<float>::max())))) {
            throw std::invalid_argument("Point preview coordinate exceeds finite float storage");
        }
        return glm::vec3(normalized);
    };
    result->has_normals = !cloud.normals.empty();
    result->points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto normal = result->has_normals
                                ? glm::vec3(glm::normalize(glm::dvec3(cloud.normals[i])))
                                : glm::vec3(0);
        result->points.push_back({position(cloud.positions[i]), normal});
        if (i % 1024 == 0) {
            report(count + i + 1);
        }
    }
    result->normal_samples.reserve(normal_count);
    for (std::size_t i = 0; i < normal_count; ++i) {
        result->normal_samples.push_back(result->points[i * count / normal_count]);
        if (i % 1024 == 0) {
            report(count * 2 + i + 1);
        }
    }
    if (topology) {
        result->boundary_edges = topology->statistics().boundary_edges;
        result->boundary_vertices.reserve(result->boundary_edges * 2);
        for (std::size_t i = 0; i < topology->edges().size(); ++i) {
            const auto h = topology->edges()[i].halfedge;
            const auto& halfedges = topology->halfedges();
            if (topology->is_boundary(halfedges[h].twin)) {
                result->boundary_vertices.push_back(
                    {position(reconstruction->positions[halfedges[h].origin]), {0, 0, 0}});
                result->boundary_vertices.push_back(
                    {position(reconstruction->positions[topology->destination(h)]), {0, 0, 0}});
            }
            if (i % 1024 == 0) {
                report(count * 2 + normal_count + i + 1);
            }
        }
    }
    report(total);
    return result;
}

} // namespace minirender
