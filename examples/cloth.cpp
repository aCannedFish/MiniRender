#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>
#include <minirender/simulation/cloth.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace minirender;

namespace {

// 使用已有场景资源显示模拟几何；A6.2 再接入专门的微圆柱布料材质。
SceneDocument scene_for(const FrozenCloth& cloth, bool sphere) {
    const auto source = std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes";
    auto scene = load_scene(source / "studio.json");
    std::erase_if(scene.objects, [](const auto& object) { return object.id != "ground"; });
    Material fabric;
    fabric.id = "fabric";
    fabric.name = "布料几何参照";
    fabric.base_color = {.08F, .42F, .68F};
    fabric.base_color_texture = "fabric-grid";
    scene.materials.push_back(fabric);
    scene.textures.push_back(
        {"fabric-grid", "assets/textures/checker.png", ColorEncoding::srgb,
         std::make_shared<const Texture>(Image::read(source / "assets/textures/checker.png"))});
    scene.meshes.push_back({"cloth", {}, cloth.mesh});
    scene.objects.push_back({"cloth", "模拟布料", "cloth", "fabric", cloth.transform});
    if (sphere) {
        Material support;
        support.id = "support";
        support.name = "支撑球";
        support.base_color = {.65F, .7F, .75F};
        scene.materials.push_back(support);
        scene.meshes.push_back(
            {"support", "assets/demo-sphere.obj",
             std::make_shared<const MeshData>(load_obj(source / "assets/demo-sphere.obj"))});
        Transform transform;
        transform.translation = {0, .65F, 0};
        transform.scale = glm::vec3(.65F);
        scene.objects.push_back({"support", "碰撞球", "support", "support", transform});
        scene.camera.position = {3, 2.4F, 3.8F};
        scene.camera.target = {0, .7F, 0};
    } else {
        scene.camera.position = {2.7F, 2.1F, 4.5F};
        scene.camera.target = {0, 1.25F, .35F};
    }
    return scene;
}

void render_frame(const ClothSimulation& simulation, bool sphere,
                  const std::filesystem::path& output, const std::string& name, bool save) {
    const auto scene = scene_for(simulation.freeze(), sphere);
    const RenderSettings settings{160, 160, 32, 101};
    const auto image = PathTracer(make_snapshot(scene)).render(settings);
    image.write_exr(output / (name + ".exr"));
    display_image(image).write_png(output / (name + ".png"));
    const auto statistics = simulation.statistics();
    std::cout << name << " time_s=" << statistics.time_seconds
              << " max_spring_ratio=" << statistics.maximum_spring_ratio
              << " kinetic_J=" << statistics.kinetic_energy
              << " elastic_J=" << statistics.elastic_energy << '\n';
    if (save) {
        const auto path = output / std::filesystem::path(u8"布料 冻结.json");
        save_scene(scene, path);
        const auto reopened = load_scene(path);
        const auto check = PathTracer(make_snapshot(reopened)).render(settings);
        double maximum = 0;
        for (std::size_t y = 0; y < settings.height; ++y) {
            for (std::size_t x = 0; x < settings.width; ++x) {
                maximum = std::max(maximum, double(glm::length(image.at(x, y) - check.at(x, y))));
            }
        }
        std::cout << name << " roundtrip_rgb_max=" << maximum << '\n';
        if (maximum >= 1e-4) {
            throw std::runtime_error("Frozen cloth scene changed after save and reload");
        }
    }
}

} // namespace

// 固定时间步模拟落球和悬挂迎风布料，冻结后经普通 SceneDocument 保存并离线渲染。
int main() {
    try {
        const auto output = std::filesystem::current_path() / "outputs/a6.1";
        std::filesystem::create_directories(output / "drape");
        std::filesystem::create_directories(output / "pinned");
        ClothEnvironment environment;
        environment.planes.push_back({});
        environment.spheres.push_back({});
        ClothSimulation drape({}, {}, environment);
        std::cout << "vertices=" << drape.state().positions.size()
                  << " springs=" << drape.springs().size()
                  << " triangles=" << drape.triangles().size()
                  << " minimum_substeps=" << drape.minimum_substeps()
                  << " selected_substeps=" << drape.parameters().substeps << '\n';
        for (const auto frame : {0U, 18U, 45U, 90U}) {
            const auto start = std::chrono::steady_clock::now();
            const auto report =
                drape.advance(frame - static_cast<std::uint32_t>(drape.state().frame));
            const double milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            std::cout << "frame=" << frame << " contacts=" << report.contacts
                      << " contact_limits=" << report.contact_limits
                      << " simulation_ms=" << milliseconds << '\n';
            if (report.contact_limits) {
                throw std::runtime_error("Cloth example exhausted the collision event budget");
            }
            render_frame(drape, true, output / "drape", "frame-" + std::to_string(frame),
                         frame == 90);
        }

        ClothGrid grid;
        grid.origin = {-1, 2.2F, .3F};
        grid.span_v = {0, -1.7F, 0};
        grid.pinned = {0, 24};
        environment.spheres.clear();
        environment.wind_velocity = {0, 0, 1.5};
        ClothSimulation pinned(grid, {}, environment);
        const auto report = pinned.advance(90);
        std::cout << "pinned contacts=" << report.contacts
                  << " contact_limits=" << report.contact_limits << '\n';
        if (report.contact_limits) {
            throw std::runtime_error("Pinned cloth exhausted the collision event budget");
        }
        render_frame(pinned, false, output / "pinned", "wind", true);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Cloth example failed: " << error.what() << '\n';
        return 1;
    }
}
