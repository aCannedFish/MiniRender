#include <minirender/assets/scene_io.hpp>
#include <minirender/render/render_session.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>

using namespace minirender;
namespace {
IntegratorSettings options(std::uint32_t photons = 40000) {
    IntegratorSettings settings;
    settings.kind = IntegratorKind::sppm;
    settings.maximum_depth = 8;
    settings.sppm.photons_per_iteration = photons;
    settings.sppm.initial_radius = .12;
    return settings;
}
// 使用同一后台接口报告相机、哈希、光子与统计阶段，主线程不执行路径循环。
std::shared_ptr<const Image> render(const SceneDocument& scene, RenderSettings film,
                                    IntegratorSettings settings, const std::string& label) {
    RenderSession task;
    task.start(make_snapshot(scene), film, settings, {4, 8, 4});
    while (!task.wait_for(std::chrono::milliseconds(1000))) {
        const auto p = task.progress();
        std::cout << label << ' ' << render_state_name(p.state) << ' ' << p.recorded_samples << '/'
                  << p.total_samples;
        if (settings.kind == IntegratorKind::sppm) {
            std::cout << ' ' << sppm_phase_name(p.sppm_progress.phase) << ' '
                      << p.sppm_progress.completed << '/' << p.sppm_progress.total;
        }
        std::cout << '\n' << std::flush;
    }
    task.wait();
    const auto p = task.progress();
    if (p.state != RenderState::completed) {
        throw std::runtime_error(p.error);
    }
    std::cout << label << " paths=" << p.statistics.paths;
    if (settings.kind == IntegratorKind::sppm) {
        std::cout << " iterations=" << p.sppm.iterations
                  << " photon_trials=" << p.sppm.emitted_photons
                  << " gathered=" << p.sppm.gathered_contributions
                  << " radius=" << p.sppm.mean_radius
                  << " caustic=" << p.sppm.specular_light_contributions
                  << " SDS=" << p.sppm.sds_contributions;
    }
    std::cout << " seconds=" << p.elapsed_seconds << '\n' << std::flush;
    return task.result();
}
double error(const Image& a, const Image& b) {
    double sum = 0;
    for (std::size_t y = 0; y < a.height(); ++y) {
        for (std::size_t x = 0; x < a.width(); ++x) {
            const auto d = glm::dvec3(a.at(x, y)) - glm::dvec3(b.at(x, y));
            sum += glm::dot(d, d);
        }
    }
    return std::sqrt(sum / double(3 * a.width() * a.height()));
}
void example(const SceneDocument& scene, const std::filesystem::path& output,
             const std::string& name) {
    const auto file = output / (name + ".json");
    save_scene(scene, file);
    const auto image = render(scene, {128, 128, 128, 101}, options(), name);
    image->write_exr(output / (name + ".exr"));
    display_image(*image).write_png(output / (name + ".png"));
    const auto reopened =
        render(load_scene(file), {128, 128, 128, 101}, options(), name + "-reload");
    const double rms = error(*image, *reopened);
    std::cout << name << " roundtrip_rmse=" << rms << '\n' << std::flush;
    if (rms > 1e-6) {
        throw std::runtime_error("SPPM changed after scene reload");
    }
}
} // namespace

int main() {
    try {
        const auto root = std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes";
        const auto output = std::filesystem::current_path() / "outputs/a7.3";
        std::filesystem::create_directories(output);
        auto glass = load_scene(root / "cornell-materials.json");
        example(glass, output, "glass-caustic");
        for (auto& material : glass.materials) {
            if (material.id == "mat_back") {
                material.kind = MaterialKind::mirror;
                material.base_color = glm::vec3(.95F);
            }
        }
        glass.camera.position = {-.43F, 1.5F, 2.4F};
        glass.camera.target = {-.43F, .45F, -1};
        glass.camera.vertical_fov_degrees = 35;
        example(glass, output, "mirror-caustic");
        const auto diffuse = load_scene(root / "cornell-box.json");
        IntegratorSettings pt;
        pt.maximum_depth = 8;
        const auto reference = render(diffuse, {32, 32, 4096, 909}, pt, "PT-reference");
        reference->write_exr(output / "pt-reference.exr");
        display_image(*reference).write_png(output / "pt-reference.png");
        std::ofstream csv(output / "convergence.csv");
        csv.imbue(std::locale::classic());
        csv << "iterations,seed,linear_rgb_rmse\n";
        for (const auto iterations : {32U, 128U, 512U}) {
            double mean = 0;
            for (const auto seed : {17ULL, 77ULL, 317ULL}) {
                const auto image =
                    render(diffuse, {32, 32, iterations, seed}, options(10000), "convergence");
                const double rms = error(*image, *reference);
                csv << iterations << ',' << seed << ',' << rms << '\n';
                mean += rms / 3;
            }
            std::cout << "convergence iterations=" << iterations << " mean_rmse=" << mean << '\n'
                      << std::flush;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
