#include <minirender/assets/scene_io.hpp>
#include <minirender/render/render_session.hpp>
#include <minirender/simulation/cloth.hpp>

#include <algorithm>
#include <iostream>

using namespace minirender;

namespace {

// 例程复用与桌面相同的多线程任务，按真实累计样本报告进度。
std::shared_ptr<const Image> render(const SceneDocument& document) {
    RenderSession session;
    session.start(make_snapshot(document), {160, 160, 64, 101}, {.maximum_depth = 4});
    while (!session.wait_for(std::chrono::milliseconds(1000))) {
        const auto progress = session.progress();
        std::cout << "samples=" << progress.recorded_samples << '/' << progress.total_samples
                  << '\n'
                  << std::flush;
    }
    session.wait();
    const auto progress = session.progress();
    if (progress.state != RenderState::completed) {
        throw std::runtime_error("Cloth material render failed: " + progress.error);
    }
    return session.result();
}

SceneDocument cloth_scene() {
    ClothEnvironment environment;
    environment.planes.push_back({});
    environment.spheres.push_back({});
    ClothSimulation simulation({}, {}, environment);
    simulation.advance(90);
    const auto cloth = simulation.freeze();
    auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    std::erase_if(scene.objects, [](const auto& object) { return object.id != "ground"; });
    Material material;
    material.id = "fabric";
    material.name = "微圆柱布料";
    material.kind = MaterialKind::cloth;
    material.base_color = glm::vec3(1);
    const auto preset = make_cloth_preset("linen");
    material.ior = static_cast<float>(preset.ior);
    material.cloth = preset.appearance;
    scene.materials.push_back(material);
    scene.meshes.push_back({"cloth", {}, cloth.mesh});
    scene.objects.push_back({"cloth", "冻结布料", "cloth", "fabric", cloth.transform});
    scene.camera.position = {2.5F, 2.1F, 3.1F};
    scene.camera.target = {0, .7F, 0};
    return scene;
}

} // namespace

// 四种原文预置分别保存普通场景文件，以固定种子渲染，并检查材质保存重开的结果。
int main() {
    try {
        const auto output = std::filesystem::current_path() / "outputs/a6.2";
        std::filesystem::create_directories(output);
        auto scene = cloth_scene();
        for (const auto* name : {"linen", "silk", "satin", "shot"}) {
            const auto preset = make_cloth_preset(name);
            auto& material = scene.materials.back();
            material.ior = static_cast<float>(preset.ior);
            material.cloth = preset.appearance;
            const auto file = output / (std::string(name) + ".json");
            save_scene(scene, file);
            const auto image = render(scene);
            image->write_exr(output / (std::string(name) + ".exr"));
            display_image(*image).write_png(output / (std::string(name) + ".png"));
            const auto reopened = load_scene(file);
            const auto repeated = render(reopened);
            double maximum = 0;
            for (std::size_t y = 0; y < image->height(); ++y) {
                for (std::size_t x = 0; x < image->width(); ++x) {
                    maximum = std::max(maximum,
                                       double(glm::length(image->at(x, y) - repeated->at(x, y))));
                }
            }
            std::cout << name << " roundtrip_rgb_max=" << maximum << '\n' << std::flush;
            if (maximum >= 1e-4) {
                throw std::runtime_error("Cloth appearance changed after scene reload");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
