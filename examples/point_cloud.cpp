#include <minirender/assets/point_io.hpp>
#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/ball_pivot.hpp>
#include <minirender/geometry/point_normals.hpp>
#include <minirender/geometry/surface.hpp>
#include <minirender/render/path_tracer.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

using namespace minirender;

namespace {

// 自建环面提供完整参照三角面与光滑法线，采样后清除连接和法线再重建。
std::shared_ptr<const MeshData> reference_torus() {
    auto mesh = std::make_shared<MeshData>();
    constexpr std::uint32_t around = 64;
    constexpr std::uint32_t tube = 32;
    std::vector<glm::vec3> normals;
    for (std::uint32_t u = 0; u < around; ++u) {
        for (std::uint32_t v = 0; v < tube; ++v) {
            const double theta = 2 * std::numbers::pi * u / around;
            const double phi = 2 * std::numbers::pi * v / tube;
            const double radius = 1 + .35 * std::cos(phi);
            mesh->positions.emplace_back(radius * std::cos(theta), .35 * std::sin(phi),
                                         radius * std::sin(theta));
            normals.emplace_back(std::cos(phi) * std::cos(theta), std::sin(phi),
                                 std::cos(phi) * std::sin(theta));
        }
    }
    for (std::uint32_t u = 0; u < around; ++u) {
        for (std::uint32_t v = 0; v < tube; ++v) {
            const auto a = u * tube + v;
            const auto b = u * tube + (v + 1) % tube;
            const auto c = ((u + 1) % around) * tube + (v + 1) % tube;
            const auto d = ((u + 1) % around) * tube + v;
            for (const auto ids : {std::array{a, b, c}, std::array{a, c, d}}) {
                MeshTriangle triangle;
                for (std::size_t i = 0; i < 3; ++i) {
                    triangle.corners[i] = {ids[i], normals[ids[i]], {0, 0}};
                }
                mesh->triangles.push_back(triangle);
            }
        }
    }
    mesh->validate();
    return mesh;
}

std::filesystem::path native(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}
std::vector<double> parse_radii(std::string_view text) {
    std::vector<double> radii;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto token = text.substr(0, comma);
        double radius = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), radius);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
            throw std::invalid_argument("Radii must be comma-separated numbers");
        }
        radii.push_back(radius);
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
        if (text.empty()) {
            throw std::invalid_argument("Missing radius after comma");
        }
    }
    return radii;
}

// 重建保持输入单位；用于展示时只通过物体变换缩放至约两米，底部放在地面上方。
SceneDocument preview_scene(std::shared_ptr<const MeshData> mesh) {
    auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    auto low = glm::vec3(std::numeric_limits<float>::infinity());
    auto high = glm::vec3(-std::numeric_limits<float>::infinity());
    for (const auto point : mesh->positions) {
        low = glm::min(low, point);
        high = glm::max(high, point);
    }
    const auto extent = high - low;
    const float scale = 2 / std::max({extent.x, extent.y, extent.z});
    const auto center = low + extent * .5F;
    scene.meshes.push_back({"reconstruction", {}, std::move(mesh)});
    std::erase_if(scene.objects, [](const auto& object) { return object.id == "metal_cube"; });
    for (auto& object : scene.objects) {
        if (object.id == "blue_cube") {
            object.mesh_id = "reconstruction";
            object.name = "点云重建";
            object.transform.rotation_degrees = {0, 0, 0};
            object.transform.scale = glm::vec3(scale);
            object.transform.translation = {-center.x * scale, .03F - low.y * scale,
                                            -center.z * scale};
        }
    }
    scene.camera.position = {3, 2.4F, 3};
    scene.camera.target = {0, .7F, 0};
    return scene;
}

} // namespace

