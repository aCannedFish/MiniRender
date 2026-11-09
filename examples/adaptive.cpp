#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/adaptive.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/surface.hpp>
#include <minirender/render/path_tracer.hpp>

#include <cmath>
#include <iostream>

using namespace minirender;

namespace {

// 连续平面参数用于局部高度特征，XZ 几何正面朝 +Y。
std::shared_ptr<const MeshData> control_grid(MeshIndex side) {
    auto mesh = std::make_shared<MeshData>();
    for (MeshIndex z = 0; z <= side; ++z) {
        for (MeshIndex x = 0; x <= side; ++x) {
            mesh->positions.emplace_back(
                2.0F * static_cast<float>(x) / static_cast<float>(side) - 1, 0,
                2.0F * static_cast<float>(z) / static_cast<float>(side) - 1);
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
                    face.corners[i] = {ids[i], {0, 1, 0}, {(p.x + 1) / 2, (p.z + 1) / 2}};
                }
                mesh->triangles.push_back(face);
            }
        }
    }
    return mesh;
}

} // namespace

// 与实际均匀 Loop/位移参照比较面数和表面距离，并保存可在工作台重开的场景。
int main() {
    try {
        const auto output = std::filesystem::current_path() / "outputs/a4.5";
        std::filesystem::create_directories(output);
        Image image(32, 32);
        for (std::size_t y = 0; y < 32; ++y) {
            for (std::size_t x = 0; x < 32; ++x) {
                const double u = (static_cast<double>(x) + .5) / 32;
                const double v = 1 - (static_cast<double>(y) + .5) / 32;
                image.at(x, y) = glm::vec3(static_cast<float>(
                    .5 + .8 * std::exp(-((u - .68) * (u - .68) + (v - .62) * (v - .62)) / .008)));
            }
        }
        const auto height = std::make_shared<const Texture>(image);
        const auto input = control_grid(3);
        AdaptiveSettings settings;
        settings.maximum_level = 6;
        settings.tolerance = .008;
        settings.displacement = DisplacementSettings{.scale = .5};
        const auto adaptive = subdivide_adaptive(input, height, settings);
        const auto refined = subdivide_loop(input, {.levels = settings.maximum_level}).mesh;
        const auto uniform = displace_mesh(refined, height, *settings.displacement).mesh;
        const auto comparison = compare_surfaces(uniform, adaptive.mesh);
        std::cout << "faces=" << input->triangles.size() << " -> "
                  << adaptive.mesh->triangles.size() << " uniform=" << uniform->triangles.size()
                  << " eval_points=" << adaptive.evaluation_points
                  << " uniform_positions=" << uniform->positions.size()
                  << " sampled_error=" << adaptive.maximum_sampled_error
                  << " limited=" << adaptive.maximum_level_reached
                  << " transitions=" << adaptive.transition_triangles
                  << " rms_m=" << comparison.symmetric_rms
                  << " sampled_max_m=" << comparison.sampled_maximum << '\n';
        for (std::size_t level = 0; level < adaptive.triangles_per_level.size(); ++level) {
            std::cout << "L" << level << "=" << adaptive.triangles_per_level[level] << ' ';
        }
        std::cout << '\n';
        auto scene =
            load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
        scene.meshes.push_back({"terrain", {}, adaptive.mesh});
        for (auto& object : scene.objects) {
            if (object.id == "blue_cube") {
                object.mesh_id = "terrain";
                object.transform.translation = {0, .05F, 0};
                object.transform.rotation_degrees = {0, 0, 0};
            } else if (object.id == "metal_cube") {
                object.transform.translation = {1.4F, .35F, .4F};
            }
        }
        scene.camera.position = {3, 2.3F, 3.4F};
        scene.camera.target = {0, .2F, 0};
        const RenderSettings render{128, 128, 32, 101};
        const auto actual = PathTracer(make_snapshot(scene)).render(render);
        actual.write_exr(output / "adaptive.exr");
        display_image(actual).write_png(output / "adaptive.png");
        save_scene(scene, output / "adaptive.json");
        const auto reopened = load_scene(output / "adaptive.json");
        const auto roundtrip = PathTracer(make_snapshot(reopened)).render(render);
        double maximum = 0;
        for (std::size_t y = 0; y < render.height; ++y) {
            for (std::size_t x = 0; x < render.width; ++x) {
                maximum = std::max(maximum, static_cast<double>(
                                                glm::length(actual.at(x, y) - roundtrip.at(x, y))));
            }
        }
        scene.meshes.back().data = uniform;
        const auto reference = PathTracer(make_snapshot(scene)).render(render);
        display_image(reference).write_png(output / "uniform.png");
        std::cout << "roundtrip_rgb_max=" << maximum << '\n';
        return maximum < 2e-6 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
