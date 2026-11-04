#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/displacement.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/render/path_tracer.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace minirender;

namespace {

// 像素中心上的周期高度；每个纹理图集边界处回到 midlevel，允许 cube 的 UV 接缝。
Image make_height() {
    Image image(64, 64);
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            const double u = (static_cast<double>(x) + 0.5) / static_cast<double>(image.width());
            const double v =
                1 - (static_cast<double>(y) + 0.5) / static_cast<double>(image.height());
            const double h = 0.5 + 0.35 * std::sin(2 * std::numbers::pi * u) *
                                       std::sin(2 * std::numbers::pi * v);
            image.at(x, y) = glm::vec3(static_cast<float>(h));
        }
    }
    return image;
}

Image render(const SceneDocument& scene, const std::filesystem::path& output,
             const std::string& label) {
    RenderSettings settings;
    settings.width = 128;
    settings.height = 128;
    settings.samples_per_pixel = 64;
    settings.seed = 101;
    const PathTracer tracer(make_snapshot(scene));
    auto image = tracer.render(settings);
    image.write_exr(output / (label + ".exr"));
    display_image(image).write_png(output / (label + ".png"));
    return image;
}

// 通过独立的 BVH 可见性查询计数轮廓与地面阴影变化，不用噪声图像的像素差猜测遮挡。
void compare_visibility(std::shared_ptr<const SceneSnapshot> before,
                        std::shared_ptr<const SceneSnapshot> after) {
    if (after->directional_lights.empty()) {
        throw std::runtime_error("The visibility example requires a directional light");
    }
    const SceneBvh first(before), second(after);
    const auto light = -glm::normalize(glm::dvec3(after->directional_lights[0].direction));
    std::size_t silhouettes = 0, shadows = 0;
    for (std::size_t y = 0; y < 128; ++y) {
        for (std::size_t x = 0; x < 128; ++x) {
            const auto ray = after->camera.generate_ray(
                {(static_cast<double>(x) + 0.5) / 128, (static_cast<double>(y) + 0.5) / 128}, 1);
            const auto a = first.intersect(ray), b = second.intersect(ray);
            if (a.has_value() != b.has_value() ||
                (a && b && a->instance_index != b->instance_index)) {
                ++silhouettes;
            }
            if (a && b && before->instances[a->instance_index].object_id == "ground" &&
                after->instances[b->instance_index].object_id == "ground") {
                shadows +=
                    first.occluded(spawn_ray(*a, light)) != second.occluded(spawn_ray(*b, light));
            }
        }
    }
    if (silhouettes == 0 || shadows == 0) {
        throw std::runtime_error("Displacement did not change the example silhouette and shadow");
    }
    std::cout << "silhouette_pixels_changed=" << silhouettes
              << " shadow_samples_changed=" << shadows << '\n';
}

} // namespace

// 完整 CPU 链路：Loop -> 真实位移 -> 新 OBJ/场景 -> 重新加载 -> 同种子路径追踪。
// 输入 studio 与原 OBJ 均不改写，输出位于当前工作目录的 outputs/a4.2a。
int main() {
    try {
        const auto output = std::filesystem::current_path() / "outputs/a4.2a";
        std::filesystem::create_directories(output);
        auto scene =
            load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
        const auto cube = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                                       [](const MeshAsset& mesh) { return mesh.id == "cube"; });
        if (cube == scene.meshes.end()) {
            throw std::runtime_error("Studio scene is missing the cube mesh");
        }
        cube->data = subdivide_loop(cube->data, {3}).mesh;
        cube->source.clear();
        const auto baseline = make_snapshot(scene);
        render(scene, output, "before");
        const auto height_image = make_height();
        height_image.write_exr(output / "height.exr");
        const auto height = std::make_shared<const Texture>(height_image);
        scene.textures.push_back({"height", output / "height.exr", ColorEncoding::linear, height});
        const auto result = displace_mesh(cube->data, height, {0.15});
        cube->data = result.mesh;
        compare_visibility(baseline, make_snapshot(scene));
        const auto after = render(scene, output, "displaced");
        save_obj(*result.mesh, output / "displaced.obj");
        save_scene(scene, output / "scene.json");
        const auto reopened = render(load_scene(output / "scene.json"), output, "reopened");
        double error = 0;
        for (std::size_t y = 0; y < after.height(); ++y) {
            for (std::size_t x = 0; x < after.width(); ++x) {
                error = std::max(
                    error, glm::length(glm::dvec3(after.at(x, y)) - glm::dvec3(reopened.at(x, y))));
            }
        }
        if (error > 1e-5) {
            throw std::runtime_error("Reloaded displacement image differs from the in-memory mesh");
        }
        std::cout << "vertices=" << result.mesh->positions.size()
                  << " faces=" << result.mesh->triangles.size()
                  << " maximum_displacement=" << result.maximum_displacement
                  << " seam_edges=" << result.seam_edges
                  << " seam_segments=" << result.seam_segments
                  << " seam_difference=" << result.maximum_seam_difference
                  << " reload_max_rgb_error=" << error << '\n';
        std::cout << "Output: " << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Displacement example failed: " << error.what() << '\n';
        return 1;
    }
}
