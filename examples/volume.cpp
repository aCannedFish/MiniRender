#include <minirender/assets/scene_io.hpp>
#include <minirender/render/render_session.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>

using namespace minirender;
namespace {

// 与桌面共用后台任务和样本统计；例程只负责场景与文件，不另写积分器。
std::shared_ptr<const Image> render(const SceneDocument& scene, const std::string& label,
                                    RenderSettings settings = {160, 160, 128, 101}) {
    RenderSession session;
    session.start(make_snapshot(scene), settings, {.maximum_depth = 12});
    while (!session.wait_for(std::chrono::milliseconds(1000))) {
        const auto progress = session.progress();
        std::cout << label << " samples=" << progress.recorded_samples << '/'
                  << progress.total_samples << '\n'
                  << std::flush;
    }
    session.wait();
    const auto progress = session.progress();
    if (progress.state != RenderState::completed) {
        throw std::runtime_error("Volume render failed: " + progress.error);
    }
    std::cout << label << " paths=" << progress.statistics.paths
              << " medium_events=" << progress.statistics.medium_scattering_events
              << " shadow_rays=" << progress.statistics.shadow_rays
              << " seconds=" << progress.elapsed_seconds << '\n'
              << std::flush;
    return session.result();
}
MediumRegion medium(glm::dvec3 minimum, glm::dvec3 maximum, glm::dvec3 absorption,
                    glm::dvec3 scattering, double g) {
    MediumRegion region;
    region.id = "fog";
    region.name = "均匀体积";
    region.minimum = minimum;
    region.maximum = maximum;
    region.medium = {absorption, scattering, g};
    return region;
}
SceneDocument bounded_fog() {
    auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    scene.media.push_back(medium({-2, .01, -2}, {2, 2.8, 2}, {.06, .12, .18}, {.3, .3, .3}, .6));
    return scene;
}
SceneDocument cornell_fog() {
    auto scene =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-box.json");
    scene.media.push_back(
        medium({-.99, .01, -.99}, {.99, 1.99, .99}, {.08, .10, .13}, {.4, .4, .4}, .25));
    return scene;
}

// 四块顶板围成透光孔。方向光穿过孔后在参与介质中形成有遮挡的平行光束。
SceneDocument light_beam() {
    auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    std::erase_if(scene.objects, [](const auto& object) { return object.id != "ground"; });
    scene.environment = glm::vec3(.001F);
    scene.directional_lights = {{"sun", {.2F, -1, .3F}, {1, .93F, .8F}, 12}};
    scene.camera.position = {3, 1.8F, 4.5F};
    scene.camera.target = {0, 1.2F, 0};
    auto mesh = std::make_shared<MeshData>();
    const auto quad = [&](float x0, float x1, float z0, float z1) {
        const auto start = std::uint32_t(mesh->positions.size());
        mesh->positions.insert(mesh->positions.end(),
                               {{x0, 2.6F, z0}, {x0, 2.6F, z1}, {x1, 2.6F, z1}, {x1, 2.6F, z0}});
        for (const auto indices : {std::array{0U, 1U, 2U}, std::array{0U, 2U, 3U}}) {
            MeshTriangle triangle;
            for (std::size_t i = 0; i < 3; ++i) {
                triangle.corners[i] = {start + indices[i], {0, 1, 0}, {}};
            }
            mesh->triangles.push_back(triangle);
        }
    };
    quad(-2, -.35F, -2, 2);
    quad(.35F, 2, -2, 2);
    quad(-.35F, .35F, -2, -.35F);
    quad(-.35F, .35F, .35F, 2);
    Material ceiling;
    ceiling.id = "aperture";
    ceiling.base_color = glm::vec3(.08F);
    scene.materials.push_back(ceiling);
    scene.meshes.push_back({"aperture", {}, mesh});
    scene.objects.push_back({"aperture", "透光顶板", "aperture", "aperture", {}});
    scene.media.push_back(
        medium({-2, .01, -2}, {2, 2.5, 2}, {.05, .08, .13}, {.55, .55, .55}, .15));
    return scene;
}

void write_example(const SceneDocument& scene, const std::filesystem::path& output,
                   const std::string& name) {
    const auto file = output / (name + ".json");
    save_scene(scene, file);
    const auto image = render(scene, name);
    image->write_exr(output / (name + ".exr"));
    display_image(*image).write_png(output / (name + ".png"));
    const auto reopened = load_scene(file);
    const auto repeated = render(reopened, name + "-reload");
    double maximum = 0;
    for (std::size_t y = 0; y < image->height(); ++y) {
        for (std::size_t x = 0; x < image->width(); ++x) {
            maximum = std::max(maximum, double(glm::length(image->at(x, y) - repeated->at(x, y))));
        }
    }
    std::cout << name << " roundtrip_rgb_max=" << maximum << '\n' << std::flush;
    if (maximum > 1e-6) {
        throw std::runtime_error("Volume image changed after scene reload");
    }
}

// 独立高样本图固定为参考；三个种子分别记录误差，避免用某一张噪声图判断收敛。
void convergence(const std::filesystem::path& output) {
    const auto scene = bounded_fog();
    const auto reference = render(scene, "reference", {32, 32, 4096, 909});
    std::ofstream csv(output / "convergence.csv");
    csv.imbue(std::locale::classic());
    csv << "spp,seed,linear_rgb_rmse\n";
    double previous = std::numeric_limits<double>::infinity();
    for (const auto spp : {16U, 64U, 256U}) {
        double average = 0;
        for (const auto seed : {17ULL, 77ULL, 317ULL}) {
            const auto image = render(scene, "convergence", {32, 32, spp, seed});
            double squared_error = 0;
            for (std::size_t y = 0; y < 32; ++y) {
                for (std::size_t x = 0; x < 32; ++x) {
                    const auto delta =
                        glm::dvec3(image->at(x, y)) - glm::dvec3(reference->at(x, y));
                    squared_error += glm::dot(delta, delta);
                }
            }
            const double error = std::sqrt(squared_error / (32 * 32 * 3));
            csv << spp << ',' << seed << ',' << error << '\n';
            average += error / 3;
        }
        std::cout << "convergence spp=" << spp << " mean_rmse=" << average << '\n' << std::flush;
        if (average >= previous) {
            throw std::runtime_error("Volume average image error did not decrease");
        }
        previous = average;
    }
}
} // namespace

int main() {
    try {
        const auto output = std::filesystem::current_path() / "outputs/a7.1";
        std::filesystem::create_directories(output);
        auto clear = bounded_fog();
        clear.media.clear();
        write_example(clear, output, "clear");
        write_example(bounded_fog(), output, "bounded-fog");
        write_example(cornell_fog(), output, "cornell-fog");
        write_example(light_beam(), output, "light-beam");
        convergence(output);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
