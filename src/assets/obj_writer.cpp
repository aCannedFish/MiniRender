#include "obj_writer.hpp"
#include "staged_file.hpp"

#include <iomanip>
#include <limits>
#include <locale>

namespace minirender {
namespace detail {

void write_obj_content(const MeshData& mesh, std::ostream& stream, CancellationToken cancellation,
                       const MeshWriteProgress& progress) {
    const auto total = mesh.positions.size() + mesh.triangles.size() * 7;
    const auto report = [&](std::size_t completed) {
        cancellation.check();
        if (progress) {
            progress(completed, total);
        }
        cancellation.check();
    };
    std::size_t completed = 0;
    const auto advance = [&] {
        if (++completed % 1024 == 0) {
            report(completed);
        }
    };
    report(0);
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    stream << "# MiniRender triangle mesh; shared positions and face-varying attributes\n";
    for (const auto p : mesh.positions) {
        stream << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
        advance();
    }
    for (const auto& face : mesh.triangles) {
        for (const auto& corner : face.corners) {
            stream << "vt " << corner.uv.x << ' ' << corner.uv.y << '\n';
            advance();
        }
    }
    for (const auto& face : mesh.triangles) {
        for (const auto& corner : face.corners) {
            const auto n = glm::vec3(glm::normalize(glm::dvec3(corner.normal)));
            stream << "vn " << n.x << ' ' << n.y << ' ' << n.z << '\n';
            advance();
        }
    }
    for (std::size_t face = 0; face < mesh.triangles.size(); ++face) {
        stream << 'f';
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto attribute = face * 3 + corner + 1;
            stream << ' '
                   << static_cast<std::size_t>(mesh.triangles[face].corners[corner].position) + 1
                   << '/' << attribute << '/' << attribute;
        }
        stream << '\n';
        advance();
    }
    report(completed);
}

} // namespace detail

void save_obj(const MeshData& mesh, const std::filesystem::path& path,
              CancellationToken cancellation, MeshWriteProgress progress) {
    mesh.validate(cancellation);
    detail::StagedFile staged(path);
    detail::write_obj_content(mesh, staged.stream(), cancellation, progress);
    cancellation.check();
    staged.commit();
}

} // namespace minirender
