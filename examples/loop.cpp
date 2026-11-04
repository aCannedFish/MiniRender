#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/render/path_tracer.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace minirender;

// 最小 CPU 示例：在同一 studio 场景中替换共享 cube 资产，以相同相机、种子和光照
// 渲染 0/1/3 层结果。生成网格直接进入现有快照与路径追踪，不改写源场景或 OBJ。
int main() {
    try {
        const auto source_directory = std::filesystem::path(MINIRENDER_SOURCE_DIR);
        const auto original = load_scene(source_directory / "scenes/studio.json");
        const auto output = std::filesystem::current_path() / "outputs/a4.1";
        std::filesystem::create_directories(output);
        RenderSettings render;
        render.width = 128;
        render.height = 128;
        render.samples_per_pixel = 32;
        render.seed = 101;
        for (const std::uint32_t level : {0U, 1U, 3U}) {
            auto scene = original;
            const auto cube =
                std::find_if(scene.meshes.begin(), scene.meshes.end(),
                             [](const MeshAsset& asset) { return asset.id == "cube"; });
            if (cube == scene.meshes.end()) {
                throw std::runtime_error("Studio scene is missing the cube mesh");
            }
            const auto start = std::chrono::steady_clock::now();
            const auto result = subdivide_loop(cube->data, {level});
            const auto elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
            cube->data = result.mesh;
            // 清除旧文件来源，明确当前资产已是生成结果；本示例不执行场景保存。
            cube->source.clear();
            const auto snapshot = make_snapshot(scene);
            const PathTracer tracer(snapshot);
            const auto image = tracer.render(render);
            const auto filename = "loop-" + std::to_string(level);
            image.write_exr(output / (filename + ".exr"));
            display_image(image).write_png(output / (filename + ".png"));
            const auto& stats = result.levels.back();
            std::cout << "level=" << level << " vertices=" << stats.referenced_vertices
                      << " faces=" << stats.faces << " boundary=" << stats.boundary_edges
                      << " euler=" << stats.euler_characteristic
                      << " geometry_ms=" << elapsed.count() << '\n';
        }
        std::cout << "Output: " << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Loop example failed: " << error.what() << '\n';
        return 1;
    }
}
