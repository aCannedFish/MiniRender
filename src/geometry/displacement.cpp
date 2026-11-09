#include "height_field.hpp"
#include <minirender/geometry/displacement.hpp>
#include <minirender/geometry/normals.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace minirender {

void DisplacementSettings::validate() const {
    if (!std::isfinite(scale) || !std::isfinite(midlevel) || !std::isfinite(seam_tolerance) ||
        seam_tolerance < 0 || maximum_seam_segments < 2 || maximum_seam_segments > 16000000) {
        throw std::invalid_argument("Invalid displacement settings");
    }
}

DisplacementResult displace_mesh(std::shared_ptr<const MeshData> input,
                                 std::shared_ptr<const Texture> height,
                                 DisplacementSettings settings, CancellationToken cancellation,
                                 DisplacementProgressCallback progress) {
    cancellation.check();
    settings.validate();
    if (!input || !height) {
        throw std::invalid_argument("Displacement requires a mesh and a linear height texture");
    }
    const auto report = [&](DisplacementStage stage, std::size_t completed, std::size_t total) {
        cancellation.check();
        if (progress) {
            progress({stage, completed, total});
        }
        cancellation.check();
    };
    report(DisplacementStage::topology, 0, 1);
    const HalfedgeMesh topology(*input, cancellation);
    report(DisplacementStage::topology, 1, 1);
    DisplacementResult result;
    result.topology = topology.statistics();
    if (settings.scale == 0) {
        result.mesh = std::move(input);
        report(DisplacementStage::complete, 1, 1);
        return result;
    }
    const auto directions = smooth_vertex_normals(*input, cancellation, [&](auto done, auto total) {
        report(DisplacementStage::directions, done, total);
    });
    const auto sampled = detail::inspect_height_field(*input, topology, height->level(0), settings,
                                                      cancellation, progress);
    const auto& offsets = sampled.offsets;
    result.sampled_corners = sampled.sampled_corners;
    result.seam_edges = sampled.seam_edges;
    result.seam_segments = sampled.seam_segments;
    result.maximum_seam_difference = sampled.maximum_seam_difference;

    // 分块复制与移动，使大网格的复制同样可取消，不执行不可中断的整体 MeshData 拷贝。
    auto output = std::make_shared<MeshData>();
    output->positions.resize(input->positions.size());
    output->triangles.resize(input->triangles.size());
    const auto position_work = input->positions.size() + input->triangles.size();
    report(DisplacementStage::positions, 0, position_work);
    for (std::size_t vertex = 0; vertex < input->positions.size(); ++vertex) {
        const auto p =
            glm::dvec3(input->positions[vertex]) + offsets[vertex] * glm::dvec3(directions[vertex]);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            glm::any(
                glm::greaterThan(glm::abs(p), glm::dvec3(std::numeric_limits<float>::max())))) {
            throw std::invalid_argument("Displaced position exceeds finite float coordinates");
        }
        output->positions[vertex] = glm::vec3(p);
        result.maximum_displacement = std::max(result.maximum_displacement,
                                               glm::length(glm::dvec3(output->positions[vertex]) -
                                                           glm::dvec3(input->positions[vertex])));
        if ((vertex + 1) % 1024 == 0) {
            report(DisplacementStage::positions, vertex + 1, position_work);
        }
    }
    for (std::size_t face = 0; face < input->triangles.size(); ++face) {
        output->triangles[face] = input->triangles[face];
        std::array<glm::dvec3, 3> before, after;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto index = input->triangles[face].corners[corner].position;
            before[corner] = input->positions[index];
            after[corner] = output->positions[index];
        }
        const auto original = glm::cross(before[1] - before[0], before[2] - before[0]);
        const auto displaced = glm::cross(after[1] - after[0], after[2] - after[0]);
        if (glm::dot(original, displaced) <= 0) {
            throw std::invalid_argument("Displacement reverses face " + std::to_string(face));
        }
        const auto done = input->positions.size() + face + 1;
        if (done % 1024 == 0) {
            report(DisplacementStage::positions, done, position_work);
        }
    }
    report(DisplacementStage::positions, position_work, position_work);
    recompute_smooth_normals(*output, cancellation, [&](auto done, auto total) {
        report(DisplacementStage::normals, done, total);
    });
    result.mesh = std::move(output);
    report(DisplacementStage::complete, 1, 1);
    return result;
}

} // namespace minirender
