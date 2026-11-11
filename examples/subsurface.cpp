#include <minirender/assets/scene_io.hpp>
#include <minirender/render/render_session.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>
#include <numbers>

using namespace minirender;
namespace {

// 复用场景、后台积分器和图像输出，例程不另设一套 SSS 实现。
std::shared_ptr<const Image> render(const SceneDocument& scene, const std::string& label,
                                    std::uint32_t samples) {
    RenderSession task;
    task.start(make_snapshot(scene), {128, 128, samples, 101}, {.maximum_depth = 8},
               {.threads = 4});
    while (!task.wait_for(std::chrono::milliseconds(1000))) {
        const auto progress = task.progress();
        std::cout << label << " samples=" << progress.recorded_samples << '/'
                  << progress.total_samples << '\n'
                  << std::flush;
    }
    task.wait();
    const auto progress = task.progress();
    if (progress.state != RenderState::completed) {
        throw std::runtime_error("Subsurface example failed: " + progress.error);
    }
    const auto& statistics = progress.statistics;
    if (statistics.subsurface_events == 0) {
        throw std::runtime_error("Subsurface example recorded no surface jumps");
    }
    std::cout << label << " paths=" << statistics.paths
              << " SSS_events=" << statistics.subsurface_events << " mean_jump_m="
              << statistics.subsurface_distance / double(statistics.subsurface_events)
              << " seconds=" << progress.elapsed_seconds << '\n'
              << std::flush;
    return task.result();
}

SceneDocument material_scene(const std::string& preset_name, double density) {
    auto scene =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-materials.json");
    // 将房间缩为 10cm，球直径为 4cm，和测量系数的毫米扩散尺度相符。
    constexpr float scale = .05F;
    for (auto& object : scene.objects) {
        object.transform.translation *= scale;
        object.transform.scale *= scale;
    }
    scene.camera.position *= scale;
    scene.camera.target *= scale;
    scene.camera.near_plane *= scale;
    scene.camera.far_plane *= scale;
    scene.camera.focus_distance *= scale;
    const auto preset = make_subsurface_preset(preset_name);
    auto parameters = *preset.parameters;
    parameters.density = density;
    for (auto& material : scene.materials) {
        if (material.id == "mat_glass_ball") {
            material.name = preset_name;
            material.kind = MaterialKind::subsurface;
            material.ior = float(preset.ior);
            material.subsurface = std::make_shared<const SubsurfaceParameters>(parameters);
        }
    }
    return scene;
}

// 顶部方向光照亮 4cm 方块，正面没有直接入射，观察光在上沿向侧面扩散。
SceneDocument edge_scene(const std::string& preset, double density) {
    auto scene = material_scene(preset, density);
    auto material =
        *std::find_if(scene.materials.begin(), scene.materials.end(),
                      [](const auto& value) { return value.kind == MaterialKind::subsurface; });
    material.id = "block";
    Material floor;
    floor.id = "floor";
    floor.base_color = glm::vec3(.12F);
    floor.double_sided = false;
    scene.materials = {material, floor};
    scene.objects = {{"block", "SSS block", "cube", "block", {{}, {}, {.04F, .04F, .04F}}},
                     {"floor", "Floor", "plane", "floor", {{0, -.0201F, 0}, {}, {.1F, 1, .1F}}}};
    scene.camera.position = {.065F, .045F, .12F};
    scene.camera.target = {};
    scene.camera.vertical_fov_degrees = 35;
    scene.camera.near_plane = .001F;
    scene.camera.far_plane = 2;
    scene.environment = glm::vec3(.01F);
    scene.directional_lights = {{"sun", {0, -1, 0}, {1, .95F, .9F}, 5}};
    return scene;
}

void example(const std::filesystem::path& directory, const std::string& name,
             const SceneDocument& scene, std::uint32_t samples = 128) {
    const auto file = directory / (name + ".json");
    save_scene(scene, file);
    const auto image = render(scene, name, samples);
    image->write_exr(directory / (name + ".exr"));
    display_image(*image).write_png(directory / (name + ".png"));
    const auto repeated = render(load_scene(file), name + "-reload", samples);
    double maximum = 0;
    for (std::size_t y = 0; y < image->height(); ++y) {
        for (std::size_t x = 0; x < image->width(); ++x) {
            maximum = std::max(maximum, double(glm::length(image->at(x, y) - repeated->at(x, y))));
        }
    }
    std::cout << name << " reload_maximum_rgb=" << maximum << '\n' << std::flush;
    if (maximum > 1e-6) {
        throw std::runtime_error("Subsurface example changed after scene IO");
    }
}

// 用正面无直接入射、左侧受方向光照射的立方体量化跨位置扩散的收敛。
void convergence(const std::filesystem::path& directory) {
    auto scene =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-materials.json");
    std::erase_if(scene.meshes, [](const auto& mesh) { return mesh.id != "cube"; });
    Material material;
    material.id = "block";
    material.kind = MaterialKind::subsurface;
    material.ior = 1.3F;
    material.subsurface = std::make_shared<const SubsurfaceParameters>(
        SubsurfaceParameters{{.1, .2, .3}, {1, 2, 3}, 1});
    scene.materials = {material};
    scene.objects = {{"block", "Block", "cube", "block", {}}};
    scene.environment = glm::vec3(0);
    scene.directional_lights = {{"side", {1, 0, 0}, {1, .8F, .6F}, 4}};
    const DipoleProfile profile(*material.subsurface, material.ior);
    glm::dvec3 reference(0);
    constexpr int grid = 512;
    for (int y = 0; y < grid; ++y) {
        for (int z = 0; z < grid; ++z) {
            const glm::dvec3 point{-.5, (y + .5) / grid - .5, (z + .5) / grid - .5};
            reference +=
                profile.evaluate(glm::length(point - glm::dvec3(0, 0, .5))) / double(grid * grid);
        }
    }
    const double f0 = std::pow((material.ior - 1.) / (material.ior + 1.), 2);
    reference *= (1 - f0) * (1 - f0) / std::numbers::pi * 4 * glm::dvec3(1, .8F, .6F);
    const PathTracer tracer(make_snapshot(scene), {SamplingMode::mis, 1, false, 3});
    std::ofstream csv(directory / "convergence.csv");
    csv.imbue(std::locale::classic());
    csv << "samples,seed,linear_rgb_rmse\n";
    for (const std::uint64_t count : {1000ULL, 4000ULL, 16000ULL, 64000ULL}) {
        double mse = 0;
        for (const std::uint64_t seed : {17ULL, 77ULL, 317ULL, 517ULL, 717ULL}) {
            glm::dvec3 sum(0);
            for (std::uint64_t i = 0; i < count; ++i) {
                auto random = Sampler::for_sample(seed, 0, i);
                sum += tracer.trace({{0, 0, 3}, {0, 0, -1}}, random);
            }
            const auto error = sum / double(count) - reference;
            const double squared = glm::dot(error, error) / 3;
            csv << count << ',' << seed << ',' << std::sqrt(squared) << '\n';
            mse += squared / 5;
        }
        std::cout << "SSS convergence samples=" << count << " five_seed_rmse=" << std::sqrt(mse)
                  << " reference=" << reference.x << ',' << reference.y << ',' << reference.z
                  << '\n'
                  << std::flush;
    }
}
} // namespace

int main() {
    try {
        const auto output = std::filesystem::current_path() / "outputs/a7.5";
        std::filesystem::create_directories(output);
        example(output, "marble", material_scene("marble", 1));
        example(output, "wax", material_scene("wax", .5));
        example(output, "wax-dense", material_scene("wax", 4));
        example(output, "marble-edge", edge_scene("marble", 1), 256);
        example(output, "wax-edge", edge_scene("wax", .5), 256);
        convergence(output);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
