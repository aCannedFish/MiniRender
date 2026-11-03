#include <minirender/assets/scene_io.hpp>
#include <minirender/core/image.hpp>
#include <minirender/render/bvh.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <string_view>

// 无窗口入口，仅依赖 core。后续算法在此提供与 UI 相同的场景/任务接口。
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--capabilities") {
            std::cout << "MiniRender 0.1\nimage: linear RGB\noptix: unavailable (B2 pending)\n";
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--inspect") {
            const auto scene = minirender::load_scene(argv[2]);
            const auto snapshot = minirender::make_snapshot(scene);
            std::size_t triangles = 0;
            for (const auto& instance : snapshot->instances) {
                triangles += instance.mesh->triangles.size();
            }
            std::cout << "meshes: " << scene.meshes.size() << "\nobjects: " << scene.objects.size()
                      << "\nvisible instances: " << snapshot->instances.size()
                      << "\ninstance triangles: " << triangles << '\n';
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--test-pattern") {
            minirender::make_diagnostic_image(128, 96).write_ppm(argv[2]);
            std::cout << "Diagnostic image saved; this is not a rendered scene.\n";
            return 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--trace-normals") {
            const auto snapshot = minirender::make_snapshot(minirender::load_scene(argv[2]));
            const auto start = std::chrono::steady_clock::now();
            const minirender::SceneBvh bvh(snapshot);
            const auto built = std::chrono::steady_clock::now();
            minirender::Image image(256, 256);
            minirender::TraceStatistics queries;
            const auto& camera = snapshot->camera;
            const auto forward = glm::normalize(camera.target - camera.position);
            // 一条主射线对应一个像素中心。这里只输出法线，尚未计算光照或路径积分。
            for (std::size_t y = 0; y < image.height(); ++y) {
                for (std::size_t x = 0; x < image.width(); ++x) {
                    const glm::vec2 uv{
                        (static_cast<float>(x) + 0.5F) / static_cast<float>(image.width()),
                        (static_cast<float>(y) + 0.5F) / static_cast<float>(image.height())};
                    const auto ray = camera.generate_ray(uv, 1);
                    const double cosine = glm::dot(ray.direction, forward);
                    const auto hit = bvh.intersect(
                        ray, {camera.near_plane / cosine, camera.far_plane / cosine}, &queries);
                    image.at(x, y) = hit ? hit->shading_normal * 0.5F + 0.5F : glm::vec3(0);
                }
            }
            const auto finished = std::chrono::steady_clock::now();
            image.write_png(argv[3]);
            const auto& statistics = bvh.statistics();
            std::cout << "geometry diagnostic: world-space shading normals\ntriangles: "
                      << statistics.triangles << "\nnodes: " << statistics.nodes
                      << "\nleaves: " << statistics.leaves
                      << "\nmaximum depth: " << statistics.maximum_depth << "\nbuild ms: "
                      << std::chrono::duration<double, std::milli>(built - start).count()
                      << "\ntrace ms: "
                      << std::chrono::duration<double, std::milli>(finished - built).count()
                      << "\nbounds tests: " << queries.bounds_tests
                      << "\ntriangle tests: " << queries.triangle_tests << '\n';
            return 0;
        }
        std::cout << "Usage: minirender_cli --capabilities | --test-pattern output.ppm | --inspect "
                     "scene.json | --trace-normals scene.json output.png\n";
        return argc == 1 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
