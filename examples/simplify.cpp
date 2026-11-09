#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/geometry/simplify.hpp>
#include <minirender/render/path_tracer.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace minirender;

// 同一高精度输入分别生成两个 LOD，保留共享原资产；保存后重开并对照固定种子渲染。
// --stress 用 49152 面输入评估后台算法规模，省去图像生成。
int main(int argc, char** argv) {
    try {
        const bool stress = argc == 2 && std::string(argv[1]) == "--stress";
        if (argc > 1 && !stress) {
            throw std::invalid_argument("Usage: minirender_simplify_example [--stress]");
        }
        const auto source = std::filesystem::path(MINIRENDER_SOURCE_DIR);
        auto scene = load_scene(source / "scenes/studio.json");
        const auto cube = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                       [](const auto& mesh) { return mesh.id == "cube"; });
        if (cube == scene.meshes.end()) {
            throw std::runtime_error("Studio cube mesh is missing");
        }
        const auto original = subdivide_loop(cube->data, {.levels = stress ? 6U : 4U}).mesh;
        const auto directory = std::filesystem::current_path() / "outputs/a4.3";
        std::filesystem::create_directories(directory);
        scene.meshes.push_back({"qem_example", {}, original});
        for (auto& object : scene.objects) {
            if (object.id == "blue_cube") {
                object.mesh_id = "qem_example";
            }
        }
        RenderSettings settings;
        settings.width = 128;
        settings.height = 128;
        settings.samples_per_pixel = 32;
        settings.seed = 101;
        const auto render = [&](const SceneDocument& document, const std::string& label) {
            const PathTracer tracer(make_snapshot(document));
            auto image = tracer.render(settings);
            image.write_exr(directory / (label + ".exr"));
            display_image(image).write_png(directory / (label + ".png"));
            return image;
        };
        if (!stress) {
            render(scene, "original");
        }
        for (const auto target :
             stress ? std::vector<std::size_t>{12288} : std::vector<std::size_t>{768, 384}) {
            const auto start = std::chrono::steady_clock::now();
            const auto result = simplify_qem(original, {.target_triangles = target});
            const auto milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            std::cout << "input=" << original->triangles.size() << " target=" << target
                      << " actual=" << result.topology.faces << " collapses=" << result.collapses
                      << " protected=" << result.protected_vertices
                      << " stop=" << simplify_stop_name(result.stop)
                      << " qem_max=" << result.maximum_quadric_error
                      << " rms_m=" << result.error.symmetric_rms
                      << " sampled_max_m=" << result.error.sampled_maximum
                      << " candidate_evaluations=" << result.candidate_evaluations
                      << " total_ms=" << milliseconds << '\n';
            if (stress) {
                continue;
            }
            scene.meshes.back().data = result.mesh;
            const auto label = "lod-" + std::to_string(target);
            save_obj(*result.mesh, directory / (label + ".obj"));
            save_scene(scene, directory / (label + ".json"));
            const auto image = render(scene, label);
            const auto reopened = load_scene(directory / (label + ".json"));
            const auto roundtrip = render(reopened, label + "-reopened");
            double maximum = 0;
            for (std::size_t y = 0; y < image.height(); ++y) {
                for (std::size_t x = 0; x < image.width(); ++x) {
                    maximum = std::max(maximum, static_cast<double>(glm::length(
                                                    image.at(x, y) - roundtrip.at(x, y))));
                }
            }
            if (maximum > 1e-4) {
                throw std::runtime_error(
                    "Simplified asset round trip changed the CPU reference image");
            }
            std::cout << "roundtrip_max_rgb=" << maximum << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Simplification example failed: " << error.what() << '\n';
        return 1;
    }
}
