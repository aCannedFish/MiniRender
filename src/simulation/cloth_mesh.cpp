#include <minirender/simulation/cloth.hpp>

#include <cmath>
#include <stdexcept>

namespace minirender {

FrozenCloth ClothSimulation::freeze(CancellationToken cancellation) const {
    cancellation.check();
    auto mesh = std::make_shared<MeshData>();
    mesh->positions.reserve(state_.positions.size());
    mesh->triangles.reserve(triangles_.size());
    for (std::size_t i = 0; i < state_.positions.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        mesh->positions.push_back(glm::vec3(state_.positions[i] - glm::dvec3(grid_.origin)));
    }
    std::vector<glm::dvec3> sums(rest_.size(), glm::dvec3(0));
    std::vector<glm::dvec3> largest(rest_.size(), glm::dvec3(0));
    std::vector<double> areas(rest_.size(), 0);
    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        const auto [a, b, c] = triangles_[i];
        const glm::dvec3 p = mesh->positions[a], q = mesh->positions[b], r = mesh->positions[c];
        const auto area = glm::cross(q - p, r - p);
        const double length = glm::length(area);
        if (!std::isfinite(length) || length <= 1e-10) {
            throw std::runtime_error("Cloth has a degenerate triangle; cannot freeze this frame");
        }
        for (const auto index : triangles_[i]) {
            sums[index] += area;
            areas[index] += length;
            if (glm::dot(area, area) > glm::dot(largest[index], largest[index])) {
                largest[index] = area;
            }
        }
    }
    for (std::size_t i = 0; i < sums.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        const auto direction = glm::length(sums[i]) > 1e-12 * areas[i] ? sums[i] : largest[i];
        sums[i] = glm::normalize(direction);
    }
    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        MeshTriangle face;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto index = triangles_[i][corner];
            face.corners[corner] = {
                index,
                glm::vec3(sums[index]),
                {static_cast<float>(index % grid_.columns) / static_cast<float>(grid_.columns - 1),
                 static_cast<float>(index / grid_.columns) / static_cast<float>(grid_.rows - 1)}};
        }
        mesh->triangles.push_back(face);
    }
    mesh->validate(cancellation);
    Transform transform;
    transform.translation = grid_.origin;
    return {mesh, transform};
}

} // namespace minirender