// 默认执行采样 -> PLY -> PCA/MST -> 多半径 BPA -> OBJ/场景 -> CPU 图像。
// 可提供外部 PLY、输出目录及半径列表，便于使用相同实现复验独立点云。
int main(int argc, char** argv) {
    try {
        if (argc > 4) {
            throw std::invalid_argument(
                "Usage: minirender_point_cloud_example [input.ply [output-directory [r1,r2,...]]]");
        }
        const auto output =
            argc >= 3 ? native(argv[2]) : std::filesystem::current_path() / "outputs/a5.1";
        std::filesystem::create_directories(output);
        PointCloud input;
        std::shared_ptr<const MeshData> reference;
        if (argc >= 2) {
            const auto source = std::filesystem::weakly_canonical(native(argv[1]));
            for (const auto* name : {"prepared.ply", "reconstruction.obj", "scene.json",
                                     "reconstruction.exr", "reconstruction.png"}) {
                if (source == std::filesystem::weakly_canonical(output / name)) {
                    throw std::invalid_argument(
                        "Example output would overwrite the input point cloud");
                }
            }
            input = load_point_cloud(source);
        } else {
            reference = reference_torus();
            input = sample_mesh_surface(*reference, 5000, 101);
            input.normals.clear();
            save_point_cloud(input, output / "input.ply");
            input = load_point_cloud(output / "input.ply");
        }
        const auto started = std::chrono::steady_clock::now();
        const auto prepared =
            prepare_point_normals(std::make_shared<const PointCloud>(input), {.neighbors = 20});
        std::cout << "input=" << input.positions.size()
                  << " unique=" << prepared.cloud->positions.size()
                  << " normal_components=" << prepared.orientation_components
                  << " spacing_m=" << prepared.spacing_median << " p90=" << prepared.spacing_p90
                  << std::endl;
        BallPivotSettings settings;
        settings.radii =
            argc == 4 ? parse_radii(argv[3])
                      : std::vector<double>{2 * prepared.spacing_p90, 3 * prepared.spacing_p90,
                                            4 * prepared.spacing_p90};
        const auto reconstruction = reconstruct_ball_pivot(prepared.cloud, settings);
        if (!reconstruction.mesh) {
            throw std::runtime_error("No triangles reconstructed; increase the ball radii");
        }
        std::cout << "faces=" << reconstruction.topology.faces
                  << " vertices=" << reconstruction.mesh->positions.size()
                  << " unused=" << reconstruction.unused_points
                  << " split=" << reconstruction.split_vertices
                  << " boundary_edges=" << reconstruction.topology.boundary_edges
                  << " boundary_loops=" << reconstruction.topology.boundary_loops
                  << " components=" << reconstruction.topology.face_components
                  << " euler=" << reconstruction.topology.euler_characteristic
                  << " candidate_tests=" << reconstruction.candidate_tests << " processing_ms="
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               started)
                         .count()
                  << std::endl;
        for (const auto& pass : reconstruction.passes) {
            std::cout << "radius=" << pass.radius << " new_faces=" << pass.new_triangles
                      << " boundary=" << pass.boundary_edges
                      << " reactivated=" << pass.restarted_edges << '\n';
        }
        const MeshSurface surface(reconstruction.mesh);
        double squared = 0;
        double maximum_distance = 0;
        for (const auto p : input.positions) {
            const double distance = surface.closest(glm::dvec3(p)).distance_squared;
            squared += distance;
            maximum_distance = std::max(maximum_distance, std::sqrt(distance));
        }
        std::cout << "input_to_mesh_rms_m="
                  << std::sqrt(squared / static_cast<double>(input.positions.size()))
                  << " max_m=" << maximum_distance << '\n';
        if (reference) {
            const auto error = compare_surfaces(reference, reconstruction.mesh);
            std::cout << "reference_surface_rms_m=" << error.symmetric_rms
                      << " max_m=" << error.sampled_maximum << '\n';
        }
        save_point_cloud(*prepared.cloud, output / "prepared.ply");
        save_obj(*reconstruction.mesh, output / "reconstruction.obj");
        const auto scene = preview_scene(reconstruction.mesh);
        save_scene(scene, output / "scene.json");
        const RenderSettings render{128, 128, 32, 101};
        const auto image = PathTracer(make_snapshot(scene)).render(render);
        image.write_exr(output / "reconstruction.exr");
        display_image(image).write_png(output / "reconstruction.png");
        const auto reopened =
            PathTracer(make_snapshot(load_scene(output / "scene.json"))).render(render);
        double maximum_rgb = 0;
        for (std::size_t y = 0; y < render.height; ++y) {
            for (std::size_t x = 0; x < render.width; ++x) {
                maximum_rgb =
                    std::max(maximum_rgb,
                             static_cast<double>(glm::length(image.at(x, y) - reopened.at(x, y))));
            }
        }
        std::cout << "roundtrip_rgb_max=" << maximum_rgb << '\n';
        return maximum_rgb < 2e-6 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
