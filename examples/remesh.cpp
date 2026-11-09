#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/remesh.hpp>
#include <minirender/render/path_tracer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace minirender;

namespace {

// 压力输入为有界扰动的 XZ 平面，边界固定，UV 对应原始 XZ 参数。
std::shared_ptr<const MeshData> irregular_grid(MeshIndex side) {
    auto mesh = std::make_shared<MeshData>();
    for (MeshIndex z = 0; z <= side; ++z) {
        for (MeshIndex x = 0; x <= side; ++x) {
            const bool interior = x > 0 && z > 0 && x < side && z < side;
            const double dx = interior ? 0.23 * std::sin(x * 17.0 + z * 13.0) : 0;
            const double dz = interior ? 0.23 * std::cos(x * 11.0 - z * 19.0) : 0;
            mesh->positions.emplace_back((x + dx) / side, 0, (z + dz) / side);
        }
    }
    for (MeshIndex z = 0; z < side; ++z) {
        for (MeshIndex x = 0; x < side; ++x) {
            const auto a = z * (side + 1) + x;
            for (const auto ids :
                 {std::array{a, a + side + 2, a + 1}, std::array{a, a + side + 1, a + side + 2}}) {
                MeshTriangle face;
                for (std::size_t i = 0; i < 3; ++i) {
                    const auto p = mesh->positions[ids[i]];
                    face.corners[i] = {ids[i], {0, 1, 0}, {p.x, p.z}};
                }
                mesh->triangles.push_back(face);
            }
        }
    }
    return mesh;
}

void print(const RemeshResult& result, double milliseconds) {
    std::cout << "faces=" << result.input_topology.faces << "->" << result.topology.faces
              << " target_length=" << result.target_edge_length << " splits=" << result.splits
              << " collapses=" << result.collapses << " flips=" << result.flips
              << " vertex_updates=" << result.vertex_updates
              << " iterations=" << result.iterations_completed
              << " before_p10=" << result.before.edge_p10
              << " before_p90=" << result.before.edge_p90 << " after_p10=" << result.after.edge_p10
              << " after_p90=" << result.after.edge_p90
              << " before_angle=" << result.before.minimum_angle
              << " after_angle=" << result.after.minimum_angle
              << " before_angle_deviation=" << result.before.mean_angle_deviation
              << " after_angle_deviation=" << result.after.mean_angle_deviation
              << " target_edges=" << result.after.in_target_range << '/' << result.after.edges
              << " rms_m=" << result.error.symmetric_rms
              << " sampled_max_m=" << result.error.sampled_maximum << " total_ms=" << milliseconds
              << '\n';
}

} // namespace

// 演示在固定参照曲面上调整三角形分布，并通过资产保存重开和 CPU 渲染核对结果。
// --stress 使用 32768 面平面，只报告操作、质量与误差，省去图像生成。
int main(int argc, char** argv) {
    try {
        const bool stress = argc == 2 && std::string(argv[1]) == "--stress";
        if (argc > 1 && !stress) {
            throw std::invalid_argument("Usage: minirender_remesh_example [--stress]");
        }
        const auto output = std::filesystem::current_path() / "outputs/a4.4";
        std::filesystem::create_directories(output);
        if (stress) {
            const auto input = irregular_grid(128);
            const auto start = std::chrono::steady_clock::now();
            const auto result =
                remesh_isotropic(input, {.target_edge_length = 1.0 / 64, .iterations = 5});
            print(result, std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count());
            return 0;
        }
        const auto source = std::filesystem::path(MINIRENDER_SOURCE_DIR);
        auto scene = load_scene(source / "scenes/studio.json");
        const auto cube = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                       [](const auto& mesh) { return mesh.id == "cube"; });
        if (cube == scene.meshes.end()) {
            throw std::runtime_error("Studio cube is missing");
        }
        auto input = std::make_shared<MeshData>(*subdivide_loop(cube->data, {.levels = 4}).mesh);
        // 本例使用无纹理的连续曲面；接缝保护由单独的平面/硬边测试覆盖。
        for (auto& face : input->triangles) {
            for (auto& c : face.corners) {
                c.uv = {0, 0};
            }
        }
        scene.meshes.push_back({"remesh_example", {}, input});
        for (auto& object : scene.objects) {
            if (object.id == "blue_cube") {
                object.mesh_id = "remesh_example";
            }
        }
        RenderSettings settings;
        settings.width = settings.height = 128;
        settings.samples_per_pixel = 32;
        settings.seed = 101;
        const auto render = [&](const SceneDocument& document, const std::string& name) {
            const PathTracer tracer(make_snapshot(document));
            auto image = tracer.render(settings);
            image.write_exr(output / (name + ".exr"));
            display_image(image).write_png(output / (name + ".png"));
            return image;
        };
        render(scene, "original");
        const auto start = std::chrono::steady_clock::now();
        const auto result = remesh_isotropic(input, {.target_edge_length = 0.12, .iterations = 5});
        print(result,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count());
        scene.meshes.back().data = result.mesh;
        save_obj(*result.mesh, output / "remeshed.obj");
        save_scene(scene, output / "remeshed.json");
        const auto image = render(scene, "remeshed");
        const auto reopened = render(load_scene(output / "remeshed.json"), "reopened");
        double maximum = 0;
        for (std::size_t y = 0; y < image.height(); ++y) {
            for (std::size_t x = 0; x < image.width(); ++x) {
                maximum = std::max(
                    maximum, static_cast<double>(glm::length(image.at(x, y) - reopened.at(x, y))));
            }
        }
        std::cout << "roundtrip_max_rgb=" << maximum << '\n';
        if (maximum > 1e-4) {
            throw std::runtime_error("Remeshing asset round trip changed the CPU image");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Remeshing example failed: " << error.what() << '\n';
        return 1;
    }
}
