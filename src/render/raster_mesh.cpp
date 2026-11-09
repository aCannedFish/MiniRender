#include <minirender/render/raster_mesh.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace minirender {

std::shared_ptr<const RasterMeshData> prepare_raster_mesh(const MeshData& mesh,
                                                          CancellationToken cancellation,
                                                          RasterMeshProgress progress) {
    cancellation.check();
    if (mesh.positions.empty() || mesh.triangles.empty()) {
        throw std::invalid_argument("Raster mesh must contain positions and triangles");
    }
    if (mesh.triangles.size() > std::numeric_limits<std::uint32_t>::max() / 6U) {
        throw std::length_error("Raster mesh exceeds the draw index range");
    }
    const auto total = mesh.positions.size() + mesh.triangles.size();
    const auto report = [&](std::size_t completed) {
        cancellation.check();
        if (progress) {
            progress(completed, total);
        }
        cancellation.check();
    };
    report(0);
    auto data = std::make_shared<RasterMeshData>();
    data->minimum = glm::vec3(std::numeric_limits<float>::infinity());
    data->maximum = glm::vec3(-std::numeric_limits<float>::infinity());
    data->vertices.resize(mesh.triangles.size() * 3);
    data->edges.resize(mesh.triangles.size() * 6);
    for (std::size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
        const auto point = mesh.positions[vertex];
        data->minimum = glm::min(data->minimum, point);
        data->maximum = glm::max(data->maximum, point);
        if ((vertex + 1) % 1024 == 0) {
            report(vertex + 1);
        }
    }
    for (std::size_t face = 0; face < mesh.triangles.size(); ++face) {
        const auto& triangle = mesh.triangles[face];
        const auto a = glm::dvec3(mesh.positions.at(triangle.corners[0].position));
        const auto b = glm::dvec3(mesh.positions.at(triangle.corners[1].position));
        const auto c = glm::dvec3(mesh.positions.at(triangle.corners[2].position));
        const auto geometric = glm::vec3(glm::normalize(glm::cross(b - a, c - a)));
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& source = mesh.triangles[face].corners[corner];
            data->vertices[face * 3 + corner] = {mesh.positions.at(source.position), source.normal,
                                                 source.uv, geometric};
            data->edges[face * 6 + corner * 2] = static_cast<std::uint32_t>(face * 3 + corner);
            data->edges[face * 6 + corner * 2 + 1] =
                static_cast<std::uint32_t>(face * 3 + (corner + 1) % 3);
        }
        const auto completed = mesh.positions.size() + face + 1;
        if (completed % 1024 == 0) {
            report(completed);
        }
    }
    report(total);
    return data;
}

void RasterMeshCache::supply(const std::shared_ptr<const MeshData>& mesh,
                             std::shared_ptr<const RasterMeshData> prepared) {
    if (!mesh || !prepared) {
        throw std::invalid_argument("Raster cache requires a mesh and prepared data");
    }
    std::erase_if(entries_, [](const auto& entry) { return entry.first.expired(); });
    entries_[mesh] = {std::move(prepared), ++clock_};
}

std::shared_ptr<const RasterMeshData>
RasterMeshCache::find(const std::shared_ptr<const MeshData>& mesh) {
    std::erase_if(entries_, [](const auto& entry) { return entry.first.expired(); });
    const auto entry = entries_.find(mesh);
    if (entry == entries_.end()) {
        return nullptr;
    }
    entry->second.used = ++clock_;
    return entry->second.data;
}

void RasterMeshCache::retain(std::span<const RenderInstance> instances) {
    std::set<Key, std::owner_less<Key>> used;
    for (const auto& instance : instances) {
        used.insert(instance.mesh);
    }
    std::erase_if(entries_, [](const auto& entry) { return entry.first.expired(); });
    std::vector<decltype(entries_)::iterator> unused;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!used.contains(it->first)) {
            unused.push_back(it);
        }
    }
    std::sort(unused.begin(), unused.end(),
              [](auto first, auto second) { return first->second.used > second->second.used; });
    for (std::size_t index = 2; index < unused.size(); ++index) {
        entries_.erase(unused[index]);
    }
}

} // namespace minirender
