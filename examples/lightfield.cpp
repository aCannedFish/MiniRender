#include <minirender/assets/scene_io.hpp>
#include <minirender/render/lightfield_capture.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numbers>

using namespace minirender;
namespace {

// 两个等投影尺寸的棋盘，物距分别为2m和6m；正向平行光提供一致的纹理照明。
SceneDocument scene(const std::filesystem::path& directory) {
    Image pattern(64, 64);
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            pattern.at(x, y) = glm::vec3(((x / 8 + y / 8) % 2 == 0) ? .9F : .025F);
        }
    }
    const auto texture_path = directory / "checker.exr";
    pattern.write_exr(texture_path);
    SceneDocument result;
    result.camera.position = {0, 1, 5};
    result.camera.target = {0, 1, 0};
    result.camera.focus_distance = 4;
    result.camera.aperture_radius = .2F;
    result.environment = glm::vec3(0);
    result.directional_lights = {{"sun", {0, 0, -1}, {1, 1, 1}, float(std::numbers::pi)}};
    result.textures.push_back({"checker", texture_path, ColorEncoding::linear,
                               std::make_shared<const Texture>(std::move(pattern))});
    auto mesh = std::make_shared<MeshData>();
    mesh->positions = {{-.5F, -.5F, 0}, {.5F, -.5F, 0}, {.5F, .5F, 0}, {-.5F, .5F, 0}};
    const std::array<glm::vec2, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (const auto indices : {std::array{0U, 1U, 2U}, std::array{0U, 2U, 3U}}) {
        MeshTriangle triangle;
        for (std::size_t c = 0; c < 3; ++c) {
            triangle.corners[c] = {indices[c], {0, 0, 1}, uv[indices[c]]};
        }
        mesh->triangles.push_back(triangle);
    }
    result.meshes.push_back({"card", {}, mesh});
    for (int i = 0; i < 2; ++i) {
        const std::string id = i == 0 ? "near" : "far";
        Material material;
        material.id = id;
        material.base_color = i == 0 ? glm::vec3(.9F, .15F, .1F) : glm::vec3(.12F, .35F, .9F);
        material.base_color_texture = "checker";
        material.texture_filter = TextureFilter::nearest;
        result.materials.push_back(material);
        result.objects.push_back({id,
                                  id,
                                  "card",
                                  id,
                                  {i == 0 ? glm::vec3(-.36F, 1, 3) : glm::vec3(1.08F, 1, -1),
                                   {},
                                   i == 0 ? glm::vec3(.6F, .9F, 1) : glm::vec3(1.8F, 2.7F, 1)}});
    }
    return result;
}

double sharpness(const Image& image, std::size_t x0, std::size_t x1, int channel) {
    double total = 0;
    // 卡片边框和孔径边界之外的内部区域，梯度只反映棋盘细节。
    for (std::size_t y = 40; y < 88; ++y) {
        for (std::size_t x = x0; x < x1; ++x) {
            total += std::abs(double(image.at(x + 1, y)[channel] - image.at(x, y)[channel]));
            total += std::abs(double(image.at(x, y + 1)[channel] - image.at(x, y)[channel]));
        }
    }
    return total;
}
void output(const LightFieldFile& file, const LightFieldReconstruction& result,
            const std::filesystem::path& directory, const std::string& name) {
    result.image.write_exr(directory / (name + ".exr"));
    display_image(result.image).write_png(directory / (name + ".png"));
    std::cout << name << " views=" << result.selected_views
              << " stored_paths=" << file.traced_paths()
              << " new_rays=0 working_bytes=" << result.estimated_bytes << '\n';
}
} // namespace

int main() {
    try {
        const auto directory = std::filesystem::current_path() / "outputs/a8.1";
        std::filesystem::create_directories(directory);
        const auto document = scene(directory);
        save_scene(document, directory / "scene.json");
        LightFieldCaptureSettings settings;
        settings.render = {128, 128, 32, 101};
        settings.aperture_radius = .2;
        settings.reference_focus = 4;
        settings.threads = 4;
        LightFieldCaptureSession capture;
        capture.start(make_snapshot(document), settings, {SamplingMode::mis, 1, false, 3});
        while (!capture.wait_for(std::chrono::milliseconds(1000))) {
            const auto p = capture.progress();
            std::cout << "capture views=" << p.completed_views << '/' << p.total_views
                      << " samples=" << p.recorded_samples << '/' << p.total_samples << '\n'
                      << std::flush;
        }
        capture.wait();
        const auto p = capture.progress();
        if (p.state != RenderState::completed) {
            throw std::runtime_error(p.error);
        }
        const auto file = capture.result();
        file->save(directory / "capture.mrlf");
        LightFieldReader reader(file);
        const auto near = refocus_lightfield(reader, {2, .2});
        const auto far = refocus_lightfield(reader, {6, .2});
        const auto original = refocus_lightfield(reader, {4, .2});
        const auto pinhole = refocus_lightfield(reader, {2, 0});
        output(*file, near, directory, "near");
        output(*file, far, directory, "far");
        output(*file, original, directory, "reference");
        output(*file, pinhole, directory, "pinhole");
        const double near_ratio =
            sharpness(near.image, 24, 51, 0) / sharpness(far.image, 24, 51, 0);
        const double far_ratio =
            sharpness(far.image, 77, 104, 2) / sharpness(near.image, 77, 104, 2);
        std::cout << "near_sharpness_ratio=" << near_ratio << " far_sharpness_ratio=" << far_ratio
                  << " cache_peak=" << reader.statistics().peak_cached_bytes
                  << " capture_bytes=" << p.estimated_bytes << " payload_bytes=" << p.payload_bytes
                  << " paths=" << p.recorded_samples << '\n';
        if (near_ratio < 2 || far_ratio < 2) {
            throw std::runtime_error("Light field demo did not restore both depth planes");
        }
        LightFieldReader reopened(LightFieldFile::open(directory / "capture.mrlf"), 0);
        const auto repeated = refocus_lightfield(reopened, {2, .2});
        double maximum = 0;
        for (std::size_t y = 0; y < 128; ++y) {
            for (std::size_t x = 0; x < 128; ++x) {
                maximum = std::max(
                    maximum, double(glm::length(repeated.image.at(x, y) - near.image.at(x, y))));
            }
        }
        std::cout << "reload_maximum_rgb=" << maximum << '\n';
        if (maximum != 0 || capture.progress().recorded_samples != p.recorded_samples) {
            throw std::runtime_error("Light field changed after reloading or refocusing");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
