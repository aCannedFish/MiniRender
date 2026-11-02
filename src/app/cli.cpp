#include <minirender/assets/scene_io.hpp>
#include <minirender/core/image.hpp>

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
        std::cout << "Usage: minirender_cli --capabilities | --test-pattern output.ppm | --inspect "
                     "scene.json\n";
        return argc == 1 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
