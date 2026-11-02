#include <minirender/assets/scene_io.hpp>

#include <tiny_obj_loader.h>

#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace minirender {

MeshData load_obj(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open OBJ: " + path.generic_string());
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), {});
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromString(text, "", config)) {
        throw std::runtime_error("OBJ parse error: " + reader.Error());
    }
    const auto& attributes = reader.GetAttrib();
    if (attributes.vertices.size() / 3 > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("OBJ exceeds supported position index range");
    }
    MeshData mesh;
    for (std::size_t i = 0; i + 2 < attributes.vertices.size(); i += 3) {
        mesh.positions.emplace_back(attributes.vertices[i], attributes.vertices[i + 1],
                                    attributes.vertices[i + 2]);
    }
    for (const auto& shape : reader.GetShapes()) {
        std::size_t offset = 0;
        for (auto vertices : shape.mesh.num_face_vertices) {
            if (vertices != 3 || offset + 3 > shape.mesh.indices.size()) {
                throw std::runtime_error("OBJ triangulation did not produce triangles");
            }
            MeshTriangle triangle;
            std::array<bool, 3> needs_normal{};
            for (std::size_t k = 0; k < 3; ++k) {
                const auto index = shape.mesh.indices[offset + k];
                if (index.vertex_index < 0 ||
                    static_cast<std::size_t>(index.vertex_index) >= mesh.positions.size()) {
                    throw std::runtime_error("OBJ has an invalid vertex index");
                }
                auto& corner = triangle.corners[k];
                corner.position = static_cast<std::uint32_t>(index.vertex_index);
                if (index.normal_index >= 0) {
                    const auto n = static_cast<std::size_t>(index.normal_index) * 3;
                    if (n + 2 >= attributes.normals.size()) {
                        throw std::runtime_error("Invalid OBJ normal index");
                    }
                    corner.normal = {attributes.normals[n], attributes.normals[n + 1],
                                     attributes.normals[n + 2]};
                    const auto length = glm::length(corner.normal);
                    if (!(length > 1e-10F)) {
                        throw std::runtime_error("OBJ normal is zero");
                    }
                    corner.normal /= length;
                } else {
                    needs_normal[k] = true;
                }
                if (index.texcoord_index >= 0) {
                    const auto uv = static_cast<std::size_t>(index.texcoord_index) * 2;
                    if (uv + 1 >= attributes.texcoords.size()) {
                        throw std::runtime_error("Invalid OBJ UV index");
                    }
                    corner.uv = {attributes.texcoords[uv], attributes.texcoords[uv + 1]};
                }
            }
            const auto a = mesh.positions[triangle.corners[0].position];
            const auto b = mesh.positions[triangle.corners[1].position];
            const auto c = mesh.positions[triangle.corners[2].position];
            const auto cross = glm::cross(b - a, c - a);
            if (!(glm::length(cross) > 1e-10F)) {
                throw std::runtime_error("OBJ has a degenerate triangle");
            }
            for (std::size_t k = 0; k < 3; ++k) {
                if (needs_normal[k]) {
                    triangle.corners[k].normal = glm::normalize(cross);
                }
            }
            mesh.triangles.push_back(triangle);
            offset += 3;
        }
    }
    mesh.validate();
    return mesh;
}

} // namespace minirender
